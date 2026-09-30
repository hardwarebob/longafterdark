// Live sinks (AUDIO.md §6.6): the real device, for streamed runs only.
//
//   PCM:  a thread of its own drives WASAPI shared mode (event-driven,
//         AUTOCONVERTPCM, session "Long After Dark") from a single-producer /
//         single-consumer ring. Playback starts once the ring holds the
//         latency; an underrun plays silence (counted) and waits for the ring
//         to fill again; a ring above latency + 100 ms drops its oldest frames
//         back to the latency (counted). A lost device is reopened at most once
//         per second, dropping meanwhile. When WASAPI cannot start, waveOut on
//         WAVE_MAPPER (4 buffers of latency / 4); when that fails too, silence
//         and one log line.
//   MIDI: a thread of its own sends each message at its wall-clock deadline,
//         anchor_wall + (at - anchor_guest) + latency, through midiOut (the MIDI
//         mapper by default). Closing silences the synth: note-offs, CC123 on
//         all channels, midiOutReset. When no advance has come for latency +
//         500 ms (the saver sends no GO while the display is off; a stalled
//         step), the notes still sounding are released, so none drones on
//         while the emulation waits; the song goes on with its next events.
//
// The emulation thread never waits on either: writes go into the ring (or a
// briefly locked queue) and return.
#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cinttypes>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "adw/core/clock.h"
#include "adw/core/log.h"
#include "audio_internal.h"

namespace adw::audio {

// ---- the ring -------------------------------------------------------------------------

PcmRing::PcmRing(size_t capacity_frames) {
  cap_ = 1;
  while (cap_ < capacity_frames) cap_ <<= 1;
  mask_ = cap_ - 1;
  buf_.assign(cap_ * 2, 0);
}

size_t PcmRing::push(const int16_t* lr, size_t frames) {
  uint64_t w = w_.load(std::memory_order_relaxed);
  uint64_t r = r_.load(std::memory_order_acquire);
  size_t space = cap_ - size_t(w - r);
  size_t n = std::min(frames, space);
  for (size_t done = 0; done < n;) {
    size_t at = size_t((w + done) & mask_);
    size_t run = std::min(n - done, cap_ - at);
    memcpy(&buf_[at * 2], lr + done * 2, run * 4);
    done += run;
  }
  w_.store(w + n, std::memory_order_release);
  return frames - n;
}

size_t PcmRing::fill() const {
  uint64_t r = r_.load(std::memory_order_acquire);
  return size_t(w_.load(std::memory_order_acquire) - r);
}

size_t PcmRing::pop(int16_t* lr, size_t frames) {
  uint64_t r = r_.load(std::memory_order_relaxed);
  uint64_t w = w_.load(std::memory_order_acquire);
  size_t n = std::min(frames, size_t(w - r));
  for (size_t done = 0; done < n;) {
    size_t at = size_t((r + done) & mask_);
    size_t run = std::min(n - done, cap_ - at);
    memcpy(lr + done * 2, &buf_[at * 2], run * 4);
    done += run;
  }
  r_.store(r + n, std::memory_order_release);
  return n;
}

size_t PcmRing::skip(size_t frames) {
  uint64_t r = r_.load(std::memory_order_relaxed);
  uint64_t w = w_.load(std::memory_order_acquire);
  size_t n = std::min(frames, size_t(w - r));
  r_.store(r + n, std::memory_order_release);
  return n;
}

void PcmConsumer::fill(int16_t* dst, size_t frames) {
  if (!frames) return;
  size_t f = ring_.fill();
  if (f > target_ + slack_) {
    size_t d = ring_.skip(f - target_);
    dropped += d;
    f -= d;
  }
  if (prefill_) {
    if (f < target_ || f == 0) {
      memset(dst, 0, frames * 4);
      return;
    }
    prefill_ = false;
  }
  // Drift control: one frame a fill, toward the target.
  if (f > target_ + band_ && f > frames) {
    trimmed += ring_.skip(1);
  } else if (f + band_ < target_ && f >= frames && frames > 1) {
    size_t got = ring_.pop(dst, frames - 1);
    if (got == frames - 1) {
      memcpy(dst + (frames - 1) * 2, dst + (frames - 2) * 2, 4);  // the last frame again
      padded++;
      return;
    }
    memset(dst + got * 2, 0, (frames - got) * 4);
    underruns++;
    prefill_ = true;
    return;
  }
  size_t got = ring_.pop(dst, frames);
  if (got < frames) {
    memset(dst + got * 2, 0, (frames - got) * 4);
    underruns++;
    prefill_ = true;
  }
}

void PcmConsumer::drop_backlog() {
  size_t f = ring_.fill();
  if (f > target_) dropped += ring_.skip(f - target_);
}

namespace {

template <typename T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

// ---- live PCM ---------------------------------------------------------------------------
class LivePcm : public PcmSink {
 public:
  LivePcm(uint32_t rate, uint32_t latency_ms)
      : rate_(rate),
        target_(size_t(uint64_t(rate) * latency_ms / 1000)),
        slack_(size_t(rate / 10)),
        ring_(size_t(rate) * 2 + size_t(uint64_t(rate) * latency_ms / 1000)),
        consumer_(ring_, target_, slack_, size_t(rate / 50)) {
    stop_evt_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
  }
  ~LivePcm() override { close(); }

