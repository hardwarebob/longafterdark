// The host audio engine (audio.h; docs/AUDIO.md §6).
//
// Every voice, stream and song keeps its state as an anchor in continuous
// virtual time — (t0, position at t0, rate) — so what the guest is told
// (cursors, positions, end times, events) is a closed-form function of its
// own calls and the times it passed, independent of the mixer rate and of how
// often advance() runs. The renderer samples those same functions at output
// frame k = time k * 10^6 / R with integer arithmetic only, so a capture is
// bit-identical run to run and machine to machine.
//
// Every call that takes a time first clamps it (never backwards) and renders
// up to it, so a state change is heard from output frame floor(t * R / 10^6).
//
// The MIDI bus carries the songs' events and the guest's own raw messages
// (midi_short/long/reset, for a guest that sequences itself through midiOut):
// both reach the sinks stamped with their guest time, in time order, under the
// same CC7 rule for the bus gain (AUDIO.md §6.4).
#include <algorithm>
#include <bitset>
#include <cinttypes>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <map>

#include "adw/core/audio.h"
#include "adw/core/log.h"
#include "audio_internal.h"

namespace adw::audio {

namespace {

constexpr uint64_t kUs = 1000000;
constexpr size_t kMaxVoices = 256;
constexpr uint32_t kMaxBufferBytes = 256u << 20;
// Guest-sized memory has a ceiling: all live buffers together (a DirectSound
// module makes a few of a few hundred KB), and what one stream holds queued
// and not yet played (a paused waveOut keeps every chunk written to it;
// 64 MB is six minutes of 44.1 kHz 16-bit stereo). Past either, creation
// fails and a write is handed back at once, unplayed.
constexpr uint64_t kMaxTotalBufferBytes = 512ull << 20;
constexpr uint64_t kMaxStreamBytes = 64ull << 20;
constexpr size_t kMixBlock = 4096;

using u128 = unsigned __int128;
using i128 = __int128;

// ceil(a * b / c), 128-bit intermediate.
uint64_t mul_div_ceil(uint64_t a, uint64_t b, uint64_t c) {
  u128 p = u128(a) * b;
  return uint64_t((p + c - 1) / c);
}
uint64_t mul_div(uint64_t a, uint64_t b, uint64_t c) { return uint64_t(u128(a) * b / c); }

const char* bus_name(Bus b) { return b == Bus::wave ? "wave" : "midi"; }

// "[audio] @<µs> …" (only built when ADTRACE=audio is on).
#define ATRACE(...)                                  \
  do {                                               \
    if (tracing("audio")) trace("audio", __VA_ARGS__); \
  } while (0)

// ---- the disabled engine ----------------------------------------------------------
class DisabledEngine : public Engine {
 public:
  explicit DisabledEngine(Config c) : config_(std::move(c)) { config_.guest_sound = false; }
  const Config& config() const override { return config_; }
  BufferId create_buffer(const WaveFormat&, uint32_t) override { return 0; }
  void write_buffer(BufferId, uint32_t, std::span<const uint8_t>, Time) override {}
  void release_buffer(BufferId) override {}
  const WaveFormat* buffer_format(BufferId) const override { return nullptr; }
  uint32_t buffer_size(BufferId) const override { return 0; }
  VoiceId create_voice(BufferId, Bus) override { return 0; }
  void destroy_voice(VoiceId, Time) override {}
  void play(VoiceId, bool, Time) override {}
  void stop(VoiceId, Time) override {}
  void set_cursor(VoiceId, uint32_t, Time) override {}
  uint32_t cursor(VoiceId, Time) override { return 0; }
  bool playing(VoiceId, Time) override { return false; }
  bool looping(VoiceId, Time) override { return false; }
  Time end_time(VoiceId, Time) override { return 0; }
  void set_gain(VoiceId, Gain, Time) override {}
  void set_rate(VoiceId, uint32_t, Time) override {}
  uint32_t rate(VoiceId) const override { return 0; }
  StreamId open_stream(const WaveFormat&, Bus, Time) override { return 0; }
  void stream_write(StreamId, std::span<const uint8_t>, uint64_t, Time) override {}
  void stream_pause(StreamId, Time) override {}
  void stream_restart(StreamId, Time) override {}
  void stream_reset(StreamId, Time) override {}
  uint64_t stream_position(StreamId, Time) override { return 0; }
  bool stream_paused(StreamId) const override { return false; }
  void set_stream_gain(StreamId, Gain, Time) override {}
  void close_stream(StreamId, Time) override {}
  SongId load_song(std::span<const uint8_t>, std::string* error) override {
    if (error) *error = "sound is off";
    return 0;
  }
  void song_play(SongId, Time) override {}
  void song_stop(SongId, Time) override {}
  void song_seek(SongId, uint64_t, Time) override {}
  uint64_t song_position(SongId, Time) override { return 0; }
  uint64_t song_length(SongId) const override { return 0; }
  bool song_playing(SongId, Time) override { return false; }
  void close_song(SongId, Time) override {}
  void midi_short(uint32_t, Time) override {}
  void midi_long(std::span<const uint8_t>, Time) override {}
  void midi_reset(Time) override {}
  void set_bus_gain(Bus, Gain, Time) override {}
  Gain bus_gain(Bus) const override { return Gain{}; }
  void poll(Time, std::vector<Event>&) override {}
  Time next_event_time() override { return 0; }
  void advance(Time) override {}
  void shutdown(Time) override {}
  Stats stats() const override { return {}; }

 private:
  Config config_;
};

// ---- the engine ---------------------------------------------------------------------
class EngineImpl : public Engine {
 public:
  explicit EngineImpl(const Config& c) : config_(c), R_(std::clamp<uint32_t>(c.rate, 8000, 96000)) {
    config_.rate = R_;
    if (config_.capture_mid.empty() && !config_.capture_wav.empty()) {
      // A hand-built Config (tests) gets the same derivation as from_env.
      std::string w = config_.capture_wav;
      size_t slash = w.find_last_of("\\/"), dot = w.find_last_of('.');
      if (dot != std::string::npos && (slash == std::string::npos || dot > slash + 1)) w.resize(dot);
      config_.capture_mid = w + ".mid";
    }
    std::string err;
    if (!config_.capture_wav.empty()) {
      wav_ = open_wav_capture(config_.capture_wav, R_, &err);
      if (!wav_) log("audio: WAV capture off: %s", err.c_str());
      else ATRACE("capture %s (%u Hz, 16-bit stereo)", config_.capture_wav.c_str(), R_);
    }
    if (!config_.capture_mid.empty()) {
      mid_ = open_midi_capture(config_.capture_mid, &err);
      if (!mid_) log("audio: MIDI capture off: %s", err.c_str());
      else ATRACE("capture %s (MIDI event log, 1 tick = 1 ms)", config_.capture_mid.c_str());
    }
    if (config_.live) {
      live_pcm_ = open_live_pcm(R_, config_.latency_ms);
      if (config_.live_midi) live_midi_ = open_live_midi(config_.midi_device, config_.latency_ms);
    }
    bus_gain_[0] = bus_gain_[1] = Gain{};
  }

