// Capture sinks (AUDIO.md §6.6): a 16-bit stereo RIFF WAVE of the mix, and
// an SMF event log of the MIDI messages sent to the synth. Both patch their
// lengths on a 5 s grid of guest time and at close, so a killed host leaves
// readable files.
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "adw/core/log.h"
#include "adw/core/text.h"
#include "audio_internal.h"

namespace adw::audio {

namespace {

constexpr uint64_t kPatchUs = 5000000;  // 5 s of guest time

void put32le(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i));
}
void put32be(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (24 - 8 * i));
}

FILE* open_write(const std::string& path_utf8, std::string* error) {
  FILE* f = _wfopen(widen(path_utf8).c_str(), L"w+b");
  if (!f && error) *error = "cannot create '" + path_utf8 + "': " + strerror(errno);
  return f;
}

class WavCapture : public PcmSink {
 public:
  WavCapture(FILE* f, uint32_t rate) : f_(f), rate_(rate), next_patch_(uint64_t(rate) * 5) {
    auto h = wav_header(rate_, 0);
    fwrite(h.data(), 1, h.size(), f_);
    fflush(f_);
  }
  ~WavCapture() override { close(); }

  void write(const int16_t* lr, size_t frames) override {
    if (!f_) return;
    // RIFF sizes are 32-bit: stop short of 4 GiB (6.7 hours at 44.1 kHz).
    const uint64_t max_frames = (0xFFFFFFFFull - 36) / 4;
    if (frames_ + frames > max_frames) {
      if (!full_logged_) log("audio: the WAV capture reached its 4 GiB limit; the rest is not captured");
      full_logged_ = true;
      frames = size_t(max_frames - frames_);
    }
    // Little-endian int16 in memory is the file format.
    fwrite(lr, 4, frames, f_);
    frames_ += frames;
    while (frames_ >= next_patch_) {
      patch();
      next_patch_ += uint64_t(rate_) * 5;
    }
  }

  void close() override {
    if (!f_) return;
    patch();
    fclose(f_);
    f_ = nullptr;
  }

 private:
  void patch() {
    auto h = wav_header(rate_, frames_);
    fflush(f_);
    int64_t end = _ftelli64(f_);
    _fseeki64(f_, 0, SEEK_SET);
    fwrite(h.data(), 1, h.size(), f_);
    _fseeki64(f_, end, SEEK_SET);
    fflush(f_);
  }

  FILE* f_;
  uint32_t rate_;
  uint64_t frames_ = 0, next_patch_;
  bool full_logged_ = false;
};

// Format 0, 500 PPQN at the default tempo (500000 µs per quarter): one tick
// is one millisecond of guest time. Messages are written with explicit
// status bytes (no running status).
class MidiCapture : public MidiSink {
 public:
  explicit MidiCapture(FILE* f) : f_(f) {
    static const uint8_t hdr[22] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xF4,
                                    'M', 'T', 'r', 'k', 0, 0, 0, 0};
    fwrite(hdr, 1, sizeof(hdr), f_);
    fflush(f_);
  }
  ~MidiCapture() override {
    if (f_) close(last_ms_ * 1000);
  }

  void send(Time at, const uint8_t* msg, size_t len) override {
    if (!f_ || len == 0) return;
    progress(at);
    buf_.clear();
    uint64_t ms = at / 1000;
    put_vlq(buf_, uint32_t(ms > last_ms_ ? ms - last_ms_ : 0));
    if (ms > last_ms_) last_ms_ = ms;
    if (msg[0] == 0xF0) {  // SysEx: F0 <len> <data after F0>
      buf_.push_back(0xF0);
      put_vlq(buf_, uint32_t(len - 1));
      buf_.insert(buf_.end(), msg + 1, msg + len);
    } else if (msg[0] == 0xF7) {  // escape: F7 <len> <raw bytes>
      buf_.push_back(0xF7);
      put_vlq(buf_, uint32_t(len - 1));
      buf_.insert(buf_.end(), msg + 1, msg + len);
    } else {
      buf_.insert(buf_.end(), msg, msg + len);
    }
    fwrite(buf_.data(), 1, buf_.size(), f_);
    track_bytes_ += buf_.size();
  }

  void progress(Time t) override {
    if (!f_) return;
    while (t >= next_patch_us_) {
      patch();
      next_patch_us_ += kPatchUs;
    }
  }

  void close(Time t) override {
    if (!f_) return;
    uint64_t ms = t / 1000;
    buf_.clear();
    put_vlq(buf_, uint32_t(ms > last_ms_ ? ms - last_ms_ : 0));
    buf_.insert(buf_.end(), {0xFF, 0x2F, 0x00});
    fwrite(buf_.data(), 1, buf_.size(), f_);
    track_bytes_ += buf_.size();
    patch();
    fclose(f_);
    f_ = nullptr;
  }

 private:
  void patch() {
    uint8_t len[4];
    put32be(len, uint32_t(track_bytes_));
    fflush(f_);
    int64_t end = _ftelli64(f_);
    _fseeki64(f_, 18, SEEK_SET);
    fwrite(len, 1, 4, f_);
    _fseeki64(f_, end, SEEK_SET);
    fflush(f_);
  }

  FILE* f_;
  uint64_t last_ms_ = 0, track_bytes_ = 0, next_patch_us_ = kPatchUs;
  std::vector<uint8_t> buf_;
};

}  // namespace

std::array<uint8_t, 44> wav_header(uint32_t rate, uint64_t frames) {
  std::array<uint8_t, 44> h{};
  uint32_t data = uint32_t(frames * 4);
  memcpy(&h[0], "RIFF", 4);
  put32le(&h[4], 36 + data);
  memcpy(&h[8], "WAVEfmt ", 8);
  put32le(&h[16], 16);
  h[20] = 1;  // PCM
  h[22] = 2;  // stereo
  put32le(&h[24], rate);
  put32le(&h[28], rate * 4);
  h[32] = 4;   // block align
  h[34] = 16;  // bits
  memcpy(&h[36], "data", 4);
  put32le(&h[40], data);
  return h;
}

std::unique_ptr<PcmSink> open_wav_capture(const std::string& path_utf8, uint32_t rate, std::string* error) {
  FILE* f = open_write(path_utf8, error);
  if (!f) return nullptr;
  return std::make_unique<WavCapture>(f, rate);
}

std::unique_ptr<MidiSink> open_midi_capture(const std::string& path_utf8, std::string* error) {
  FILE* f = open_write(path_utf8, error);
  if (!f) return nullptr;
  return std::make_unique<MidiCapture>(f);
}

}  // namespace adw::audio
