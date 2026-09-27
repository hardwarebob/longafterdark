// adw/core/audio.h — the host audio engine (docs/AUDIO.md). FROZEN:
// the lanes code against this header; a change goes through AUDIO.md §5.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace adw {
struct Env;
}

namespace adw::audio {

// Guest time: virtual microseconds on VirtualClock's scale, as a guest clock
// read would see it at the moment of the call (pe32: rt.clock().now_us();
// ne16: Runtime16::peek_us()). Never read_us() or any read that nudges time:
// audio calls must not move the clock. Times passed to one Engine never go
// backwards; a value earlier than the latest seen is taken as the latest seen.
using Time = uint64_t;

// ---- configuration ----------------------------------------------------------
struct Config {
  bool guest_sound = false;    // ADSOUND=1 or ADAUDIOOUT set (AUDIO.md §4)
  bool live = false;           // guest_sound && ADSOUND=1 && ADSTREAM=1 && ADAUDIOLIVE!=0
  int volume = 50;             // ADVOLUME, clamped 0..100
  std::string capture_wav;     // ADAUDIOOUT (UTF-8); "" = no capture
  std::string capture_mid;     // capture_wav with its extension replaced by ".mid"
  uint32_t rate = 44100;       // ADAUDIORATE, 8000..96000
  uint32_t latency_ms = 80;    // ADAUDIOLATENCYMS, 20..500
  bool live_midi = true;       // ADMIDI (default on)
  int midi_device = -1;        // ADMIDIDEV; -1 = MIDI mapper
  bool mpc_base_channels = false;  // ADMIDIBASE=1