  ~EngineImpl() override { shutdown(latest_); }

  const Config& config() const override { return config_; }

  // ---- buffers ----
  BufferId create_buffer(const WaveFormat& pcm, uint32_t bytes) override {
    if (!playable(pcm) || bytes == 0 || bytes > kMaxBufferBytes) return 0;
    uint64_t held = 0;
    for (const auto& [bid, b] : buffers_) held += b.data.size();
    if (held + bytes > kMaxTotalBufferBytes) {
      log("audio: a %u-byte buffer would take the buffers past %llu MB; refused", bytes,
          (unsigned long long)(kMaxTotalBufferBytes >> 20));
      return 0;
    }
    BufferId id = ++next_buffer_;
    Buffer& b = buffers_[id];
    b.fmt = pcm;
    b.data.assign(bytes, 0);
    b.frames = bytes / pcm.block_align;
    return id;
  }

  void write_buffer(BufferId id, uint32_t offset, std::span<const uint8_t> bytes, Time t) override {
    t = sync(t);
    Buffer* b = buffer(id);
    if (!b || offset >= b->data.size()) return;
    size_t n = std::min(bytes.size(), b->data.size() - offset);
    memcpy(b->data.data() + offset, bytes.data(), n);
  }

  void release_buffer(BufferId id) override {
    auto it = buffers_.find(id);
    if (it == buffers_.end() || it->second.released) return;
    it->second.released = true;
    if (it->second.refs == 0) buffers_.erase(it);
  }

  const WaveFormat* buffer_format(BufferId id) const override {
    auto it = buffers_.find(id);
    return it == buffers_.end() ? nullptr : &it->second.fmt;
  }

  uint32_t buffer_size(BufferId id) const override {
    auto it = buffers_.find(id);
    return it == buffers_.end() ? 0 : uint32_t(it->second.data.size());
  }

  // ---- voices ----
  VoiceId create_voice(BufferId id, Bus bus) override {
    Buffer* b = buffer(id);
    if (!b || voices_.size() >= kMaxVoices) return 0;
    VoiceId v = ++next_voice_;
    Voice& vc = voices_[v];
    vc.buf = id;
    vc.b = b;
    vc.bus = bus;
    vc.rate = b->fmt.rate;
    vc.t0 = latest_;
    b->refs++;
    return v;
  }

  void destroy_voice(VoiceId id, Time t) override {
    t = sync(t);
    auto it = voices_.find(id);
    if (it == voices_.end()) return;
    Voice& v = it->second;
    settle(id, v, t);
    if (v.playing) {
      cancel_end(v, t);
      ATRACE("@%" PRIu64 " voice %u stop frame %" PRIu64 " (destroyed)", t, id, frame_at(v, t));
    }
    Buffer* b = v.b;
    BufferId bid = v.buf;
    voices_.erase(it);
    if (--b->refs == 0 && b->released) buffers_.erase(bid);
  }

  void play(VoiceId id, bool loop, Time t) override {
    t = sync(t);
    Voice* v = voice(id);
    if (!v) return;
    settle(id, *v, t);
    if (v->playing) {
      if (loop == v->loop) return;
      reanchor(*v, t);
      v->loop = loop;
      reschedule_end(id, *v, t);
      ATRACE("@%" PRIu64 " voice %u loop %d", t, id, loop ? 1 : 0);
      return;
    }
    v->playing = true;
    v->loop = loop;
    v->t0 = t;
    stats_.voices_started++;
    reschedule_end(id, *v, t);
    ATRACE("@%" PRIu64 " voice %u start buf %u frame %" PRIu64 " of %u rate %u loop %d gain %04x/%04x bus %s", t, id,
           v->buf, v->f0, v->b->frames, v->rate, loop ? 1 : 0, v->gain.left, v->gain.right, bus_name(v->bus));
  }

  void stop(VoiceId id, Time t) override {
    t = sync(t);
    Voice* v = voice(id);
    if (!v) return;
    settle(id, *v, t);
    if (!v->playing) return;
    reanchor(*v, t);
    v->playing = false;
    cancel_end(*v, t);
    ATRACE("@%" PRIu64 " voice %u stop frame %" PRIu64, t, id, v->f0);
  }

  void set_cursor(VoiceId id, uint32_t byte_offset, Time t) override {
    t = sync(t);
    Voice* v = voice(id);
    if (!v) return;
    settle(id, *v, t);
    uint64_t f = byte_offset / v->b->fmt.block_align;
    uint64_t n = v->b->frames;
    v->f0 = n == 0 ? 0 : std::min<uint64_t>(f, n - 1);
    v->t0 = t;
    if (v->playing) reschedule_end(id, *v, t);
    ATRACE("@%" PRIu64 " voice %u cursor frame %" PRIu64, t, id, v->f0);
  }

  uint32_t cursor(VoiceId id, Time t) override {
    t = sync(t);
    Voice* v = voice(id);
    if (!v) return 0;
    settle(id, *v, t);
    return uint32_t(frame_at(*v, t) * v->b->fmt.block_align);
  }

  bool playing(VoiceId id, Time t) override {
    t = sync(t);
    Voice* v = voice(id);
    if (!v) return false;
    settle(id, *v, t);
    return v->playing;
  }

  bool looping(VoiceId id, Time t) override {
    t = sync(t);
    Voice* v = voice(id);
    if (!v) return false;
    settle(id, *v, t);
    return v->playing && v->loop;
  }

  Time end_time(VoiceId id, Time t) override {
    t = sync(t);
    Voice* v = voice(id);
    if (!v) return 0;
    settle(id, *v, t);
    return v->playing && !v->loop ? v->end_at : 0;
  }

  void set_gain(VoiceId id, Gain g, Time t) override {
    t = sync(t);
    Voice* v = voice(id);
    if (!v || v->gain == g) return;
    v->gain = g;
    ATRACE("@%" PRIu64 " voice %u gain %04x/%04x", t, id, g.left, g.right);
  }