  void write(const int16_t* lr, size_t frames) override {
    if (dead_.load(std::memory_order_relaxed) || closed_) return;
    size_t lost = ring_.push(lr, frames);
    if (lost) dropped_.fetch_add(lost, std::memory_order_relaxed);
  }

  void close() override {
    if (closed_) return;
    closed_ = true;
    SetEvent(stop_evt_);
    if (thread_.joinable()) thread_.join();
    CloseHandle(stop_evt_);
    trace("audio", "live PCM closed: %" PRIu64 " underruns, %" PRIu64 " frames dropped", underruns(),
          dropped_frames());
  }

  uint64_t underruns() const override { return underruns_.load(std::memory_order_relaxed); }
  uint64_t dropped_frames() const override {
    return dropped_.load(std::memory_order_relaxed) + consumer_dropped_.load(std::memory_order_relaxed);
  }

 private:
  bool stopping(DWORD wait_ms) { return WaitForSingleObject(stop_evt_, wait_ms) == WAIT_OBJECT_0; }

  // The consumer's side of the ring: prefill, underrun, drift control (PcmConsumer).
  void fill(int16_t* dst, size_t frames) {
    consumer_.fill(dst, frames);
    publish();
  }

  // While no device plays: keep only the latest latency's worth.
  void drop_backlog() {
    consumer_.drop_backlog();
    publish();
  }

  // The consumer's counters, where the emulation thread reads them.
  void publish() {
    underruns_.store(consumer_.underruns, std::memory_order_relaxed);
    consumer_dropped_.store(consumer_.dropped, std::memory_order_relaxed);
  }

  WAVEFORMATEX format() const {
    WAVEFORMATEX w{};
    w.wFormatTag = WAVE_FORMAT_PCM;
    w.nChannels = 2;
    w.nSamplesPerSec = rate_;
    w.wBitsPerSample = 16;
    w.nBlockAlign = 4;
    w.nAvgBytesPerSec = rate_ * 4;
    return w;
  }