  // Malformed values fall back to the defaults, with a line in *warnings.
  static Config from_env(const Env& env, std::vector<std::string>* warnings = nullptr);
};

// ---- formats ------------------------------------------------------------------
constexpr uint16_t kTagPcm = 0x0001, kTagMsAdpcm = 0x0002, kTagImaAdpcm = 0x0011;

struct WaveFormat {
  uint16_t tag = kTagPcm;
  uint16_t channels = 1;            // 1 or 2
  uint32_t rate = 22050;            // frames per second
  uint32_t avg_bytes = 22050;       // nAvgBytesPerSec
  uint16_t block_align = 1;         // nBlockAlign
  uint16_t bits = 8;                // PCM: 8 or 16; ADPCM: 4
  uint16_t samples_per_block = 0;   // ADPCM only
  std::vector<std::array<int16_t, 2>> coefs;  // MS-ADPCM only: (iCoef1, iCoef2) pairs
  bool operator==(const WaveFormat&) const = default;
};

WaveFormat pcm_format(uint32_t rate, uint16_t channels, uint16_t bits);
// The standard formats of Windows' imaadp32/msadp32 codecs (AUDIO.md §2.2):
// block_align 256*ch at <= 11025 Hz, 512*ch at 22050, 1024*ch at 44100.
WaveFormat ima_adpcm_format(uint32_t rate, uint16_t channels);
WaveFormat ms_adpcm_format(uint32_t rate, uint16_t channels);
// PCMWAVEFORMAT (16 bytes) or WAVEFORMATEX (18 + cbSize) as held in guest
// memory; `bytes` is what is readable there. False = malformed or truncated.
bool parse_waveformat(std::span<const uint8_t> bytes, WaveFormat& out);
// WAVEFORMATEX bytes (18 + cbSize, ADPCM extras included) for the guest.
std::vector<uint8_t> waveformat_bytes(const WaveFormat& f);
// PCM, 8|16 bits, 1|2 channels, 1000..192000 Hz, block_align = channels*bits/8.
bool playable(const WaveFormat& f);
// playable(), or IMA-/MS-ADPCM, 4 bits, 1|2 channels, a block that holds its header.
bool decodable(const WaveFormat& f);

struct Wave {
  WaveFormat format;
  std::span<const uint8_t> data;   // the data chunk, clipped to the image
  uint32_t fact_samples = 0;       // the fact chunk, 0 when absent
};
// A RIFF WAVE image: fmt, optional fact, data; other chunks skipped; odd
// chunk sizes padded. `out.data` points into `riff`.
bool parse_wave(std::span<const uint8_t> riff, Wave& out);
// From the first 12 bytes: 8 + the RIFF size when they read "RIFF….WAVE", else 0
// (how much guest memory sndPlaySound(SND_MEMORY) must read).
size_t riff_extent(std::span<const uint8_t> first12);

// Decoding to PCM16 at the source's rate and channels (PCM input is returned
// unchanged). Bit-exact with Windows' imaadp32.acm / msadp32.acm, partial
// blocks included: imaadp32 drops a trailing partial block; msadp32 decodes
// one that holds its 7*channels-byte header.
WaveFormat decoded_format(const WaveFormat& src);
// acmStreamSize(ACM_STREAMSIZEF_SOURCE): whole blocks, rounded up; 0 below one block.
size_t decoded_size(const WaveFormat& src, size_t src_bytes);
size_t decode(const WaveFormat& src, std::span<const uint8_t> in, std::span<uint8_t> out);  // bytes written
std::vector<uint8_t> decode(const WaveFormat& src, std::span<const uint8_t> in);
// acmStreamSize(ACM_STREAMSIZEF_DESTINATION): the source bytes of the whole
// blocks whose output fits in pcm_bytes (rounded down; 0 when none does).
size_t encoded_size_for(const WaveFormat& src, size_t pcm_bytes);

// ---- gains --------------------------------------------------------------------
// Linear gain per channel, Q15: 0x8000 = unity (0 dB), 0 = silent.
struct Gain {
  uint16_t left = 0x8000, right = 0x8000;
  bool operator==(const Gain&) const = default;
};
// DirectSound: volume in hundredths of a dB (-10000..0; -10000 = silent),
// pan -10000 (left only) .. 10000 (right only); the other side is attenuated
// by |pan| hundredths of a dB. From a fixed table: identical everywhere.
Gain gain_from_ds(int32_t volume, int32_t pan);
// A WINMM/MMSYSTEM device volume DWORD: low word left, high word right,
// 0..0xFFFF linear (a mono device repeats the low word).
Gain gain_from_mm(uint32_t volume);
uint32_t mm_from_gain(Gain g);

// ---- the engine ----------------------------------------------------------------
enum class Bus : uint8_t { wave, midi };

using BufferId = uint32_t;   // 0 = none
using VoiceId = uint32_t;
using StreamId = uint32_t;
using SongId = uint32_t;

struct Event {
  enum class Kind : uint8_t {
    voice_end,    // a playing, non-looping voice reached the end of its buffer (id = voice)
    chunk_done,   // a stream finished a chunk, or reset/close released it (id = stream, cookie)
    song_end,     // a playing song reached its end (id = song)
  };
  Kind kind = Kind::voice_end;
  uint32_t id = 0;
  uint64_t cookie = 0;
  Time at = 0;
};

struct Stats {
  uint64_t voices_started = 0, chunks = 0, songs_started = 0, midi_events = 0;
  uint64_t rendered_frames = 0, underruns = 0, dropped_frames = 0;
};

// Called from the emulation thread only. No method blocks on a device. With
// enabled() false the lanes keep their sound-off behaviour and call nothing
// but config()/enabled(); a disabled engine answers 0 ids and neutral values.
class Engine {
 public:
  virtual ~Engine() = default;
  virtual const Config& config() const = 0;
  bool enabled() const { return config().guest_sound; }

  // Buffers: PCM sample memory (a DirectSound buffer's contents, a decoded sound).
  // Zero-filled. 0 when !playable(pcm), bytes == 0 or bytes > 256 MiB.
  virtual BufferId create_buffer(const WaveFormat& pcm, uint32_t bytes) = 0;
  // Copies into the buffer (clipped to it); audible from t on.
  virtual void write_buffer(BufferId b, uint32_t offset, std::span<const uint8_t> bytes, Time t) = 0;
  // The buffer lives on until its last voice is destroyed.
  virtual void release_buffer(BufferId b) = 0;
  virtual const WaveFormat* buffer_format(BufferId b) const = 0;
  virtual uint32_t buffer_size(BufferId b) const = 0;