  void set_rate(VoiceId id, uint32_t hz, Time t) override {
    t = sync(t);
    Voice* v = voice(id);
    if (!v) return;
    settle(id, *v, t);
    uint32_t r = hz == 0 ? v->b->fmt.rate : std::clamp<uint32_t>(hz, 100, 200000);
    if (r == v->rate) return;
    if (v->playing) reanchor(*v, t);
    v->rate = r;
    if (v->playing) reschedule_end(id, *v, t);
    ATRACE("@%" PRIu64 " voice %u rate %u", t, id, r);
  }

  uint32_t rate(VoiceId id) const override {
    auto it = voices_.find(id);
    return it == voices_.end() ? 0 : it->second.rate;
  }

  // ---- streams ----
  StreamId open_stream(const WaveFormat& pcm, Bus bus, Time t) override {
    t = sync(t);
    if (!playable(pcm)) return 0;
    StreamId id = ++next_stream_;
    Stream& s = streams_[id];
    s.fmt = pcm;
    s.bus = bus;
    s.t0 = t;
    ATRACE("@%" PRIu64 " stream %u open %u Hz %u-bit %s bus %s", t, id, pcm.rate, pcm.bits,
           pcm.channels == 2 ? "stereo" : "mono", bus_name(bus));
    return id;
  }

  void stream_write(StreamId id, std::span<const uint8_t> bytes, uint64_t cookie, Time t) override {
    t = sync(t);
    Stream* s = stream(id);
    if (!s) return;
    const uint32_t ba = s->fmt.block_align;
    uint64_t frames = bytes.size() / ba;
    if ((s->total - s->base + frames) * ba > kMaxStreamBytes) {
      // Over the ceiling (kMaxStreamBytes): handed back at once, unplayed,
      // so a guest waiting for it goes on.
      if (!s->overflowed) log("audio: stream %u holds %llu MB unplayed; further writes are returned unplayed", id,
                              (unsigned long long)(kMaxStreamBytes >> 20));
      s->overflowed = true;
      schedule(Event::Kind::chunk_done, id, cookie, t);
      return;
    }
    if (!s->paused && unclamped_consumed(*s, t) >= s->total) {
      // Starved (or never started): playback resumes from this write.
      s->q0 = s->total;
      s->t0 = t;
    }
    s->data.insert(s->data.end(), bytes.begin(), bytes.begin() + ptrdiff_t(frames * ba));
    s->total += frames;
    Chunk c;
    c.end = s->total;
    c.cookie = cookie;
    s->chunks.push_back(c);
    if (!s->paused) schedule_chunk(id, *s, s->chunks.back(), t);
    stats_.chunks++;
    ATRACE("@%" PRIu64 " stream %u chunk %" PRIx64 " frames %" PRIu64 " done %s%" PRIu64, t, id, cookie, frames,
           s->paused ? "(paused) " : "@", s->chunks.back().scheduled ? s->chunks.back().key.first : 0);
  }

  void stream_pause(StreamId id, Time t) override {
    t = sync(t);
    Stream* s = stream(id);
    if (!s || s->paused) return;
    s->q0 = consumed(*s, t);
    s->t0 = t;
    s->paused = true;
    for (Chunk& c : s->chunks)
      if (c.scheduled && c.key.first > t) {
        pending_.erase(c.key);
        c.scheduled = false;
      }
    ATRACE("@%" PRIu64 " stream %u pause frame %" PRIu64, t, id, s->q0);
  }

  void stream_restart(StreamId id, Time t) override {
    t = sync(t);
    Stream* s = stream(id);
    if (!s || !s->paused) return;
    s->paused = false;
    s->t0 = t;
    for (Chunk& c : s->chunks)
      if (!c.scheduled) schedule_chunk(id, *s, c, t);
    ATRACE("@%" PRIu64 " stream %u restart frame %" PRIu64, t, id, s->q0);
  }

  void stream_reset(StreamId id, Time t) override {
    t = sync(t);
    Stream* s = stream(id);
    if (!s) return;
    reset(id, *s, t);
    ATRACE("@%" PRIu64 " stream %u reset", t, id);
  }

  uint64_t stream_position(StreamId id, Time t) override {
    t = sync(t);
    Stream* s = stream(id);
    return s ? consumed(*s, t) * s->fmt.block_align : 0;
  }

  bool stream_paused(StreamId id) const override {
    auto it = streams_.find(id);
    return it != streams_.end() && it->second.paused;
  }

  void set_stream_gain(StreamId id, Gain g, Time t) override {
    t = sync(t);
    Stream* s = stream(id);
    if (!s || s->gain == g) return;
    s->gain = g;
    ATRACE("@%" PRIu64 " stream %u gain %04x/%04x", t, id, g.left, g.right);
  }

  void close_stream(StreamId id, Time t) override {
    t = sync(t);
    auto it = streams_.find(id);
    if (it == streams_.end()) return;
    reset(id, it->second, t);
    streams_.erase(it);
    ATRACE("@%" PRIu64 " stream %u close", t, id);
  }

  // ---- songs ----
  SongId load_song(std::span<const uint8_t> smf, std::string* error) override {
    SmfSong parsed;
    std::string why;
    if (!parse_smf(smf, parsed, &why)) {
      if (error) *error = why;
      ATRACE("song refused: %s", why.c_str());
      return 0;
    }
    bool dual = !config_.mpc_base_channels && apply_mpc_rule(parsed);
    SongId id = ++next_song_;
    Song& s = songs_[id];
    s.smf = std::move(parsed);
    s.used = s.smf.channels_used();
    s.dual = dual;
    s.cc7.fill(100);
    for (const std::string& w : s.smf.warnings) ATRACE("song %u: %s", id, w.c_str());
    ATRACE("song %u load format %u tracks %u division %d events %zu length %" PRIu64 " us channels %04x%s", id,
           s.smf.format, s.smf.tracks, s.smf.division, s.smf.events.size(), s.smf.length_us, s.used,
           dual ? " (MPC dual-mode: channels 13-16 dropped)" : "");
    return id;
  }