  HRESULT open_wasapi() {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator_));
    if (SUCCEEDED(hr)) hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (SUCCEEDED(hr))
      hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
    WAVEFORMATEX wfx = format();
    if (SUCCEEDED(hr))
      hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                   AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                               REFERENCE_TIME(30) * 10000, 0, &wfx, nullptr);
    if (SUCCEEDED(hr)) {
      if (!buffer_evt_) buffer_evt_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      hr = client_->SetEventHandle(buffer_evt_);
    }
    if (SUCCEEDED(hr)) hr = client_->GetBufferSize(&buffer_frames_);
    if (SUCCEEDED(hr)) hr = client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_));
    if (SUCCEEDED(hr)) {
      IAudioSessionControl* session = nullptr;
      if (SUCCEEDED(client_->GetService(__uuidof(IAudioSessionControl), reinterpret_cast<void**>(&session)))) {
        session->SetDisplayName(L"Long After Dark", nullptr);
        session->Release();
      }
      BYTE* data = nullptr;
      if (SUCCEEDED(render_->GetBuffer(buffer_frames_, &data)))
        render_->ReleaseBuffer(buffer_frames_, AUDCLNT_BUFFERFLAGS_SILENT);
      hr = client_->Start();
    }
    if (FAILED(hr)) close_wasapi();
    return hr;
  }

  void close_wasapi() {
    if (client_) client_->Stop();
    release(render_);
    release(client_);
    release(device_);
    release(enumerator_);
  }

  // One wakeup's worth. False = the device went away.
  bool feed_wasapi() {
    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (FAILED(hr)) return false;
    UINT32 n = buffer_frames_ - padding;
    if (n == 0) return true;
    BYTE* data = nullptr;
    hr = render_->GetBuffer(n, &data);
    if (FAILED(hr)) return false;
    fill(reinterpret_cast<int16_t*>(data), n);
    return SUCCEEDED(render_->ReleaseBuffer(n, 0));
  }

  bool run_waveout() {
    WAVEFORMATEX wfx = format();
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT wo = nullptr;
    MMRESULT mr = waveOutOpen(&wo, WAVE_MAPPER, &wfx, DWORD_PTR(evt), 0, CALLBACK_EVENT);
    if (mr != MMSYSERR_NOERROR) {
      CloseHandle(evt);
      waveout_error_ = mr;
      return false;
    }
    const size_t frames = std::max<size_t>(64, target_ / 4);
    std::vector<int16_t> mem(frames * 2 * 4);
    WAVEHDR hdr[4]{};
    for (int i = 0; i < 4; i++) {
      hdr[i].lpData = reinterpret_cast<LPSTR>(&mem[size_t(i) * frames * 2]);
      hdr[i].dwBufferLength = DWORD(frames * 4);
      waveOutPrepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
      hdr[i].dwFlags |= WHDR_DONE;
    }
    trace("audio", "live PCM: waveOut on WAVE_MAPPER, %u Hz, 4 x %zu frames", rate_, frames);
    while (!stopping(0)) {
      for (int i = 0; i < 4; i++) {
        if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
        fill(reinterpret_cast<int16_t*>(hdr[i].lpData), frames);
        hdr[i].dwFlags &= ~WHDR_DONE;
        waveOutWrite(wo, &hdr[i], sizeof(WAVEHDR));
      }
      HANDLE h[2] = {stop_evt_, evt};
      WaitForMultipleObjects(2, h, FALSE, 200);
    }
    waveOutReset(wo);
    for (int i = 0; i < 4; i++) waveOutUnprepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
    waveOutClose(wo);
    CloseHandle(evt);
    return true;
  }

  void run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = open_wasapi();
    if (FAILED(hr)) {
      if (!run_waveout()) {
        log("audio: no live sound output (WASAPI error 0x%08lx, waveOut error %u); playing silence",
            (unsigned long)hr, waveout_error_);
        dead_ = true;
      }
    } else {
      trace("audio", "live PCM: WASAPI shared, %u Hz 16-bit stereo, device buffer %u frames, ring target %zu frames",
            rate_, buffer_frames_, target_);
      for (;;) {
        HANDLE h[2] = {stop_evt_, buffer_evt_};
        DWORD w = WaitForMultipleObjects(2, h, FALSE, 200);
        if (w == WAIT_OBJECT_0) break;
        if (feed_wasapi()) continue;
        // The device went away (AUDCLNT_E_DEVICE_INVALIDATED or worse): try
        // the new default once a second, dropping meanwhile.
        trace("audio", "live PCM: device lost; reopening");
        close_wasapi();
        bool stop = false;
        for (;;) {
          if (stopping(1000)) {
            stop = true;
            break;
          }
          drop_backlog();
          if (SUCCEEDED(open_wasapi())) break;
        }
        if (stop) break;
        consumer_.reset();
        trace("audio", "live PCM: device reopened");
      }
      close_wasapi();
    }
    if (buffer_evt_) CloseHandle(buffer_evt_);
    buffer_evt_ = nullptr;
    if (SUCCEEDED(co)) CoUninitialize();
  }

  const uint32_t rate_;
  const size_t target_, slack_;
  PcmRing ring_;
  PcmConsumer consumer_;  // consumer (device) thread only
  std::thread thread_;
  HANDLE stop_evt_ = nullptr, buffer_evt_ = nullptr;
  bool closed_ = false;
  std::atomic<bool> dead_{false};
  // dropped_: the producer's overflows; the rest mirror consumer_'s counters.
  std::atomic<uint64_t> underruns_{0}, dropped_{0}, consumer_dropped_{0};
  MMRESULT waveout_error_ = 0;
  IMMDeviceEnumerator* enumerator_ = nullptr;
  IMMDevice* device_ = nullptr;
  IAudioClient* client_ = nullptr;
  IAudioRenderClient* render_ = nullptr;
  UINT32 buffer_frames_ = 0;
};

// ---- live MIDI --------------------------------------------------------------------------
class LiveMidi : public MidiSink {
 public:
  LiveMidi(int device, uint32_t latency_ms) : device_(device), latency_us_(int64_t(latency_ms) * 1000) {
    anchor_wall_ = int64_t(VirtualClock::system_wall_us());
    last_anchor_wall_ = anchor_wall_;
    evt_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
  }
  ~LiveMidi() override { close(0); }

