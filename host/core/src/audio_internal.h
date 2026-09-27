// Internals of the audio engine (audio.h, docs/AUDIO.md §6) shared by
// its sources and by core's tests: the SMF parser, the capture writers, the
// sink interfaces and the dB table. Not part of the lanes' API.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "adw/core/audio.h"

namespace adw::audio {

// ---- gains ----------------------------------------------------------------------
// round(32768 * 10^(-i/2000)) for i = 0..10000 hundredths of a dB of attenuation
// (audio_db_table.inc, generated once and committed: identical everywhere).
inline constexpr int kDbTableSize = 10001;
uint16_t db_table(int attenuation_mb);  // 0..10000, clamped
// Q15 x Q15 -> Q15, saturated to 0xFFFF.
inline uint16_t gain_mul(uint16_t a, uint16_t b) {
  uint32_t p = (uint32_t(a) * uint32_t(b)) >> 15;
  return uint16_t(p > 0xFFFF ? 0xFFFF : p);
}

// ---- Standard MIDI Files ----------------------------------------------------------
struct SmfEvent {
  uint64_t tick = 0;
  uint64_t us = 0;      // absolute time from the start, through the tempo map
  uint32_t track = 0;
  uint8_t status = 0;   // 0x80..0xEF channel message, 0xF0 SysEx, 0xF7 escape, 0xFF meta
  uint8_t meta = 0;     // meta type (status 0xFF)
  uint8_t d1 = 0, d2 = 0;  // channel message data bytes
  uint32_t off = 0, len = 0;  // SysEx / escape / meta payload in SmfSong::blob
  bool is_channel() const { return status >= 0x80 && status < 0xF0; }
  uint8_t channel() const { return status & 0x0F; }
  uint8_t type() const { return status & 0xF0; }
  // Channel message length in bytes (status included).
  int channel_len() const { return (type() == 0xC0 || type() == 0xD0) ? 2 : 3; }
};

struct SmfSong {
  uint16_t format = 0;
  uint16_t tracks = 0;    // MTrk chunks read
  int16_t division = 0;   // > 0 PPQN; < 0 SMPTE (high byte -fps, low byte ticks per frame)
  std::vector<SmfEvent> events;  // merged over the tracks: (tick, track, order in track)
  std::vector<uint8_t> blob;
  uint64_t length_us = 0;  // the last end of track
  uint64_t length_ticks = 0;
  std::vector<std::string> warnings;  // tolerated damage (a truncated track, …)
  uint16_t channels_used() const;     // bit c = an event on channel c (0-based)
  // Note-ons (velocity > 0) per channel.
  std::array<uint32_t, 16> note_ons() const;
};

// Parses an SMF (or a RIFF RMID wrapping one). Format 2 and a missing or
// malformed header are refused (false, *error set). Damage inside a track
// ends that track where it starts (a warning), as a tolerant player would.
bool parse_smf(std::span<const uint8_t> bytes, SmfSong& out, std::string* error = nullptr);

// AUDIO.md §6.4: a song with note-ons on channel 13 and on any of 1..10 is MPC
// dual-mode; its events on channels 13..16 are dropped. Returns whether it did.
bool apply_mpc_rule(SmfSong& song);

// Writes a VLQ.
void put_vlq(std::vector<uint8_t>& out, uint32_t v);

// ---- sinks ------------------------------------------------------------------------
// Interleaved stereo int16 frames at the mixer rate, in guest-time order.
class PcmSink {
 public:
  virtual ~PcmSink() = default;
  virtual void write(const int16_t* lr, size_t frames) = 0;
  // Stop output; a capture is finalized. Called once.
  virtual void close() = 0;
  virtual uint64_t underruns() const { return 0; }
  virtual uint64_t dropped_frames() const { return 0; }
};

// MIDI messages stamped with their guest time, in nondecreasing time order.
// A message is a channel message (1..3 bytes), or F0 + SysEx data, or an F7
// escape (raw bytes).
class MidiSink {
 public:
  virtual ~MidiSink() = default;
  virtual void send(Time at, const uint8_t* msg, size_t len) = 0;
  // run_host's advance(t): the live sink re-takes its wall-clock anchor here.
  virtual void anchor(Time t) { (void)t; }
  // Guest time passed t (the capture patches its length on the 5 s grid).
  virtual void progress(Time t) { (void)t; }
  // Stop: the capture ends its track at t; the live sink silences the synth.
  virtual void close(Time t) = 0;
};

// Capture sinks (AUDIO.md §6.6). nullptr (with *error) when the file cannot be
// created.
std::unique_ptr<PcmSink> open_wav_capture(const std::string& path_utf8, uint32_t rate, std::string* error);
std::unique_ptr<MidiSink> open_midi_capture(const std::string& path_utf8, std::string* error);
// Live sinks (AUDIO.md §6.6): their own threads, never blocking the caller.
std::unique_ptr<PcmSink> open_live_pcm(uint32_t rate, uint32_t latency_ms);
std::unique_ptr<MidiSink> open_live_midi(int device, uint32_t latency_ms);

// The RIFF WAVE header of a 16-bit stereo capture holding `frames` frames.
std::array<uint8_t, 44> wav_header(uint32_t rate, uint64_t frames);

// ---- the live PCM ring (single producer, single consumer) ----------------------------
class PcmRing {
 public:
  explicit PcmRing(size_t capacity_frames);  // rounded up to a power of two
  // Producer. Returns the frames that did not fit (dropped).
  size_t push(const int16_t* lr, size_t frames);
  // Consumer.
  size_t fill() const;
  size_t pop(int16_t* lr, size_t frames);  // up to `frames`; returns how many
  size_t skip(size_t frames);              // drop the oldest; returns how many
  size_t capacity() const { return cap_; }

 private:
  std::vector<int16_t> buf_;
  size_t cap_, mask_;
  std::atomic<uint64_t> w_{0}, r_{0};
};

// The live ring's consumer side (the device thread's fill, audio_live.cc):
// prefill, underrun and drift control, without a device. The engine writes
// at the virtual clock's rate and the device reads at its own crystal's, so
// the ring's fill wanders (by some 100 ppm: ~20 ms in a few minutes). Kept at
// `target` frames: beyond +/- `band` of it, each fill drops (too full) or
// repeats (too empty) one frame — a 0.2% rate change at a 10 ms period,
// inaudible, and far more than any crystal's drift — so neither the 100 ms
// jump beyond `slack` (a stalled device or emulation) nor an underrun's
// re-prefill happens in a long session.
class PcmConsumer {
 public:
  PcmConsumer(PcmRing& ring, size_t target, size_t slack, size_t band)
      : ring_(ring), target_(target), slack_(slack), band_(band) {}
  void fill(int16_t* dst, size_t frames);
  // While no device plays: keep only the latest target's worth.
  void drop_backlog();
  void reset() { prefill_ = true; }

  uint64_t underruns = 0;  // the ring ran dry mid-fill (silence, then a new prefill)
  uint64_t dropped = 0;    // frames jumped over (fill beyond target + slack, backlog)
  uint64_t trimmed = 0;    // single frames dropped by drift control
  uint64_t padded = 0;     // single frames repeated by drift control

 private:
  PcmRing& ring_;
  const size_t target_, slack_, band_;
  bool prefill_ = true;
};

}  // namespace adw::audio