  void song_play(SongId id, Time t) override {
    t = sync(t);
    Song* s = song(id);
    if (!s || s->playing) return;
    if (s->pos0 >= s->smf.length_us) {
      schedule(Event::Kind::song_end, id, 0, t);
      ATRACE("@%" PRIu64 " song %u play at its end: end", t, id);
      return;
    }
    s->playing = true;
    s->t0 = t;
    stats_.songs_started++;
    ATRACE("@%" PRIu64 " song %u start position %" PRIu64 " us", t, id, s->pos0);
    for (const auto& m : s->chase) emit_midi(t, m.data(), m.size());
    s->chase.clear();
    for (int c = 0; c < 16; c++)
      if (s->used & (1u << c)) send_cc7(t, c, s->cc7[c]);
    s->end_at = t + (s->smf.length_us - s->pos0);
    s->end_key = schedule(Event::Kind::song_end, id, 0, s->end_at);
    s->has_end = true;
  }

  void song_stop(SongId id, Time t) override {
    t = sync(t);
    Song* s = song(id);
    if (!s || !s->playing) return;
    stop_song(id, *s, t);
    ATRACE("@%" PRIu64 " song %u stop position %" PRIu64 " us", t, id, s->pos0);
  }

  void song_seek(SongId id, uint64_t position_us, Time t) override {
    t = sync(t);
    Song* s = song(id);
    if (!s) return;
    if (s->playing) stop_song(id, *s, t);
    s->pos0 = std::min(position_us, s->smf.length_us);
    s->next = size_t(std::lower_bound(s->smf.events.begin(), s->smf.events.end(), s->pos0,
                                      [](const SmfEvent& e, uint64_t p) { return e.us < p; }) -
                     s->smf.events.begin());
    chase(*s);
    ATRACE("@%" PRIu64 " song %u seek %" PRIu64 " us", t, id, s->pos0);
  }

  uint64_t song_position(SongId id, Time t) override {
    t = sync(t);
    Song* s = song(id);
    if (!s) return 0;
    return s->playing ? std::min(s->smf.length_us, s->pos0 + (t - s->t0)) : s->pos0;
  }

  uint64_t song_length(SongId id) const override {
    auto it = songs_.find(id);
    return it == songs_.end() ? 0 : it->second.smf.length_us;
  }

  bool song_playing(SongId id, Time t) override {
    t = sync(t);
    Song* s = song(id);
    return s && s->playing;
  }

  void close_song(SongId id, Time t) override {
    t = sync(t);
    auto it = songs_.find(id);
    if (it == songs_.end()) return;
    if (it->second.playing) stop_song(id, it->second, t);
    // Its events not yet polled go with it.
    for (auto p = pending_.begin(); p != pending_.end();) {
      if (p->second.kind == Event::Kind::song_end && p->second.id == id) p = pending_.erase(p);
      else ++p;
    }
    songs_.erase(it);
    ATRACE("@%" PRIu64 " song %u close", t, id);
  }

  // ---- raw MIDI (midiOut*) ----
  void midi_short(uint32_t msg, Time t) override {
    t = sync(t);
    const uint8_t b[3] = {uint8_t(msg), uint8_t(msg >> 8), uint8_t(msg >> 16)};
    if (b[0] >= 0xF8) {  // real time: one byte; running status stays
      emit_raw_system(t, b, 1);
      return;
    }
    if (b[0] >= 0xF0) {  // system common ends running status; F0/F7 belong in a long message
      raw_.running = 0;
      if (int n = system_len(b[0])) emit_raw_system(t, b, size_t(n));
      return;
    }
    uint8_t st, d1, d2;
    if (b[0] & 0x80) {
      st = b[0], d1 = b[1], d2 = b[2];
    } else if (raw_.running) {
      st = raw_.running, d1 = b[0], d2 = b[1];
    } else {
      return;  // data with no status to run on: dropped, as a synth would
    }
    raw_.running = st;
    raw_channel(st, uint8_t(d1 & 0x7F), uint8_t(d2 & 0x7F), t);
  }

  // Real-time bytes (F8–FF) may come anywhere in the stream, as MIDI 1.0 lets
  // them — between a message's data bytes and inside a SysEx too: each goes
  // out on its own as it comes, and the message around it goes on (running
  // status included), as a synth on the cable would take them.
  void midi_long(std::span<const uint8_t> bytes, Time t) override {
    t = sync(t);
    const size_t n = bytes.size();
    size_t i = 0;
    while (i < n) {
      const uint8_t b = bytes[i];
      if (b == 0xF0) {
        // SysEx: through its F7, or up to the next status byte that is not
        // real time (which ends an unterminated one), or the buffer's end.
        std::vector<uint8_t> sysex{b};
        size_t j = i + 1;
        while (j < n) {
          const uint8_t c = bytes[j++];
          if (c >= 0xF8) {
            emit_raw_system(t, &c, 1);
            continue;
          }
          if (c >= 0x80 && c != 0xF7) {
            j--;
            break;
          }
          sysex.push_back(c);
          if (c == 0xF7) break;
        }
        emit_midi(t, sysex.data(), sysex.size());
        raw_.running = 0;
        i = j;
        continue;
      }
      if (b >= 0xF8) {
        emit_raw_system(t, &b, 1);
        i++;
        continue;
      }
      if (b >= 0xF1) {
        raw_.running = 0;
        size_t len = size_t(system_len(b));
        if (len == 0 || i + len > n) {  // a stray F7, or cut short
          i++;
          continue;
        }
        emit_raw_system(t, bytes.data() + i, len);
        i += len;
        continue;
      }
      uint8_t st = raw_.running;
      if (b & 0x80) {
        st = b;
        i++;
      } else if (!st) {
        i++;
        continue;
      }
      const size_t need = ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) ? 1 : 2;
      uint8_t data[2] = {0, 0};
      size_t got = 0;
      while (got < need && i < n) {
        const uint8_t c = bytes[i];
        if (c >= 0xF8) {  // real time: out now, and the message goes on
          emit_raw_system(t, &c, 1);
          i++;
          continue;
        }
        if (c & 0x80) break;
        data[got++] = c;
        i++;
      }
      if (got < need) {
        if (i >= n) break;  // cut short at the end: dropped
        raw_.running = 0;   // a status byte where data belongs: the message is dropped
        continue;
      }
      raw_.running = st;
      raw_channel(st, data[0], data[1], t);
    }
  }

  void midi_reset(Time t) override {
    t = sync(t);
    raw_.running = 0;
    raw_notes_off(t, true);
    ATRACE("@%" PRIu64 " raw MIDI reset (channels %04x)", t, raw_.used);
  }