  void send(Time at, const uint8_t* msg, size_t len) override {
    if (failed_.load(std::memory_order_relaxed) || closed_) return;
    Item it;
    it.deadline = anchor_wall_ + (int64_t(at) - int64_t(anchor_guest_)) + latency_us_;
    it.bytes.assign(msg, msg + len);
    {
      std::lock_guard<std::mutex> lock(mu_);
      q_.push_back(std::move(it));
    }
    SetEvent(evt_);
  }

  void anchor(Time t) override {
    anchor_guest_ = t;
    anchor_wall_ = int64_t(VirtualClock::system_wall_us());
    last_anchor_wall_.store(anchor_wall_, std::memory_order_relaxed);
  }

  void close(Time) override {
    if (closed_) return;
    closed_ = true;
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
    }
    SetEvent(evt_);
    if (thread_.joinable()) thread_.join();
    CloseHandle(evt_);
  }

 private:
  struct Item {
    int64_t deadline = 0;
    std::vector<uint8_t> bytes;
  };

  // A SysEx on its way out: the header and the bytes it points at belong to
  // the driver until it sets MHDR_DONE, so they live here (at stable
  // addresses) until then, and are freed only once midiOutUnprepareHeader
  // succeeds (MIDIERR_STILLPLAYING: the driver still reads them).
  struct LongMsg {
    MIDIHDR hdr{};
    std::vector<uint8_t> data;
  };

  // Frees the SysEx headers the driver is done with; with `force`, first
  // takes every one back (midiOutReset marks them done) — at close.
  void reap_long(bool force) {
    if (force && !long_.empty()) midiOutReset(hmo_);
    for (auto it = long_.begin(); it != long_.end();) {
      LongMsg& l = **it;
      if ((l.hdr.dwFlags & MHDR_DONE) && midiOutUnprepareHeader(hmo_, &l.hdr, sizeof(l.hdr)) == MMSYSERR_NOERROR) {
        it = long_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void out(const std::vector<uint8_t>& m) {
    if (m.empty()) return;
    reap_long(false);
    if (m[0] == 0xF7 && m.size() >= 2 && m.size() <= 4 && m[1] >= 0xF1 && m[1] != 0xF7) {
      // An escape holding one system common or real-time message (the raw
      // MIDI port's, AUDIO.md §6.4): a short message, with no header to wait on.
      DWORD packed = 0;
      for (size_t i = 1; i < m.size(); i++) packed |= DWORD(m[i]) << (8 * (i - 1));
      midiOutShortMsg(hmo_, packed);
      return;
    }
    if (m[0] == 0xF0 || m[0] == 0xF7) {
      // SysEx (F0 …) goes as is; an escape's raw bytes follow its F7.
      auto l = std::make_unique<LongMsg>();
      l->data.assign(m[0] == 0xF7 ? m.begin() + 1 : m.begin(), m.end());
      if (l->data.empty()) return;
      l->hdr.lpData = reinterpret_cast<LPSTR>(l->data.data());
      l->hdr.dwBufferLength = DWORD(l->data.size());
      if (midiOutPrepareHeader(hmo_, &l->hdr, sizeof(l->hdr)) != MMSYSERR_NOERROR) return;
      if (midiOutLongMsg(hmo_, &l->hdr, sizeof(l->hdr)) != MMSYSERR_NOERROR) {
        midiOutUnprepareHeader(hmo_, &l->hdr, sizeof(l->hdr));
        return;
      }
      // What follows waits for it (MIDI keeps its order), but only as long
      // as the bytes take on a 31,250-baud cable (320 us each) plus 20 ms: a
      // driver that is slower than that no longer holds every note up, and
      // the message stays in long_ until the driver hands it back.
      const DWORD wait_ms = DWORD(std::min<size_t>(20 + (l->data.size() * 320 + 999) / 1000, 500));
      for (DWORD i = 0; i < wait_ms && !(l->hdr.dwFlags & MHDR_DONE); i++) Sleep(1);
      long_.push_back(std::move(l));
      reap_long(false);
      return;
    }
    uint8_t st = m[0], ch = st & 15;
    uint8_t d1 = m.size() > 1 ? m[1] : 0, d2 = m.size() > 2 ? m[2] : 0;
    if ((st & 0xF0) == 0x90 && d2 > 0) sounding_[ch].set(d1 & 127);
    else if ((st & 0xF0) == 0x80 || (st & 0xF0) == 0x90) sounding_[ch].reset(d1 & 127);
    midiOutShortMsg(hmo_, DWORD(st) | (DWORD(d1) << 8) | (DWORD(d2) << 16));
  }

  // Nothing queued and no advance for latency + kIdleUs: the emulation is not
  // running (the saver sends no GO while the display is off, or a step
  // stalled). Release what is sounding, so no held note drones on meanwhile.
  // Live output only: the guest and the captures never see it.
  void release_if_idle() {
    int64_t idle = int64_t(VirtualClock::system_wall_us()) - last_anchor_wall_.load(std::memory_order_relaxed);
    if (idle < latency_us_ + kIdleUs) return;
    int released = 0;
    for (int c = 0; c < 16; c++) {
      if (sounding_[c].none()) continue;
      for (int n = 0; n < 128; n++)
        if (sounding_[c].test(size_t(n))) {
          midiOutShortMsg(hmo_, DWORD(0x80 | c) | (DWORD(n) << 8));
          released++;
        }
      midiOutShortMsg(hmo_, DWORD(0xB0 | c) | (123u << 8));
      sounding_[c].reset();
    }
    if (released) trace("audio", "live MIDI: no advance for %" PRId64 " ms; %d sounding notes released", idle / 1000, released);
  }

  void run() {
    UINT id = device_ < 0 ? MIDI_MAPPER : UINT(device_);
    MMRESULT mr = midiOutOpen(&hmo_, id, 0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
      log("audio: live MIDI off: midiOutOpen(%d) failed (error %u); the music is still captured", device_, mr);
      failed_ = true;
      return;
    }
    trace("audio", "live MIDI: midiOut device %d, latency %" PRId64 " ms", device_, latency_us_ / 1000);
    for (;;) {
      Item item;
      bool have = false, idle = false;
      DWORD wait = kIdlePollMs;
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) break;
        if (q_.empty()) {
          idle = true;
        } else {
          int64_t now = int64_t(VirtualClock::system_wall_us());
          if (q_.front().deadline <= now) {
            item = std::move(q_.front());
            q_.pop_front();
            have = true;
          } else {
            wait = DWORD(std::min<int64_t>((q_.front().deadline - now + 999) / 1000, 1000));
          }
        }
      }
      if (have) {
        out(item.bytes);
        continue;
      }
      if (idle) release_if_idle();
      WaitForSingleObject(evt_, wait);
    }
    // Silence the synth: what is sounding, then every channel, then reset.
    for (int c = 0; c < 16; c++)
      for (int n = 0; n < 128; n++)
        if (sounding_[c].test(size_t(n))) midiOutShortMsg(hmo_, DWORD(0x80 | c) | (DWORD(n) << 8));
    for (int c = 0; c < 16; c++) midiOutShortMsg(hmo_, DWORD(0xB0 | c) | (123u << 8));
    reap_long(true);
    midiOutReset(hmo_);
    if (!long_.empty()) {
      // A driver that keeps a header after midiOutReset: closing now would
      // free what it may still read. Leave the device open (and the bytes
      // alive) instead; the process is ending.
      log("audio: live MIDI: the driver kept %zu SysEx message(s); the device is left open", long_.size());
      for (auto& l : long_) (void)l.release();
      long_.clear();
      return;
    }
    midiOutClose(hmo_);
    trace("audio", "live MIDI closed");
  }

  static constexpr int64_t kIdleUs = 500000;
  static constexpr DWORD kIdlePollMs = 250;

  const int device_;
  const int64_t latency_us_;
  int64_t anchor_wall_ = 0;  // emulation thread only
  Time anchor_guest_ = 0;
  std::atomic<int64_t> last_anchor_wall_{0};  // the latest anchor, for the MIDI thread
  HANDLE evt_ = nullptr;
  HMIDIOUT hmo_ = nullptr;
  std::thread thread_;
  std::mutex mu_;
  std::deque<Item> q_;
  std::vector<std::unique_ptr<LongMsg>> long_;  // SysEx the driver has not handed back (MIDI thread only)
  bool stop_ = false, closed_ = false;
  std::atomic<bool> failed_{false};
  std::bitset<128> sounding_[16];
};

}  // namespace

std::unique_ptr<PcmSink> open_live_pcm(uint32_t rate, uint32_t latency_ms) {
  return std::make_unique<LivePcm>(rate, latency_ms);
}

std::unique_ptr<MidiSink> open_live_midi(int device, uint32_t latency_ms) {
  return std::make_unique<LiveMidi>(device, latency_ms);
}

}  // namespace adw::audio