  // Voices: one play cursor over one buffer; several may share a buffer
  // (DuplicateSoundBuffer). New: stopped, cursor 0, unity gain, the buffer's rate.
  virtual VoiceId create_voice(BufferId b, Bus bus) = 0;
  virtual void destroy_voice(VoiceId v, Time t) = 0;
  virtual void play(VoiceId v, bool loop, Time t) = 0;   // from the cursor; if already playing only `loop` changes
  virtual void stop(VoiceId v, Time t) = 0;              // the cursor stays where playback got to
  virtual void set_cursor(VoiceId v, uint32_t byte_offset, Time t) = 0;  // rounded down to a block, clamped
  virtual uint32_t cursor(VoiceId v, Time t) = 0;        // play cursor, bytes
  virtual bool playing(VoiceId v, Time t) = 0;
  virtual bool looping(VoiceId v, Time t) = 0;
  virtual Time end_time(VoiceId v, Time t) = 0;          // end of a playing non-looping voice, else 0
  virtual void set_gain(VoiceId v, Gain g, Time t) = 0;
  virtual void set_rate(VoiceId v, uint32_t hz, Time t) = 0;  // 0 = the buffer's own; clamped 100..200000
  virtual uint32_t rate(VoiceId v) const = 0;

  // Streams: PCM chunks played back to back (waveOut).
  virtual StreamId open_stream(const WaveFormat& pcm, Bus bus, Time t) = 0;  // 0 when !playable(pcm)
  // Copies the chunk; an empty chunk completes when playback reaches it.
  virtual void stream_write(StreamId s, std::span<const uint8_t> bytes, uint64_t cookie, Time t) = 0;
  virtual void stream_pause(StreamId s, Time t) = 0;
  virtual void stream_restart(StreamId s, Time t) = 0;
  // Every queued chunk is done at t (events in queue order), position back to 0, not paused.
  virtual void stream_reset(StreamId s, Time t) = 0;
  virtual uint64_t stream_position(StreamId s, Time t) = 0;  // bytes played since open or the last reset
  virtual bool stream_paused(StreamId s) const = 0;
  virtual void set_stream_gain(StreamId s, Gain g, Time t) = 0;
  virtual void close_stream(StreamId s, Time t) = 0;         // reset, then gone; its events are still polled

  // Songs: Standard MIDI Files (format 0/1, PPQN or SMPTE division) on the MIDI bus.
  virtual SongId load_song(std::span<const uint8_t> smf, std::string* error = nullptr) = 0;  // 0 = unplayable
  virtual void song_play(SongId s, Time t) = 0;          // from the position; at the end already: song_end at t
  virtual void song_stop(SongId s, Time t) = 0;          // sounding notes off; the position stays
  virtual void song_seek(SongId s, uint64_t position_us, Time t) = 0;  // stops first; clamped to the length
  virtual uint64_t song_position(SongId s, Time t) = 0;  // µs from the start
  virtual uint64_t song_length(SongId s) const = 0;      // µs
  virtual bool song_playing(SongId s, Time t) = 0;
  virtual void close_song(SongId s, Time t) = 0;         // stops; its pending events are dropped

  // The guest's device volumes: wave = waveOutSetVolume; midi = midiOutSetVolume, auxSetVolume(1).
  virtual void set_bus_gain(Bus b, Gain g, Time t) = 0;
  virtual Gain bus_gain(Bus b) const = 0;

  // Events at or before t, in time order (equal times: in the order their
  // causes were issued), each returned once; appended to `out`.
  virtual void poll(Time t, std::vector<Event>& out) = 0;
  // The time of the earliest event not yet polled; 0 = none.
  virtual Time next_event_time() = 0;

  // Render up to t. run_host calls it after every step; lanes may call it too.
  virtual void advance(Time t) = 0;
  // Stop audible output now (QUIT, stdin EOF, ADFRAMES, lane end): live PCM
  // stops, MIDI all-notes-off, captures are finalized. Later calls are accepted
  // and change nothing audible.
  virtual void shutdown(Time t) = 0;
  virtual Stats stats() const = 0;
};

std::unique_ptr<Engine> make_engine(const Config& config);
// A disabled engine for lanes run without one (LaneContext::audio == nullptr, unit tests).
Engine& null_engine();

}  // namespace adw::audio