  // ---- buses, events, time ----
  void set_bus_gain(Bus b, Gain g, Time t) override {
    t = sync(t);
    Gain& cur = bus_gain_[b == Bus::wave ? 0 : 1];
    if (cur == g) return;
    cur = g;
    ATRACE("@%" PRIu64 " bus %s gain %04x/%04x", t, bus_name(b), g.left, g.right);
    if (b == Bus::midi) {
      for (auto& [id, s] : songs_)
        if (s.playing)
          for (int c = 0; c < 16; c++)
            if (s.used & (1u << c)) send_cc7(t, c, s.cc7[c]);
      // The raw port's channels too (the synth holds their scaled CC7).
      for (int c = 0; c < 16; c++)
        if (raw_.used & (1u << c)) send_cc7(t, c, raw_.cc7[c]);
    }
  }

  Gain bus_gain(Bus b) const override { return bus_gain_[b == Bus::wave ? 0 : 1]; }

  void poll(Time t, std::vector<Event>& out) override {
    t = clamp(t);
    while (!pending_.empty() && pending_.begin()->first.first <= t) {
      out.push_back(pending_.begin()->second);
      pending_.erase(pending_.begin());
    }
  }

  Time next_event_time() override {
    if (pending_.empty()) return 0;
    return std::max<Time>(pending_.begin()->first.first, 1);
  }

  void advance(Time t) override {
    t = clamp(t);
    if (live_midi_) live_midi_->anchor(t);
    render_to(t);
  }

  void shutdown(Time t) override {
    t = sync(t);
    if (shut_) return;
    // Silence what is sounding: the sinks get note-offs and CC123 for every
    // playing song. The songs themselves keep their (guest-visible) state.
    for (auto& [id, s] : songs_)
      if (s.playing) notes_off(s, t, true);
    raw_notes_off(t, false);
    if (live_pcm_) {
      live_pcm_->close();
      final_underruns_ = live_pcm_->underruns();
      final_dropped_ = live_pcm_->dropped_frames();
    }
    if (wav_) wav_->close();
    if (mid_) mid_->close(t);
    if (live_midi_) live_midi_->close(t);
    shut_ = true;
    ATRACE("@%" PRIu64 " shutdown: %" PRIu64 " frames rendered, %" PRIu64 " MIDI events, %" PRIu64
           " underruns, %" PRIu64 " frames dropped",
           t, stats_.rendered_frames, stats_.midi_events, final_underruns_, final_dropped_);
  }

  Stats stats() const override {
    Stats s = stats_;
    s.underruns = live_pcm_ && !shut_ ? live_pcm_->underruns() : final_underruns_;
    s.dropped_frames = live_pcm_ && !shut_ ? live_pcm_->dropped_frames() : final_dropped_;
    return s;
  }

 private:
  using Key = std::pair<Time, uint64_t>;  // (at, issue order)

  struct Buffer {
    WaveFormat fmt;
    std::vector<uint8_t> data;
    uint32_t frames = 0;
    bool released = false;
    uint32_t refs = 0;
  };
  struct Voice {
    BufferId buf = 0;
    Buffer* b = nullptr;
    Bus bus = Bus::wave;
    bool playing = false, loop = false;
    uint64_t f0 = 0;  // frame at t0 (the cursor while stopped)
    Time t0 = 0;
    uint32_t rate = 0;
    Gain gain;
    Time end_at = 0;  // playing, not looping: when the cursor reaches the end
    bool has_end = false;
    Key end_key{};
  };
  struct Chunk {
    uint64_t end = 0;  // stream frame (since the last reset) at which it is done
    uint64_t cookie = 0;
    bool scheduled = false;
    Key key{};
  };
  struct Stream {
    WaveFormat fmt;
    Bus bus = Bus::wave;
    Gain gain;
    std::vector<uint8_t> data;  // frames [base, total)
    uint64_t base = 0, total = 0;
    uint64_t q0 = 0;  // frames consumed at t0
    Time t0 = 0;
    bool paused = false;
    bool overflowed = false;  // a write was handed back over kMaxStreamBytes (logged once)
    std::deque<Chunk> chunks;
  };
  struct Song {
    SmfSong smf;
    uint16_t used = 0;
    bool dual = false;
    bool playing = false;
    uint64_t pos0 = 0;  // position (µs) at t0, or while stopped
    Time t0 = 0;
    size_t next = 0;  // next event to send
    Time end_at = 0;
    bool has_end = false;
    Key end_key{};
    std::array<uint8_t, 16> cc7{};  // the song's own channel volume, before the bus gain
    std::array<std::bitset<128>, 16> sounding;
    std::vector<std::vector<uint8_t>> chase;  // state to re-send at the next play (after a seek)
  };
  // The raw MIDI port (midi_short/long/reset): one per engine, as the guest's
  // one MIDI output device is. Its channels keep their CC7 (and stay "used")
  // across resets: the synth keeps them too.
  struct RawMidi {
    uint8_t running = 0;   // running status (a channel status byte)
    uint16_t used = 0;     // bit c = the port has sent a channel message on channel c
    std::array<uint8_t, 16> cc7;  // the guest's own channel volume, before the bus gain
    std::array<std::bitset<128>, 16> sounding;
    RawMidi() { cc7.fill(100); }
  };

  // ---- helpers ----
  Time clamp(Time t) {
    if (t < latest_) t = latest_;
    latest_ = t;
    return t;
  }
  Time sync(Time t) {
    t = clamp(t);
    render_to(t);
    return t;
  }
  // A released buffer stays usable (writes, new voices: a duplicate of a
  // duplicate) for as long as a voice keeps it alive.
  Buffer* buffer(BufferId id) {
    auto it = buffers_.find(id);
    return it == buffers_.end() ? nullptr : &it->second;
  }
  Voice* voice(VoiceId id) {
    auto it = voices_.find(id);
    return it == voices_.end() ? nullptr : &it->second;
  }
  Stream* stream(StreamId id) {
    auto it = streams_.find(id);
    return it == streams_.end() ? nullptr : &it->second;
  }
  Song* song(SongId id) {
    auto it = songs_.find(id);
    return it == songs_.end() ? nullptr : &it->second;
  }

  Key schedule(Event::Kind k, uint32_t id, uint64_t cookie, Time at) {
    Key key{at, ++seq_};
    Event e;
    e.kind = k;
    e.id = id;
    e.cookie = cookie;
    e.at = at;
    pending_.emplace(key, e);
    return key;
  }

  // Voice position (frames, before wrapping) at t while playing.
  static uint64_t upos(const Voice& v, Time t) { return v.f0 + mul_div(t - v.t0, v.rate, kUs); }
  // The cursor frame at t (settled).
  static uint64_t frame_at(const Voice& v, Time t) {
    if (!v.playing) return v.f0;
    uint64_t p = upos(v, t);
    uint64_t n = v.b->frames;
    if (v.loop) return n ? p % n : 0;
    return std::min<uint64_t>(p, n);
  }
  void reanchor(Voice& v, Time t) {
    v.f0 = frame_at(v, t);
    v.t0 = t;
  }
  void cancel_end(Voice& v, Time t) {
    if (v.has_end && v.end_key.first > t) pending_.erase(v.end_key);
    v.has_end = false;
  }
  void reschedule_end(VoiceId id, Voice& v, Time t) {
    cancel_end(v, t);
    if (!v.playing || v.loop) return;
    uint64_t n = v.b->frames;
    v.end_at = v.f0 >= n ? v.t0 : v.t0 + mul_div_ceil(n - v.f0, kUs, v.rate);
    v.end_key = schedule(Event::Kind::voice_end, id, 0, v.end_at);
    v.has_end = true;
  }
  // A playing, non-looping voice whose end has come stops with its cursor
  // back at 0 (its voice_end event stays queued).
  void settle(VoiceId id, Voice& v, Time t) {
    if (!v.playing || v.loop || t < v.end_at) return;
    v.playing = false;
    v.has_end = false;
    v.f0 = 0;
    v.t0 = t;
    ATRACE("@%" PRIu64 " voice %u end", v.end_at, id);
  }

  uint64_t unclamped_consumed(const Stream& s, Time t) const {
    if (s.paused) return s.q0;
    return s.q0 + mul_div(t - s.t0, s.fmt.rate, kUs);
  }
  uint64_t consumed(const Stream& s, Time t) const { return std::min(s.total, unclamped_consumed(s, t)); }
  void schedule_chunk(StreamId id, Stream& s, Chunk& c, Time t) {
    Time at = c.end <= s.q0 ? std::max(s.t0, t) : s.t0 + mul_div_ceil(c.end - s.q0, kUs, s.fmt.rate);
    c.key = schedule(Event::Kind::chunk_done, id, c.cookie, at);
    c.scheduled = true;
  }
  void reset(StreamId id, Stream& s, Time t) {
    for (Chunk& c : s.chunks) {
      if (c.scheduled && c.key.first <= t) continue;  // done already
      if (c.scheduled) pending_.erase(c.key);
      schedule(Event::Kind::chunk_done, id, c.cookie, t);
    }
    s.chunks.clear();
    s.data.clear();
    s.base = s.total = s.q0 = 0;
    s.t0 = t;
    s.paused = false;
  }

  // ---- MIDI ----
  void emit_midi(Time at, const uint8_t* msg, size_t len) {
    stats_.midi_events++;
    if (shut_) return;
    if (mid_) mid_->send(at, msg, len);
    if (live_midi_) live_midi_->send(at, msg, len);
  }
  void emit3(Time at, uint8_t a, uint8_t b, uint8_t c) {
    uint8_t m[3] = {a, b, c};
    emit_midi(at, m, 3);
  }
  // CC7 = round(value x the MIDI bus gain), the gain being the mean of its sides.
  uint8_t scaled_cc7(uint8_t value) const {
    const Gain& g = bus_gain_[1];
    uint32_t mean = (uint32_t(g.left) + g.right) / 2;
    uint32_t v = (uint32_t(value) * mean + 16384) >> 15;
    return uint8_t(std::min<uint32_t>(v, 127));
  }
  void send_cc7(Time at, int ch, uint8_t value) { emit3(at, uint8_t(0xB0 | ch), 7, scaled_cc7(value)); }
  void notes_off(Song& s, Time t, bool all_notes_off) {
    for (int c = 0; c < 16; c++) {
      for (int n = 0; n < 128; n++)
        if (s.sounding[c].test(size_t(n))) emit3(t, uint8_t(0x80 | c), uint8_t(n), 0);
      s.sounding[c].reset();
    }
    if (all_notes_off)
      for (int c = 0; c < 16; c++)
        if (s.used & (1u << c)) emit3(t, uint8_t(0xB0 | c), 123, 0);
  }
  // The raw port's channel message (midi_short, midi_long): the bus gain on
  // its CC7 and on a channel's first use, its notes tracked for silencing.
  void raw_channel(uint8_t st, uint8_t d1, uint8_t d2, Time t) {
    const int c = st & 0x0F, type = st & 0xF0;
    const bool cc7 = type == 0xB0 && d1 == 7;
    if (!(raw_.used & (1u << c))) {
      raw_.used = uint16_t(raw_.used | (1u << c));
      ATRACE("@%" PRIu64 " raw MIDI: channel %d in use", t, c + 1);
      // A channel whose CC7 the guest never sets still gets the bus gain, as
      // song_play gives a song's channels theirs.
      if (!cc7 && scaled_cc7(raw_.cc7[c]) != raw_.cc7[c]) send_cc7(t, c, raw_.cc7[c]);
    }
    if (type == 0x90 && d2 > 0) raw_.sounding[c].set(d1);
    else if (type == 0x80 || type == 0x90) raw_.sounding[c].reset(d1);
    if (cc7) {
      raw_.cc7[c] = d2;
      send_cc7(t, c, d2);
      return;
    }
    const uint8_t m[3] = {st, d1, d2};
    emit_midi(t, m, (type == 0xC0 || type == 0xD0) ? 2 : 3);
  }
  // System common (1..3 bytes: status and data) or real time (1): an SMF has no
  // event for them, so they go as an F7 escape (the live sink sends its bytes raw).
  void emit_raw_system(Time t, const uint8_t* b, size_t n) {
    uint8_t m[4] = {0xF7, b[0], 0, 0};
    for (size_t i = 1; i < n; i++) m[i + 1] = uint8_t(b[i] & 0x7F);
    emit_midi(t, m, n + 1);
  }
  // Bytes in a system common message (status included); 0 for F0/F7, which
  // only a long message carries.
  static int system_len(uint8_t st) {
    switch (st) {
      case 0xF1: case 0xF3: return 2;  // MTC quarter frame, song select
      case 0xF2: return 3;             // song position
      case 0xF4: case 0xF5: case 0xF6: return 1;  // undefined, tune request
      default: return 0;
    }
  }
  // Silences the raw port: note-off for every note it left sounding, then CC123
  // on those channels; a reset (midiOutReset) also sends sustain off and CC123
  // on every channel the port has used.
  void raw_notes_off(Time t, bool reset) {
    for (int c = 0; c < 16; c++) {
      if (!(raw_.used & (1u << c))) continue;
      const bool any = raw_.sounding[c].any();
      for (int k = 0; k < 128; k++)
        if (raw_.sounding[c].test(size_t(k))) emit3(t, uint8_t(0x80 | c), uint8_t(k), 0);
      raw_.sounding[c].reset();
      if (reset) emit3(t, uint8_t(0xB0 | c), 64, 0);
      if (reset || any) emit3(t, uint8_t(0xB0 | c), 123, 0);
    }
  }
  void stop_song(SongId id, Song& s, Time t) {
    s.pos0 = std::min(s.smf.length_us, s.pos0 + (t - s.t0));
    s.playing = false;
    if (s.has_end && s.end_key.first > t) pending_.erase(s.end_key);
    s.has_end = false;
    notes_off(s, t, true);
    (void)id;
  }
  void send_song_event(Song& s, const SmfEvent& e, Time at) {
    if (e.is_channel()) {
      uint8_t st = e.status, ch = e.channel();
      if (e.type() == 0x90 && e.d2 > 0) s.sounding[ch].set(e.d1);
      else if (e.type() == 0x80 || e.type() == 0x90) s.sounding[ch].reset(e.d1);
      if (e.type() == 0xB0 && e.d1 == 7) {
        s.cc7[ch] = e.d2;
        send_cc7(at, ch, e.d2);
        return;
      }
      uint8_t m[3] = {st, e.d1, e.d2};
      emit_midi(at, m, size_t(e.channel_len()));
    } else if (e.status == 0xF0 || e.status == 0xF7) {
      std::vector<uint8_t> m;
      m.reserve(size_t(e.len) + 1);
      m.push_back(e.status);
      m.insert(m.end(), s.smf.blob.begin() + e.off, s.smf.blob.begin() + e.off + e.len);
      emit_midi(at, m.data(), m.size());
    }
    // Meta events are not sent.
  }
  // The program, controller and pitch-bend state in force before pos0.
  void chase(Song& s) {
    s.chase.clear();
    s.cc7.fill(100);
    int program[16];
    int bend[16][2];
    int16_t ctl[16][128];
    for (int c = 0; c < 16; c++) {
      program[c] = -1;
      bend[c][0] = -1;
      for (int k = 0; k < 128; k++) ctl[c][k] = -1;
    }
    for (size_t i = 0; i < s.next; i++) {
      const SmfEvent& e = s.smf.events[i];
      if (!e.is_channel()) continue;
      int c = e.channel();
      switch (e.type()) {
        case 0xC0: program[c] = e.d1; break;
        case 0xE0: bend[c][0] = e.d1; bend[c][1] = e.d2; break;
        case 0xB0:
          if (e.d1 == 7) s.cc7[c] = e.d2;
          else if (e.d1 < 120) ctl[c][e.d1] = e.d2;
          break;
        default: break;
      }
    }
    for (int c = 0; c < 16; c++) {
      uint8_t cc = uint8_t(0xB0 | c);
      // Bank select first, then the program, then the other controllers.
      for (int k : {0, 32})
        if (ctl[c][k] >= 0) s.chase.push_back({cc, uint8_t(k), uint8_t(ctl[c][k])});
      if (program[c] >= 0) s.chase.push_back({uint8_t(0xC0 | c), uint8_t(program[c])});
      for (int k = 1; k < 120; k++)
        if (k != 32 && ctl[c][k] >= 0) s.chase.push_back({cc, uint8_t(k), uint8_t(ctl[c][k])});
      if (bend[c][0] >= 0) s.chase.push_back({uint8_t(0xE0 | c), uint8_t(bend[c][0]), uint8_t(bend[c][1])});
    }
  }
  // Song events up to t, across songs in time order (ties: song id), and
  // their natural ends.
  void emit_songs(Time t) {
    for (;;) {
      Song* best = nullptr;
      SongId best_id = 0;
      Time bt = 0;
      for (auto& [id, s] : songs_) {
        if (!s.playing) continue;
        Time nt = s.next < s.smf.events.size() ? s.t0 + (s.smf.events[s.next].us - s.pos0) : s.end_at;
        if (nt > t) continue;
        if (!best || nt < bt) {
          best = &s;
          best_id = id;
          bt = nt;
        }
      }
      if (!best) return;
      if (best->next < best->smf.events.size()) {
        send_song_event(*best, best->smf.events[best->next], bt);
        best->next++;
      } else {
        notes_off(*best, best->end_at, false);
        best->playing = false;
        best->pos0 = best->smf.length_us;
        best->has_end = false;
        ATRACE("@%" PRIu64 " song %u end", best->end_at, best_id);
      }
    }
  }

  // ---- rendering ----
  uint64_t frames_at(Time t) const { return mul_div(t, R_, kUs); }

  void render_to(Time t) {
    emit_songs(t);
    uint64_t target = frames_at(t);
    if (target > rendered_) {
      bool pcm = !shut_ && (wav_ || live_pcm_);
      while (rendered_ < target) {
        size_t n = size_t(std::min<uint64_t>(target - rendered_, kMixBlock));
        if (pcm) {
          mix(rendered_, n);
          if (wav_) wav_->write(out_.data(), n);
          if (live_pcm_) live_pcm_->write(out_.data(), n);
        }
        rendered_ += n;
        stats_.rendered_frames += n;
      }
      trim_streams(t);
    }
    // Voices that reached their end by t (after the frames that play their
    // tail): settled now, so the trace line is timely.
    for (auto& [id, v] : voices_) settle(id, v, t);
    if (mid_ && !shut_) mid_->progress(t);
  }

  static int32_t sample(const uint8_t* p, uint16_t bits) {
    return bits == 8 ? (int32_t(p[0]) - 128) * 256 : int32_t(int16_t(uint16_t(p[0] | (p[1] << 8))));
  }

  // Walks the position p(k) = f0 + (k * 10^6 - t0 * R) * rate / (R * 10^6)
  // for output frames k0, k0+1, … exactly: the integer part and the remainder
  // over D = R * 10^6 (the remainder gives the interpolation fraction).
  struct Walk {
    uint64_t ip = 0, rem = 0, D = 0, step_int = 0, step_rem = 0;
    Walk(uint64_t f0, Time t0, uint32_t rate, uint64_t k0, uint64_t R) {
      D = R * kUs;
      step_int = uint64_t(rate) / R;
      step_rem = (uint64_t(rate) % R) * kUs;
      i128 a = i128(k0) * kUs - i128(t0) * R;
      if (a < 0) {
        // Only the first frame of a change can precede its exact time: it
        // plays from f0; the walk resumes exactly at the next frame.
        ip = f0;
        rem = 0;
        pending_a = a + kUs;
        clamped = true;
      } else {
        u128 q = u128(a) * rate;
        ip = f0 + uint64_t(q / D);
        rem = uint64_t(q % D);
      }
      base = f0;
      r = rate;
    }
    void next() {
      if (clamped) {
        clamped = false;
        if (pending_a < 0) {  // cannot happen for R >= 1, kept for safety
          pending_a += kUs;
          clamped = true;
          return;
        }
        u128 q = u128(pending_a) * r;
        ip = base + uint64_t(q / D);
        rem = uint64_t(q % D);
        return;
      }
      ip += step_int;
      rem += step_rem;
      if (rem >= D) {
        rem -= D;
        ip++;
      }
    }
    uint32_t frac16() const { return uint32_t((rem << 16) / D); }
    i128 pending_a = 0;
    bool clamped = false;
    uint64_t base = 0;
    uint32_t r = 0;
  };

  void mix(uint64_t k0, size_t n) {
    acc_.assign(n * 2, 0);
    out_.resize(n * 2);
    for (auto& [id, v] : voices_) {
      if (!v.playing || v.b->frames == 0) continue;
      Gain g{gain_mul(v.gain.left, bus_gain_[v.bus == Bus::wave ? 0 : 1].left),
             gain_mul(v.gain.right, bus_gain_[v.bus == Bus::wave ? 0 : 1].right)};
      const Buffer& b = *v.b;
      const uint64_t N = b.frames;
      const uint16_t ba = b.fmt.block_align, bits = b.fmt.bits, ch = b.fmt.channels;
      const uint16_t bs = bits / 8;
      Walk w(v.f0, v.t0, v.rate, k0, R_);
      for (size_t i = 0; i < n; i++, w.next()) {
        uint64_t p = w.ip;
        if (!v.loop && p >= N) break;
        uint64_t f = v.loop ? p % N : p;
        uint64_t f1 = f + 1 < N ? f + 1 : (v.loop ? 0 : f);
        const uint8_t* s0 = b.data.data() + f * ba;
        const uint8_t* s1 = b.data.data() + f1 * ba;
        mix_frame(i, s0, s1, bits, ch, bs, w.frac16(), g);
      }
    }
    for (auto& [id, s] : streams_) {
      if (s.paused || s.total == 0) continue;
      Gain g{gain_mul(s.gain.left, bus_gain_[s.bus == Bus::wave ? 0 : 1].left),
             gain_mul(s.gain.right, bus_gain_[s.bus == Bus::wave ? 0 : 1].right)};
      const uint16_t ba = s.fmt.block_align, bits = s.fmt.bits, ch = s.fmt.channels;
      const uint16_t bs = bits / 8;
      Walk w(s.q0, s.t0, s.fmt.rate, k0, R_);
      for (size_t i = 0; i < n; i++, w.next()) {
        uint64_t p = w.ip;
        if (p >= s.total) break;  // starved: silence until the next write
        if (p < s.base) continue;
        uint64_t p1 = p + 1 < s.total ? p + 1 : p;
        const uint8_t* s0 = s.data.data() + (p - s.base) * ba;
        const uint8_t* s1 = s.data.data() + (p1 - s.base) * ba;
        mix_frame(i, s0, s1, bits, ch, bs, w.frac16(), g);
      }
    }
    for (size_t i = 0; i < n * 2; i++) out_[i] = int16_t(std::clamp<int32_t>(acc_[i], -32768, 32767));
  }

  void mix_frame(size_t i, const uint8_t* s0, const uint8_t* s1, uint16_t bits, uint16_t ch, uint16_t bs,
                 uint32_t frac, Gain g) {
    int32_t l0 = sample(s0, bits), l1 = sample(s1, bits);
    int32_t r0 = ch == 2 ? sample(s0 + bs, bits) : l0;
    int32_t r1 = ch == 2 ? sample(s1 + bs, bits) : l1;
    int32_t l = l0 + int32_t((int64_t(l1 - l0) * frac) >> 16);
    int32_t r = r0 + int32_t((int64_t(r1 - r0) * frac) >> 16);
    acc_[i * 2] += int32_t((int64_t(l) * g.left) >> 15);
    acc_[i * 2 + 1] += int32_t((int64_t(r) * g.right) >> 15);
  }

  // Stream data behind the render position is no longer needed.
  void trim_streams(Time t) {
    for (auto& [id, s] : streams_) {
      while (!s.chunks.empty() && s.chunks.front().scheduled && s.chunks.front().key.first <= t) s.chunks.pop_front();
      uint64_t keep_from = consumed(s, t);
      uint64_t margin = s.fmt.rate / R_ + 4;
      keep_from = keep_from > margin ? keep_from - margin : 0;
      if (keep_from > s.base + 16384) {
        uint64_t drop = keep_from - s.base;
        s.data.erase(s.data.begin(), s.data.begin() + ptrdiff_t(drop * s.fmt.block_align));
        s.base = keep_from;
      }
    }
  }

  Config config_;
  const uint32_t R_;
  Time latest_ = 0;
  uint64_t rendered_ = 0;
  bool shut_ = false;
  uint64_t seq_ = 0;
  BufferId next_buffer_ = 0;
  VoiceId next_voice_ = 0;
  StreamId next_stream_ = 0;
  SongId next_song_ = 0;
  std::map<BufferId, Buffer> buffers_;
  std::map<VoiceId, Voice> voices_;
  std::map<StreamId, Stream> streams_;
  std::map<SongId, Song> songs_;
  RawMidi raw_;
  std::map<Key, Event> pending_;
  Gain bus_gain_[2];
  Stats stats_;
  uint64_t final_underruns_ = 0, final_dropped_ = 0;
  std::unique_ptr<PcmSink> wav_, live_pcm_;
  std::unique_ptr<MidiSink> mid_, live_midi_;
  std::vector<int32_t> acc_;
  std::vector<int16_t> out_;
};

}  // namespace

std::unique_ptr<Engine> make_engine(const Config& config) {
  if (!config.guest_sound) return std::make_unique<DisabledEngine>(config);
  return std::make_unique<EngineImpl>(config);
}

Engine& null_engine() {
  static DisabledEngine e{Config{}};
  return e;
}

}  // namespace adw::audio
