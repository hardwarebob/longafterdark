// adw_win16_sound_tests: MMSYSTEM's sound half (win16/sound16.hh, AUDIO.md §8,
// the L16 tests of §10.2), with synthetic guest memory, synthetic callers and
// guest callbacks built as bytes:
//   * the silent device without an enabled engine (today's answers);
//   * device counts, caps and volumes (wave bus, MIDI bus = midiOut = aux 1);
//   * sndPlaySound's flag matrix (ASYNC, synchronous with the time charged,
//     LOOP, NOSTOP, NULL, a file and a WIN.INI [sounds] name), PCM and
//     MS-/IMA-ADPCM images, the copy made at the call;
//   * waveOut: format queries, WAVEHDR flags at the virtual end of a chunk,
//     pause/reset/close errors, MM_WOM_* to a window and to a function;
//   * the mciSendString grammar, MM_MCINOTIFY at a song's end, dispatched by
//     the lane's pump; SUPERSEDED/ABORTED before a command's own notify;
//   * CALLBACK_FUNCTION delivered at the next API call, never re-entrantly;
//   * the MCISEQ.DRV / TOOLHELP gates;
//   * the real engine (adw/core/audio.h make_engine): a synchronous sound's
//     charged time, MS-ADPCM, and a real song's MM_MCINOTIFY.
// A scripted engine (FakeEngine) stands in for most cases, so the lane's side
// is checked against exact, hand-computed times.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "adw/core/audio.h"
#include "adw/core/clock.h"
#include "adw/core/log.h"
#include "win16/dos16.hh"
#include "win16/modules16.hh"
#include "win16/runtime16.hh"
#include "win16/shim_families16.hh"
#include "win16/sound16.hh"
#include "win32/ini_store.hh"
#include "win32/vfs.hh"

using namespace adw;
using namespace adw::win16;

namespace {

int failures = 0, checks = 0;

#define CHECK(cond, ...)                          \
  do {                                            \
    checks++;                                     \
    if (!(cond)) {                                \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
      failures++;                                 \
    }                                             \
  } while (0)

// ---- a scripted engine ------------------------------------------------------------------------------------
//
// The Engine contract (audio.h) with exact, simple timing: a voice lasts
// frames / rate (rounded up to a µs), stream chunks play back to back at the
// format's byte rate, a song lasts song_length_us. Events come out of poll()
// in (time, issue) order.
class FakeEngine final : public audio::Engine {
 public:
  explicit FakeEngine(bool enabled = true) { cfg.guest_sound = enabled; }

  audio::Config cfg;
  uint64_t song_length_us = 2'000'000;
  std::vector<std::string> calls;

  struct Buf {
    audio::WaveFormat f;
    std::vector<uint8_t> bytes;
    bool released = false;
  };
  struct Voice {
    audio::BufferId b = 0;
    audio::Bus bus = audio::Bus::wave;
    bool on = false, loop = false;
    uint64_t t0 = 0, ev = 0;
    audio::Gain gain;
    uint32_t hz = 0;
  };
  struct Chunk {
    uint64_t cookie = 0, start = 0, end = 0, ev = 0;
    uint32_t bytes = 0;
  };
  struct Stream {
    audio::WaveFormat f;
    std::deque<Chunk> q;
    uint64_t done = 0, tail = 0, paused_at = 0;
    bool paused = false;
  };
  struct Song {
    uint64_t len = 0, pos = 0, t0 = 0, ev = 0;
    bool on = false;
  };
  struct Ev {
    audio::Event e;
    uint64_t seq;
  };
  std::map<uint32_t, Buf> bufs;
  std::map<uint32_t, Voice> voices;
  std::map<uint32_t, Stream> streams;
  std::map<uint32_t, Song> songs;
  std::vector<Ev> evs;
  audio::Gain bus[2];
  uint32_t next_id = 1;
  uint64_t seq = 0, latest = 0;

  uint64_t T(uint64_t t) { return latest = std::max(latest, t); }
  uint64_t add_ev(audio::Event::Kind k, uint32_t id, uint64_t cookie, uint64_t at) {
    audio::Event e;
    e.kind = k;
    e.id = id;
    e.cookie = cookie;
    e.at = at;
    evs.push_back({e, ++seq});
    return seq;
  }
  void cancel(uint64_t& s) {
    if (!s) return;
    uint64_t v = s;
    evs.erase(std::remove_if(evs.begin(), evs.end(), [&](const Ev& e) { return e.seq == v; }), evs.end());
    s = 0;
  }
  static uint64_t dur(uint64_t bytes, const audio::WaveFormat& f) {
    uint64_t frames = bytes / std::max<uint16_t>(f.block_align, 1);
    return (frames * 1'000'000 + f.rate - 1) / f.rate;
  }
  uint64_t voice_end(const Voice& v) { return v.t0 + dur(bufs.at(v.b).bytes.size(), bufs.at(v.b).f); }

  const audio::Config& config() const override { return cfg; }
  audio::BufferId create_buffer(const audio::WaveFormat& pcm, uint32_t bytes) override {
    if (!audio::playable(pcm) || !bytes) return 0;
    uint32_t id = next_id++;
    bufs[id] = Buf{pcm, std::vector<uint8_t>(bytes, 0), false};
    calls.push_back("create_buffer " + std::to_string(bytes));
    return id;
  }
  void write_buffer(audio::BufferId b, uint32_t off, std::span<const uint8_t> bytes, audio::Time t) override {
    T(t);
    auto& v = bufs.at(b).bytes;
    if (off >= v.size()) return;
    std::copy_n(bytes.begin(), std::min<size_t>(bytes.size(), v.size() - off), v.begin() + off);
  }
  void release_buffer(audio::BufferId b) override { bufs.at(b).released = true; }
  const audio::WaveFormat* buffer_format(audio::BufferId b) const override {
    auto it = bufs.find(b);
    return it == bufs.end() ? nullptr : &it->second.f;
  }
  uint32_t buffer_size(audio::BufferId b) const override {
    auto it = bufs.find(b);
    return it == bufs.end() ? 0 : uint32_t(it->second.bytes.size());
  }
  audio::VoiceId create_voice(audio::BufferId b, audio::Bus bus) override {
    if (!bufs.count(b)) return 0;
    uint32_t id = next_id++;
    voices[id] = Voice{b, bus};
    return id;
  }
  void destroy_voice(audio::VoiceId id, audio::Time t) override {
    T(t);
    auto it = voices.find(id);
    if (it == voices.end()) return;
    cancel(it->second.ev);
    voices.erase(it);
    calls.push_back("destroy_voice " + std::to_string(id));
  }
  void play(audio::VoiceId id, bool loop, audio::Time t) override {
    t = T(t);
    Voice& v = voices.at(id);
    v.on = true;
    v.loop = loop;
    v.t0 = t;
    cancel(v.ev);
    if (!loop) v.ev = add_ev(audio::Event::Kind::voice_end, id, 0, voice_end(v));
    calls.push_back("play " + std::to_string(id) + (loop ? " loop" : "") + " @" + std::to_string(t));
  }
  void stop(audio::VoiceId id, audio::Time t) override {
    T(t);
    Voice& v = voices.at(id);
    v.on = false;
    cancel(v.ev);
  }
  void set_cursor(audio::VoiceId, uint32_t, audio::Time) override {}
  uint32_t cursor(audio::VoiceId, audio::Time) override { return 0; }
  bool playing(audio::VoiceId id, audio::Time t) override {
    t = T(t);
    auto it = voices.find(id);
    return it != voices.end() && it->second.on && (it->second.loop || t < voice_end(it->second));
  }
  bool looping(audio::VoiceId id, audio::Time t) override { return playing(id, t) && voices.at(id).loop; }
  audio::Time end_time(audio::VoiceId id, audio::Time t) override {
    t = T(t);
    const Voice& v = voices.at(id);
    return v.on && !v.loop && t < voice_end(v) ? voice_end(v) : 0;
  }
  void set_gain(audio::VoiceId id, audio::Gain g, audio::Time) override { voices.at(id).gain = g; }
  void set_rate(audio::VoiceId id, uint32_t hz, audio::Time) override { voices.at(id).hz = hz; }
  uint32_t rate(audio::VoiceId id) const override { return voices.at(id).hz; }

  audio::StreamId open_stream(const audio::WaveFormat& pcm, audio::Bus, audio::Time t) override {
    if (!audio::playable(pcm)) return 0;
    uint32_t id = next_id++;
    streams[id] = Stream{pcm};
    streams[id].tail = T(t);
    calls.push_back("open_stream");
    return id;
  }
  void stream_write(audio::StreamId s, std::span<const uint8_t> bytes, uint64_t cookie, audio::Time t) override {
    t = T(t);
    Stream& st = streams.at(s);
    Chunk c;
    c.cookie = cookie;
    c.bytes = uint32_t(bytes.size());
    c.start = std::max(t, st.tail);
    c.end = c.start + dur(bytes.size(), st.f);
    st.tail = c.end;
    if (!st.paused) c.ev = add_ev(audio::Event::Kind::chunk_done, s, cookie, c.end);
    st.q.push_back(c);
    calls.push_back("stream_write " + std::to_string(bytes.size()));
  }
  void stream_pause(audio::StreamId s, audio::Time t) override {
    t = T(t);
    Stream& st = streams.at(s);
    if (st.paused) return;
    st.paused = true;
    st.paused_at = t;
    for (Chunk& c : st.q) {
      if (c.end > t) cancel(c.ev);
    }
  }
  void stream_restart(audio::StreamId s, audio::Time t) override {
    t = T(t);
    Stream& st = streams.at(s);
    if (!st.paused) return;
    uint64_t shift = t - st.paused_at;
    for (Chunk& c : st.q) {
      if (c.ev) continue;
      c.start += c.start >= st.paused_at ? shift : 0;
      c.end += shift;
      c.ev = add_ev(audio::Event::Kind::chunk_done, s, c.cookie, c.end);
    }
    st.tail += shift;
    st.paused = false;
  }
  void stream_reset(audio::StreamId s, audio::Time t) override {
    t = T(t);
    Stream& st = streams.at(s);
    for (Chunk& c : st.q) {
      cancel(c.ev);
      add_ev(audio::Event::Kind::chunk_done, s, c.cookie, t);
    }
    st.q.clear();
    st.done = 0;
    st.tail = t;
    st.paused = false;
  }
  uint64_t stream_position(audio::StreamId s, audio::Time t) override {
    t = T(t);
    Stream& st = streams.at(s);
    uint64_t pos = st.done;
    uint64_t now = st.paused ? st.paused_at : t;
    for (const Chunk& c : st.q) {
      if (c.end <= now) pos += c.bytes;
      else if (c.start < now) pos += (now - c.start) * st.f.avg_bytes / 1'000'000 / st.f.block_align * st.f.block_align;
    }
    return pos;
  }
  bool stream_paused(audio::StreamId s) const override { return streams.at(s).paused; }
  void set_stream_gain(audio::StreamId, audio::Gain, audio::Time) override {}
  void close_stream(audio::StreamId s, audio::Time t) override {
    stream_reset(s, t);
    streams.erase(s);
    calls.push_back("close_stream");
  }

  audio::SongId load_song(std::span<const uint8_t> smf, std::string* error) override {
    if (smf.size() < 4 || memcmp(smf.data(), "MThd", 4) != 0) {
      if (error) *error = "not an SMF";
      return 0;
    }
    uint32_t id = next_id++;
    songs[id] = Song{song_length_us};
    calls.push_back("load_song");
    return id;
  }
  void settle(Song& g, uint64_t t) {
    if (g.on && t >= g.t0 + (g.len - g.pos)) {
      g.pos = g.len;
      g.on = false;
    }
  }
  void song_play(audio::SongId s, audio::Time t) override {
    t = T(t);
    Song& g = songs.at(s);
    settle(g, t);
    if (g.on) return;
    g.on = true;
    g.t0 = t;
    g.ev = add_ev(audio::Event::Kind::song_end, s, 0, t + (g.len - g.pos));
    calls.push_back("song_play @" + std::to_string(t));
  }
  void song_stop(audio::SongId s, audio::Time t) override {
    t = T(t);
    Song& g = songs.at(s);
    settle(g, t);
    if (g.on) g.pos = std::min(g.len, g.pos + (t - g.t0));
    g.on = false;
    cancel(g.ev);
    calls.push_back("song_stop @" + std::to_string(t));
  }
  void song_seek(audio::SongId s, uint64_t pos, audio::Time t) override {
    song_stop(s, t);
    Song& g = songs.at(s);
    g.pos = std::min(pos, g.len);
  }
  uint64_t song_position(audio::SongId s, audio::Time t) override {
    t = T(t);
    Song& g = songs.at(s);
    settle(g, t);
    return g.on ? std::min(g.len, g.pos + (t - g.t0)) : g.pos;
  }
  uint64_t song_length(audio::SongId s) const override { return songs.at(s).len; }
  bool song_playing(audio::SongId s, audio::Time t) override {
    t = T(t);
    Song& g = songs.at(s);
    settle(g, t);
    return g.on;
  }
  void close_song(audio::SongId s, audio::Time t) override {
    T(t);
    auto it = songs.find(s);
    if (it == songs.end()) return;
    cancel(it->second.ev);
    songs.erase(it);
    calls.push_back("close_song");
  }
  void set_bus_gain(audio::Bus b, audio::Gain g, audio::Time t) override {
    T(t);
    bus[int(b)] = g;
  }
  audio::Gain bus_gain(audio::Bus b) const override { return bus[int(b)]; }
  void poll(audio::Time t, std::vector<audio::Event>& out) override {
    t = T(t);
    std::vector<Ev> due;
    for (const Ev& e : evs) {
      if (e.e.at <= t) due.push_back(e);
    }
    std::sort(due.begin(), due.end(), [](const Ev& a, const Ev& b) { return a.e.at != b.e.at ? a.e.at < b.e.at : a.seq < b.seq; });
    for (const Ev& e : due) {
      out.push_back(e.e);
      evs.erase(std::remove_if(evs.begin(), evs.end(), [&](const Ev& x) { return x.seq == e.seq; }), evs.end());
      if (e.e.kind == audio::Event::Kind::chunk_done) {
        auto it = streams.find(e.e.id);
        if (it == streams.end()) continue;
        auto& q = it->second.q;
        for (auto c = q.begin(); c != q.end(); ++c) {
          if (c->ev == e.seq) {
            it->second.done += c->bytes;
            q.erase(c);
            break;
          }
        }
      }
    }
  }
  audio::Time next_event_time() override {
    uint64_t best = 0;
    for (const Ev& e : evs) {
      if (!best || e.e.at < best) best = e.e.at;
    }
    return best;
  }
  void advance(audio::Time t) override { T(t); }
  void shutdown(audio::Time t) override { T(t); }
  audio::Stats stats() const override { return {}; }

  int live_voices() const { return int(voices.size()); }
  const Voice* only_voice() const { return voices.size() == 1 ? &voices.begin()->second : nullptr; }
};

// ---- a machine -------------------------------------------------------------------------------------------

struct Rig {
  // A 1 ms frame grid: time is moved by frames().
  VirtualClock clock{VirtualClock::Mode::fixed_step, 1000};
  Runtime16 rt;
  FakeEngine fake;
  explicit Rig(bool enabled = true, bool sound_device = true, audio::Engine* engine = nullptr)
      : rt(options(sound_device), clock), fake(enabled) {
    clock.set_read_step_us(0);
    clock.begin_frame();
    register_all16(rt);
    attach_audio16(rt, engine ? engine : &fake);
  }
  static Runtime16Options options(bool sound_device) {
    Runtime16Options o;
    o.sound_device = sound_device;
    return o;
  }
  uint32_t api(const char* module, const char* name, std::initializer_list<Arg16> args) {
    Shim16Entry* e = rt.shims().find_name(module, name);
    if (!e) throw std::runtime_error(std::string("no shim ") + name);
    return rt.call_far(rt.thunk_far(*e), args);
  }
  uint16_t api16(const char* module, const char* name, std::initializer_list<Arg16> args) {
    return uint16_t(api(module, name, args));
  }
  uint16_t code(const std::vector<uint8_t>& bytes) {
    GlobalBlock* b = rt.global().alloc_block(uint32_t(bytes.size() + 16), true, 0, 0);
    rt.mem().memcpy(b->base, bytes.data(), bytes.size());
    return b->sel;
  }
  uint16_t data(uint32_t size) { return rt.global().alloc_block(size, false, 0, 0)->sel; }
  uint32_t bytes(const std::vector<uint8_t>& v) {
    uint16_t s = data(uint32_t(v.size() + 16));
    rt.write_bytes(uint32_t(s) << 16, v.data(), v.size());
    return uint32_t(s) << 16;
  }
  uint32_t str(const std::string& s) { return rt.static_bytes("test: " + s, s); }
  // Moves the frame grid on to at least `us`.
  void to(uint64_t us) {
    while (clock.now_us() < us) clock.begin_frame();
  }
  uint64_t now() const { return rt.peek_us(); }
  // Any API call: the delivery point of §8.6.
  void tick() { api("MMSYSTEM", "timeGetTime", {}); }
  uint32_t mci(const std::string& cmd, std::string* ret = nullptr, uint16_t hwnd = 0, uint16_t cap = 128) {
    uint32_t buf = uint32_t(data(256)) << 16;
    uint32_t r = api("MMSYSTEM", "mciSendString", {l16(str(cmd)), l16(buf), w16(cap), w16(hwnd)});
    if (ret) *ret = rt.read_str(buf);
    return r;
  }
};

void append(std::vector<uint8_t>& v, const std::vector<uint8_t>& w) { v.insert(v.end(), w.begin(), w.end()); }
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)), v.push_back(uint8_t(x >> 8)); }
void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, uint16_t(x)), put16(v, uint16_t(x >> 16)); }

// A RIFF WAVE image.
std::vector<uint8_t> riff(const std::vector<uint8_t>& fmt, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> body = {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
  put32(body, uint32_t(fmt.size()));
  append(body, fmt);
  if (fmt.size() & 1) body.push_back(0);
  append(body, {'d', 'a', 't', 'a'});
  put32(body, uint32_t(data.size()));
  append(body, data);
  if (data.size() & 1) body.push_back(0);
  std::vector<uint8_t> out = {'R', 'I', 'F', 'F'};
  put32(out, uint32_t(body.size()));
  append(out, body);
  return out;
}
std::vector<uint8_t> pcm8(uint32_t rate, uint32_t n) {
  std::vector<uint8_t> d(n);
  for (uint32_t i = 0; i < n; i++) d[i] = uint8_t(0x80 + ((i * 7) & 0x3F));
  return riff(audio::waveformat_bytes(audio::pcm_format(rate, 1, 8)), d);
}

// A window procedure that appends (msg, wParam, lParam) to a record at
// DATA:0 (WORD count, then 8-byte entries) and returns 0.
std::vector<uint8_t> wndproc_recorder(uint16_t data) {
  return {0x55, 0x8B, 0xEC, 0x06, 0x53, 0xB8, uint8_t(data), uint8_t(data >> 8), 0x8E, 0xC0,  // push bp.. mov es,DATA
          0x26, 0x8B, 0x1E, 0x00, 0x00,                                                        // mov bx,es:[0]
          0xC1, 0xE3, 0x03, 0x83, 0xC3, 0x02,                                                  // shl bx,3; add bx,2
          0x8B, 0x46, 0x0C, 0x26, 0x89, 0x07,                                                  // msg
          0x8B, 0x46, 0x0A, 0x26, 0x89, 0x47, 0x02,                                            // wParam
          0x8B, 0x46, 0x06, 0x26, 0x89, 0x47, 0x04,                                            // lParam lo
          0x8B, 0x46, 0x08, 0x26, 0x89, 0x47, 0x06,                                            // lParam hi
          0x26, 0xFF, 0x06, 0x00, 0x00,                                                        // inc word es:[0]
          0x5B, 0x07, 0x31, 0xC0, 0x99, 0x5D, 0xCA, 0x0A, 0x00};                               // ... retf 10
}

// A waveOut CALLBACK_FUNCTION (hwo, msg, dwInstance, dwParam1, dwParam2),
// PASCAL: DATA:2 counts the nesting depth, entries from DATA:8 hold msg, hwo,
// dwParam1's low word and the depth at entry; it calls an API (timeGetTime)
// itself — a delivery point, where nothing may be delivered re-entrantly.
std::vector<uint8_t> function_recorder(uint16_t data, uint32_t api_thunk) {
  std::vector<uint8_t> v = {0x55, 0x8B, 0xEC, 0x06, 0x53, 0xB8, uint8_t(data), uint8_t(data >> 8), 0x8E, 0xC0,
                            0x26, 0xFF, 0x06, 0x02, 0x00,        // inc word es:[2] (depth)
                            0x26, 0x8B, 0x1E, 0x00, 0x00,        // mov bx,es:[0]
                            0xC1, 0xE3, 0x03, 0x83, 0xC3, 0x08,  // shl bx,3; add bx,8
                            0x8B, 0x46, 0x12, 0x26, 0x89, 0x07,  // msg
                            0x8B, 0x46, 0x14, 0x26, 0x89, 0x47, 0x02,  // hwo
                            0x8B, 0x46, 0x0A, 0x26, 0x89, 0x47, 0x04,  // dwParam1 lo
                            0x26, 0xA1, 0x02, 0x00, 0x26, 0x89, 0x47, 0x06,  // depth
                            0x26, 0xFF, 0x06, 0x00, 0x00};                  // inc word es:[0]
  append(v, {0x9A, uint8_t(api_thunk), uint8_t(api_thunk >> 8), uint8_t(api_thunk >> 16), uint8_t(api_thunk >> 24)});
  append(v, {0x26, 0xFF, 0x0E, 0x02, 0x00,  // dec word es:[2]
             0x5B, 0x07, 0x5D, 0xCA, 0x10, 0x00});
  return v;
}

struct Rec {
  uint16_t msg, wp;
  uint32_t lp;
};
std::vector<Rec> window_records(Rig& g, uint16_t data) {
  std::vector<Rec> out;
  uint32_t base = uint32_t(data) << 16;
  uint16_t n = g.rt.rd16(base);
  for (uint16_t i = 0; i < n; i++) {
    uint32_t e = base + 2 + 8u * i;
    out.push_back({g.rt.rd16(e), g.rt.rd16(e + 2), g.rt.rd32(e + 4)});
  }
  return out;
}
void clear_records(Rig& g, uint16_t data) { g.rt.wr16(uint32_t(data) << 16, 0); }

// A window of a class whose procedure records into `rec`.
uint16_t make_window(Rig& g, uint16_t rec) {
  uint16_t cs = g.code(wndproc_recorder(rec));
  uint16_t ds = g.data(256);
  uint32_t d = uint32_t(ds) << 16;
  g.rt.write_str(d, "SNDTEST", 16);
  g.rt.wr32(d + 0x20 + 2, uint32_t(cs) << 16);
  g.rt.wr32(d + 0x20 + 22, d);
  g.api("USER", "RegisterClass", {l16(d + 0x20)});
  uint16_t hwnd = uint16_t(g.api("USER", "CreateWindowEx", {l16(0), l16(d), l16(d), l16(0), w16(0), w16(0), w16(10),
                                                            w16(10), w16(0), w16(0), w16(0), l16(0)}));
  clear_records(g, rec);
  return hwnd;
}

std::string records_text(const std::vector<Rec>& r) {
  std::string s;
  for (const Rec& x : r) {
    char b[48];
    snprintf(b, sizeof(b), "%s%04X/%X/%X", s.empty() ? "" : " ", x.msg, x.wp, x.lp);
    s += b;
  }
  return s;
}

// ---- the silent device --------------------------------------------------------------------------------------

void test_disabled() {
  for (int pass = 0; pass < 2; pass++) {
    // No engine at all, then a disabled one (ADSOUND/ADAUDIOOUT unset).
    FakeEngine off(false);
    VirtualClock clock{VirtualClock::Mode::fixed_step, 1000};
    Runtime16 rt{Runtime16Options{}, clock};
    register_all16(rt);
    attach_audio16(rt, pass ? &off : nullptr);
    auto api = [&](const char* name, std::initializer_list<Arg16> args) {
      return rt.call_far(rt.thunk_far(*rt.shims().find_name("MMSYSTEM", name)), args);
    };
    uint32_t buf = uint32_t(rt.global().alloc_block(512, false, 0, 0)->sel) << 16;
    CHECK(!audio16_enabled(rt), "pass %d: not enabled", pass);
    CHECK((api("waveOutGetNumDevs", {}) & 0xFFFF) == 1, "pass %d: one (silent) wave device", pass);
    CHECK((api("waveOutGetDevCaps", {w16(0), l16(buf), w16(48)}) & 0xFFFF) == 0 &&
              rt.read_str(buf + 6) == "Long After Dark (silent)",
          "pass %d: the silent device's name (%s)", pass, rt.read_str(buf + 6).c_str());
    CHECK((api("midiOutGetNumDevs", {}) & 0xFFFF) == 0 && (api("auxGetNumDevs", {}) & 0xFFFF) == 0 &&
              (api("mixerGetNumDevs", {}) & 0xFFFF) == 0,
          "pass %d: no MIDI, aux or mixer devices", pass);
    CHECK((api("midiOutGetDevCaps", {w16(0), l16(buf), w16(50)}) & 0xFFFF) == 2 &&
              (api("auxGetVolume", {w16(1), l16(buf)}) & 0xFFFF) == 2,
          "pass %d: MMSYSERR_BADDEVICEID for MIDI and aux", pass);
    std::vector<uint8_t> img = pcm8(11025, 100);
    rt.write_bytes(buf, img.data(), img.size());
    CHECK((api("sndPlaySound", {l16(buf), w16(7)}) & 0xFFFF) == 1 && off.calls.empty(),
          "pass %d: sndPlaySound reports the sound played, the engine is not called", pass);
    uint32_t cmd = rt.static_bytes("open", "open sequencer");
    CHECK(api("mciSendString", {l16(cmd), l16(0), w16(0), w16(0)}) == 306, "pass %d: MCI refused (306)", pass);
    // Today's waveOutOpen: any format is fine.
    std::vector<uint8_t> ima = audio::waveformat_bytes(audio::ima_adpcm_format(22050, 1));
    rt.write_bytes(buf + 256, ima.data(), ima.size());
    CHECK((api("waveOutOpen", {l16(0), w16(0), l16(buf + 256), l16(0), l16(0), l16(1)}) & 0xFFFF) == 0,
          "pass %d: a format query succeeds as it always did", pass);
    CHECK(!rt.shims().find_name("MMSYSTEM", "waveOutWrite")->fn && !rt.shims().find_name("MMSYSTEM", "midiOutOpen")->fn,
          "pass %d: waveOutWrite/midiOutOpen stay unimplemented", pass);
    uint16_t err = 0;
    CHECK(rt.modules().load("MCISEQ.DRV", &err) == nullptr, "pass %d: no MCISEQ.DRV", pass);
    Module16* mm = rt.modules().load("MMSYSTEM.DLL", &err);
    CHECK(mm && rt.modules().proc_address(mm, "mixerGetNumDevs") != 0 &&
              rt.modules().proc_address(mm, "mixerSetControlDetails") != 0,
          "pass %d: the mixer API is there by name, as always", pass);
  }
}

// ---- devices and volumes ------------------------------------------------------------------------------------

void test_devices() {
  Rig g;
  uint32_t buf = uint32_t(g.data(512)) << 16;
  CHECK(audio16_enabled(g.rt), "enabled");
  CHECK(g.api16("MMSYSTEM", "waveOutGetNumDevs", {}) == 1, "one wave device");
  CHECK(g.api16("MMSYSTEM", "waveOutGetDevCaps", {w16(0), l16(buf), w16(48)}) == 0 &&
            g.rt.read_str(buf + 6) == "Long After Dark",
        "the wave device's name (%s)", g.rt.read_str(buf + 6).c_str());
  CHECK((g.rt.rd32(buf + 44) & 0x0C) == 0x0C && !(g.rt.rd32(buf + 44) & 0x0010), "VOLUME|LRVOLUME, not SYNC");
  CHECK(g.api16("MMSYSTEM", "midiOutGetNumDevs", {}) == 1, "one MIDI device");
  CHECK(g.api16("MMSYSTEM", "midiOutGetDevCaps", {w16(0), l16(buf), w16(50)}) == 0 &&
            g.rt.read_str(buf + 6) == "Long After Dark MIDI" && g.rt.rd32(buf + 46) == 3 && g.rt.rd16(buf + 44) == 0xFFFF,
        "the MIDI device's caps (%s)", g.rt.read_str(buf + 6).c_str());
  CHECK(g.api16("MMSYSTEM", "midiOutGetDevCaps", {w16(0xFFFF), l16(buf), w16(50)}) == 0 &&
            g.api16("MMSYSTEM", "midiOutGetDevCaps", {w16(1), l16(buf), w16(50)}) == 2,
        "MIDI_MAPPER answers, device 1 does not exist");
  CHECK(g.api16("MMSYSTEM", "auxGetNumDevs", {}) == 2, "two aux devices");
  CHECK(g.api16("MMSYSTEM", "auxGetDevCaps", {w16(0), l16(buf), w16(44)}) == 0 && g.rt.rd16(buf + 38) == 1 &&
            g.api16("MMSYSTEM", "auxGetDevCaps", {w16(1), l16(buf), w16(44)}) == 0 && g.rt.rd16(buf + 38) == 2 &&
            g.api16("MMSYSTEM", "auxGetDevCaps", {w16(2), l16(buf), w16(44)}) == 2,
        "aux 0 CD audio, aux 1 the MIDI bus, no aux 2");
  CHECK(g.api16("MMSYSTEM", "mixerGetNumDevs", {}) == 0, "no mixer");
  // Nor the mixer API by name, so AD_SND 3.2 takes its wave/MIDI volume path
  // (attach_audio16) instead of a mixer path that sets nothing.
  uint16_t mm = g.api16("KERNEL", "LoadLibrary", {l16(g.str("MMSYSTEM.DLL"))});
  CHECK(mm >= 32 && g.api("KERNEL", "GetProcAddress", {w16(mm), l16(g.str("mixerGetNumDevs"))}) == 0 &&
            g.api("KERNEL", "GetProcAddress", {w16(mm), l16(g.str("MIXERSETCONTROLDETAILS"))}) == 0 &&
            g.api("KERNEL", "GetProcAddress", {w16(mm), l16(g.str("waveOutSetVolume"))}) != 0,
        "the mixer API is not exported by name (MMSYSTEM %u)", mm);
  // The wave bus.
  CHECK(g.api16("MMSYSTEM", "waveOutSetVolume", {w16(0), l16(0x7FFF7FFF)}) == 0 &&
            g.fake.bus[0] == audio::gain_from_mm(0x7FFF7FFF),
        "waveOutSetVolume -> the wave bus gain");
  CHECK(g.api16("MMSYSTEM", "waveOutGetVolume", {w16(0), l16(buf)}) == 0 && g.rt.rd32(buf) == 0x7FFF7FFF,
        "waveOutGetVolume reads it back");
  // The MIDI bus: midiOut and aux 1 are one volume; aux 0 is its own.
  CHECK(g.api16("MMSYSTEM", "midiOutSetVolume", {w16(0), l16(0x40004000)}) == 0 &&
            g.fake.bus[1] == audio::gain_from_mm(0x40004000),
        "midiOutSetVolume -> the MIDI bus gain");
  CHECK(g.api16("MMSYSTEM", "auxGetVolume", {w16(1), l16(buf)}) == 0 && g.rt.rd32(buf) == 0x40004000,
        "aux 1 reads the MIDI volume");
  CHECK(g.api16("MMSYSTEM", "auxSetVolume", {w16(1), l16(0x20003000)}) == 0 &&
            g.fake.bus[1] == audio::gain_from_mm(0x20003000),
        "auxSetVolume(1) -> the MIDI bus gain");
  CHECK(g.api16("MMSYSTEM", "midiOutGetVolume", {w16(0), l16(buf)}) == 0 && g.rt.rd32(buf) == 0x20003000,
        "midiOutGetVolume reads aux 1's");
  CHECK(g.api16("MMSYSTEM", "auxSetVolume", {w16(0), l16(0x11112222)}) == 0 &&
            g.api16("MMSYSTEM", "auxGetVolume", {w16(0), l16(buf)}) == 0 && g.rt.rd32(buf) == 0x11112222 &&
            g.fake.bus[1] == audio::gain_from_mm(0x20003000),
        "aux 0 (CD) is stored and moves no bus");
  CHECK(g.api16("MMSYSTEM", "midiOutOpen", {l16(buf), w16(0), l16(0), l16(0), l16(0)}) == 8 &&
            g.api16("MMSYSTEM", "midiOutShortMsg", {w16(1), l16(0x90)}) == 8,
        "the rest of midiOut: MMSYSERR_NOTSUPPORTED");

  // ADSOUNDDEV=0: no wave device, sound on or off; the MIDI device stays.
  Rig n(true, false);
  uint32_t nb = uint32_t(n.data(512)) << 16;
  std::vector<uint8_t> img = pcm8(11025, 100);
  n.rt.write_bytes(nb, img.data(), img.size());
  CHECK(n.api16("MMSYSTEM", "waveOutGetNumDevs", {}) == 0 && n.api16("MMSYSTEM", "sndPlaySound", {l16(nb), w16(7)}) == 0 &&
            n.fake.voices.empty() && n.api16("MMSYSTEM", "midiOutGetNumDevs", {}) == 1,
        "ADSOUNDDEV=0: no wave device, sndPlaySound FALSE, MIDI still there");
}

// ---- sndPlaySound -------------------------------------------------------------------------------------------

void test_snd() {
  Rig g;
  // 11025 frames of 8-bit mono at 22050 Hz: 500 ms.
  std::vector<uint8_t> img = pcm8(22050, 11025);
  const size_t at = size_t(std::search(img.begin(), img.end(), std::begin("data"), std::begin("data") + 4) - img.begin()) + 8;
  uint32_t p = g.bytes(img);
  uint64_t t0 = g.now();
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(p), w16(0x0007)}) == 1, "SND_ASYNC|SND_NODEFAULT|SND_MEMORY plays");
  const FakeEngine::Voice* v = g.fake.only_voice();
  CHECK(v && v->on && !v->loop && v->bus == audio::Bus::wave, "one voice on the wave bus, playing once");
  if (v) {
    const FakeEngine::Buf& b = g.fake.bufs.at(v->b);
    CHECK(b.f == audio::pcm_format(22050, 1, 8) && b.bytes.size() == 11025 &&
              std::equal(b.bytes.begin(), b.bytes.end(), img.begin() + at) && b.released,
          "the buffer holds the image's samples (%zu bytes) and is released to its voice", b.bytes.size());
    CHECK(g.fake.voice_end(*v) - v->t0 == 500000, "it lasts 500 ms");
  }
  CHECK(g.now() - t0 < 1000, "SND_ASYNC returns at once (%llu us)", (unsigned long long)(g.now() - t0));
  // The copy was made at the call: AD_SND unlocks (and may discard) right after.
  std::vector<uint8_t> junk(img.size(), 0x11);
  g.rt.write_bytes(p, junk.data(), junk.size());
  CHECK(v && g.fake.bufs.at(v->b).bytes[100] == img[at + 100], "the guest's later writes do not reach the sound");
  g.rt.write_bytes(p, img.data(), img.size());
  // SND_NOSTOP while it plays: FALSE, the sound goes on.
  uint32_t first = g.fake.voices.begin()->first;
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(p), w16(0x0017)}) == 0 && g.fake.voices.count(first),
        "SND_NOSTOP while playing: FALSE");
  g.to(t0 + 500000 + 1000);
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(p), w16(0x0017)}) == 1 && !g.fake.voices.count(first) &&
            g.fake.live_voices() == 1,
        "SND_NOSTOP after the end: plays, on a new voice");
  // A new call replaces the sound; NULL stops it.
  uint32_t second = g.fake.voices.begin()->first;
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(p), w16(0x000F)}) == 1 && !g.fake.voices.count(second) &&
            g.fake.only_voice() && g.fake.only_voice()->loop,
        "SND_LOOP: the new sound replaces the old one and loops");
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(0), w16(0)}) == 1 && g.fake.voices.empty(), "NULL stops (TRUE)");
  // Undecodable and not RIFF: FALSE.
  std::vector<uint8_t> mp3fmt = {0x55, 0x00, 0x01, 0x00, 0x22, 0x56, 0, 0, 0x00, 0x04, 0, 0, 0, 0, 0x04, 0, 0, 0};
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(g.bytes(riff(mp3fmt, std::vector<uint8_t>(256, 0)))), w16(7)}) == 0 &&
            g.fake.voices.empty(),
        "an undecodable format: FALSE");
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(g.bytes(std::vector<uint8_t>(64, 'x'))), w16(7)}) == 0,
        "not a RIFF image: FALSE");

  // MS-ADPCM (the Totally Twisted banks) and IMA-ADPCM: decoded into one PCM16 buffer.
  for (audio::WaveFormat f : {audio::ms_adpcm_format(11025, 1), audio::ima_adpcm_format(22050, 1)}) {
    std::vector<uint8_t> data(size_t(f.block_align) * 3);
    for (size_t i = 0; i < data.size(); i++) data[i] = uint8_t(i * 37 + 11);
    for (size_t blk = 0; blk < 3; blk++) {
      uint8_t* h = data.data() + blk * f.block_align;
      if (f.tag == audio::kTagMsAdpcm) {
        h[0] = uint8_t(blk % 7);  // predictor
        h[1] = 0x10, h[2] = 0x00;  // delta 16
      } else {
        h[2] = uint8_t(blk * 11);  // step index
        h[3] = 0;
      }
    }
    std::vector<uint8_t> expect = audio::decode(f, data);
    CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(g.bytes(riff(audio::waveformat_bytes(f), data))), w16(7)}) == 1,
          "tag %u plays", f.tag);
    const FakeEngine::Voice* av = g.fake.only_voice();
    CHECK(av && g.fake.bufs.at(av->b).f == audio::decoded_format(f) && g.fake.bufs.at(av->b).bytes == expect &&
              !expect.empty(),
          "tag %u: the buffer is audio::decode's PCM16 (%zu bytes)", f.tag, expect.size());
  }
  g.api("MMSYSTEM", "sndPlaySound", {l16(0), w16(0)});

  // Synchronous (NOCTURNE: SND_NODEFAULT|SND_MEMORY): the call lasts the sound,
  // in virtual time, charged at once without a yield hook ...
  uint64_t s0 = g.now();
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(p), w16(0x0006)}) == 1, "synchronous plays");
  const FakeEngine::Voice* sv = g.fake.only_voice();
  CHECK(sv && g.now() >= g.fake.voice_end(*sv) && g.now() - s0 < 500000 + 1000,
        "the call returned at the sound's end (%llu us after %llu)", (unsigned long long)(g.now() - s0),
        (unsigned long long)s0);
  // ... or frame by frame through the lane's yield hook (a long call).
  int yields = 0;
  g.rt.set_yield_hook([&] {
    yields++;
    g.clock.begin_frame();
    return true;
  });
  g.to(g.now() + 10000);
  uint64_t y0 = g.now();
  g.api("MMSYSTEM", "sndPlaySound", {l16(p), w16(0x0006)});
  CHECK(yields >= 499 && yields <= 501 && g.now() >= y0 + 500000, "the lane ended %d 1-ms frames inside the call",
        yields);
  g.rt.set_yield_hook(nullptr);

  // By name: a file (the WINDOWS directory, ".WAV" assumed), a WIN.INI [sounds] entry.
  g.rt.vfs().add_virtual_file("C:\\WINDOWS\\DING.WAV", pcm8(11025, 1103));
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(g.str("DING.WAV")), w16(1)}) == 1 &&
            g.fake.bufs.at(g.fake.only_voice()->b).bytes.size() == 1103,
        "a file name in the WINDOWS directory");
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(g.str("C:\\WINDOWS\\DING")), w16(1)}) == 1, "a path, .WAV assumed");
  profiles16(g.rt).add_seed("C:\\WINDOWS\\WIN.INI", "sounds", "SystemAsterisk", "ding.wav,Asterisk");
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(g.str("SystemAsterisk")), w16(1)}) == 1,
        "a WIN.INI [sounds] alias");
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(g.str("NOSUCH.WAV")), w16(1)}) == 0, "a missing sound: FALSE");
}

// ---- waveOut ---------------------------------------------------------------------------------------------

// A WAVEHDR at `hdr` over `bytes` bytes of data.
void make_header(Rig& g, uint32_t hdr, uint32_t data, uint32_t bytes) {
  std::vector<uint8_t> z(32, 0);
  g.rt.write_bytes(hdr, z.data(), z.size());
  g.rt.wr32(hdr, data);
  g.rt.wr32(hdr + 4, bytes);
}

void test_waveout() {
  Rig g;
  uint32_t mem = uint32_t(g.data(0x8000)) << 16;
  uint32_t fmt = mem, phwo = mem + 0x40, hdr1 = mem + 0x80, hdr2 = mem + 0xC0, mmt = mem + 0x100, data = mem + 0x200;
  auto put_fmt = [&](const audio::WaveFormat& f) {
    std::vector<uint8_t> b = audio::waveformat_bytes(f);
    g.rt.write_bytes(fmt, b.data(), b.size());
  };
  auto open = [&](uint16_t dev, uint32_t cb, uint32_t inst, uint32_t flags) {
    return g.api16("MMSYSTEM", "waveOutOpen", {l16(phwo), w16(dev), l16(fmt), l16(cb), l16(inst), l16(flags)});
  };
  // WAVE_FORMAT_QUERY: PCM anywhere; ADPCM on WAVE_MAPPER only.
  put_fmt(audio::pcm_format(22050, 1, 16));
  CHECK(open(0, 0, 0, 1) == 0, "PCM16 query on device 0");
  put_fmt(audio::ima_adpcm_format(22050, 1));
  CHECK(open(0, 0, 0, 1) == 32 && open(0xFFFF, 0, 0, 1) == 0, "IMA-ADPCM: device 0 refuses, the mapper accepts");
  put_fmt(audio::ms_adpcm_format(11025, 1));
  CHECK(open(0xFFFF, 0, 0, 1) == 0, "MS-ADPCM on the mapper");
  std::vector<uint8_t> mp3 = {0x55, 0x00, 0x01, 0x00, 0x22, 0x56, 0, 0, 0x00, 0x04, 0, 0, 0, 0, 0x04, 0, 0, 0};
  g.rt.write_bytes(fmt, mp3.data(), mp3.size());
  CHECK(open(0xFFFF, 0, 0, 1) == 32, "MPEG: WAVERR_BADFORMAT");
  CHECK(open(3, 0, 0, 1) == 2, "no device 3");
  CHECK(g.fake.streams.empty(), "queries open nothing");

  // A real stream: 22050 Hz 16-bit mono, CALLBACK_NULL.
  put_fmt(audio::pcm_format(22050, 1, 16));
  CHECK(open(0, 0, 0, 0) == 0, "open");
  uint16_t h = g.rt.rd16(phwo);
  CHECK(h >= 0xE000 && g.fake.streams.size() == 1, "a handle (%04X) and one engine stream", h);
  make_header(g, hdr1, data, 4410);         // 2205 frames: 100 ms
  make_header(g, hdr2, data + 4410, 4410);  // another 100 ms
  CHECK(g.api16("MMSYSTEM", "waveOutWrite", {w16(h), l16(hdr1), w16(32)}) == 34, "unprepared: WAVERR_UNPREPARED");
  for (uint32_t hd : {hdr1, hdr2}) g.api("MMSYSTEM", "waveOutPrepareHeader", {w16(h), l16(hd), w16(32)});
  CHECK(g.rt.rd32(hdr1 + 16) == 2, "WHDR_PREPARED");
  uint64_t t0 = g.now();
  CHECK(g.api16("MMSYSTEM", "waveOutWrite", {w16(h), l16(hdr1), w16(32)}) == 0 &&
            g.api16("MMSYSTEM", "waveOutWrite", {w16(h), l16(hdr2), w16(32)}) == 0,
        "two chunks written");
  CHECK((g.rt.rd32(hdr1 + 16) & 0x13) == 0x12, "INQUEUE|PREPARED, not DONE (%X)", g.rt.rd32(hdr1 + 16));
  CHECK(g.api16("MMSYSTEM", "waveOutWrite", {w16(h), l16(hdr1), w16(32)}) == 33, "a queued header: STILLPLAYING");
  CHECK(g.api16("MMSYSTEM", "waveOutUnprepareHeader", {w16(h), l16(hdr1), w16(32)}) == 33 &&
            g.api16("MMSYSTEM", "waveOutClose", {w16(h)}) == 33,
        "unprepare/close while queued: WAVERR_STILLPLAYING");
  // WHDR_DONE exactly when virtual time passes the chunk's end.
  g.to(t0 + 98000);
  g.tick();
  CHECK(!(g.rt.rd32(hdr1 + 16) & 1), "not done at 98 ms");
  g.to(t0 + 100000 + 1000);
  g.tick();
  CHECK((g.rt.rd32(hdr1 + 16) & 0x11) == 0x01 && (g.rt.rd32(hdr2 + 16) & 0x11) == 0x10,
        "the first done after 100 ms, the second still queued (%X %X)", g.rt.rd32(hdr1 + 16), g.rt.rd32(hdr2 + 16));
  g.rt.wr16(mmt, 4);
  CHECK(g.api16("MMSYSTEM", "waveOutGetPosition", {w16(h), l16(mmt), w16(8)}) == 0 && g.rt.rd16(mmt) == 4 &&
            g.rt.rd32(mmt + 2) >= 4410 && g.rt.rd32(mmt + 2) < 4410 + 200,
        "TIME_BYTES position (%u)", g.rt.rd32(mmt + 2));
  g.rt.wr16(mmt, 1);
  g.api("MMSYSTEM", "waveOutGetPosition", {w16(h), l16(mmt), w16(8)});
  CHECK(g.rt.rd16(mmt) == 1 && g.rt.rd32(mmt + 2) >= 100 && g.rt.rd32(mmt + 2) <= 102, "TIME_MS position (%u)",
        g.rt.rd32(mmt + 2));
  // Pause holds the queue; Reset completes everything at once.
  g.api("MMSYSTEM", "waveOutPause", {w16(h)});
  g.to(g.now() + 300000);
  g.tick();
  CHECK(g.rt.rd32(hdr2 + 16) & 0x10, "paused: the second chunk is still queued");
  CHECK(g.api16("MMSYSTEM", "waveOutReset", {w16(h)}) == 0 && (g.rt.rd32(hdr2 + 16) & 0x11) == 0x01,
        "reset: done when it returns");
  CHECK(g.api16("MMSYSTEM", "waveOutUnprepareHeader", {w16(h), l16(hdr2), w16(32)}) == 0 && g.rt.rd32(hdr2 + 16) == 1,
        "unprepared");
  CHECK(g.api16("MMSYSTEM", "waveOutClose", {w16(h)}) == 0 && g.fake.streams.empty(), "closed");
  CHECK(g.api16("MMSYSTEM", "waveOutClose", {w16(h)}) == 5, "a closed handle: MMSYSERR_INVALHANDLE");

  // ADPCM through the mapper: each chunk decoded as it is written.
  put_fmt(audio::ima_adpcm_format(22050, 1));
  CHECK(open(0xFFFF, 0, 0, 0) == 0, "an IMA-ADPCM stream on the mapper");
  uint16_t ha = g.rt.rd16(phwo);
  make_header(g, hdr1, data, 1024);  // two 512-byte blocks: 2034 frames
  g.api("MMSYSTEM", "waveOutPrepareHeader", {w16(ha), l16(hdr1), w16(32)});
  g.fake.calls.clear();
  g.api("MMSYSTEM", "waveOutWrite", {w16(ha), l16(hdr1), w16(32)});
  CHECK(!g.fake.calls.empty() && g.fake.calls.back() == "stream_write 4068", "the engine got PCM16 (%s)",
        g.fake.calls.empty() ? "" : g.fake.calls.back().c_str());
  g.api("MMSYSTEM", "waveOutReset", {w16(ha)});
  g.api("MMSYSTEM", "waveOutClose", {w16(ha)});
}

// MM_WOM_OPEN/DONE/CLOSE to a window: posted at their time, dispatched by the
// lane's pump unless the guest takes them itself.
void test_wom_window() {
  Rig g;
  uint16_t rec = g.data(512);
  uint16_t hwnd = make_window(g, rec);
  CHECK(hwnd != 0, "a window");
  uint32_t mem = uint32_t(g.data(0x4000)) << 16;
  uint32_t fmt = mem, phwo = mem + 0x40, hdr = mem + 0x80, msg = mem + 0xC0, data = mem + 0x200;
  std::vector<uint8_t> b = audio::waveformat_bytes(audio::pcm_format(11025, 1, 8));
  g.rt.write_bytes(fmt, b.data(), b.size());
  CHECK(g.api16("MMSYSTEM", "waveOutOpen", {l16(phwo), w16(0), l16(fmt), l16(hwnd), l16(0), l16(0x00010000)}) == 0,
        "CALLBACK_WINDOW open");
  uint16_t h = g.rt.rd16(phwo);
  audio16_pump(g.rt);
  make_header(g, hdr, data, 1103);  // 100 ms
  g.api("MMSYSTEM", "waveOutPrepareHeader", {w16(h), l16(hdr), w16(32)});
  uint64_t t0 = g.now();
  g.api("MMSYSTEM", "waveOutWrite", {w16(h), l16(hdr), w16(32)});
  audio16_pump(g.rt);
  CHECK(records_text(window_records(g, rec)) == "03BB/" + [&] {
          char s[8];
          snprintf(s, sizeof(s), "%X", h);
          return std::string(s);
        }() + "/0",
        "MM_WOM_OPEN dispatched by the pump: %s", records_text(window_records(g, rec)).c_str());
  g.to(t0 + 99000);
  audio16_pump(g.rt);
  CHECK(window_records(g, rec).size() == 1, "nothing more before the chunk's end");
  g.to(t0 + 101000);
  audio16_pump(g.rt);
  std::vector<Rec> r = window_records(g, rec);
  CHECK(r.size() == 2 && r[1].msg == 0x3BD && r[1].wp == h && r[1].lp == hdr, "MM_WOM_DONE(hwo, lpWaveHdr) at 100 ms: %s",
        records_text(r).c_str());
  // The guest pumping its own queue takes the message there.
  g.api("MMSYSTEM", "waveOutWrite", {w16(h), l16(hdr), w16(32)});
  g.to(g.now() + 101000);
  g.tick();
  CHECK(g.api16("USER", "PeekMessage", {l16(msg), w16(hwnd), w16(0x3BB), w16(0x3BD), w16(1)}) == 1 &&
            g.rt.rd16(msg + 2) == 0x3BD && g.rt.rd32(msg + 6) == hdr,
        "PeekMessage(PM_REMOVE) takes MM_WOM_DONE");
  audio16_pump(g.rt);
  CHECK(window_records(g, rec).size() == 2, "the pump does not dispatch it again");
  CHECK(g.api16("MMSYSTEM", "waveOutClose", {w16(h)}) == 0, "close");
  audio16_pump(g.rt);
  r = window_records(g, rec);
  CHECK(r.size() == 3 && r[2].msg == 0x3BC, "MM_WOM_CLOSE last: %s", records_text(r).c_str());
}

// CALLBACK_FUNCTION: called at the first API call at or after the event's
// time, in order, never re-entrantly.
void test_wom_function() {
  Rig g;
  uint16_t rec = g.data(512);
  uint32_t tgt = g.rt.thunk_far(*g.rt.shims().find_name("MMSYSTEM", "timeGetTime"));
  uint16_t cs = g.code(function_recorder(rec, tgt));
  uint32_t mem = uint32_t(g.data(0x4000)) << 16;
  uint32_t fmt = mem, phwo = mem + 0x40, hdr1 = mem + 0x80, hdr2 = mem + 0xC0, data = mem + 0x200;
  std::vector<uint8_t> b = audio::waveformat_bytes(audio::pcm_format(11025, 1, 8));
  g.rt.write_bytes(fmt, b.data(), b.size());
  auto recs = [&] {
    std::vector<std::array<uint16_t, 4>> out;
    uint32_t base = uint32_t(rec) << 16;
    for (uint16_t i = 0; i < g.rt.rd16(base); i++) {
      uint32_t e = base + 8 + 8u * i;
      out.push_back({g.rt.rd16(e), g.rt.rd16(e + 2), g.rt.rd16(e + 4), g.rt.rd16(e + 6)});
    }
    return out;
  };
  CHECK(g.api16("MMSYSTEM", "waveOutOpen",
                {l16(phwo), w16(0), l16(fmt), l16(uint32_t(cs) << 16), l16(0x1234), l16(0x00030000)}) == 0,
        "CALLBACK_FUNCTION open");
  uint16_t h = g.rt.rd16(phwo);
  CHECK(recs().empty(), "nothing is called inside waveOutOpen");
  make_header(g, hdr1, data, 551);  // 50 ms
  make_header(g, hdr2, data, 551);
  uint64_t t0 = g.now();
  for (uint32_t hd : {hdr1, hdr2}) {
    g.api("MMSYSTEM", "waveOutPrepareHeader", {w16(h), l16(hd), w16(32)});
    g.api("MMSYSTEM", "waveOutWrite", {w16(h), l16(hd), w16(32)});
  }
  auto r = recs();
  CHECK(r.size() == 1 && r[0][0] == 0x3BB && r[0][1] == h, "WOM_OPEN at the first API call after the open (%zu)",
        r.size());
  // Both chunks end while no API call happens: both DONEs at the next one, in order.
  g.to(t0 + 120000);
  CHECK(recs().size() == 1, "nothing without an API call");
  g.tick();
  r = recs();
  CHECK(r.size() == 3 && r[1][0] == 0x3BD && r[1][2] == uint16_t(hdr1) && r[2][0] == 0x3BD && r[2][2] == uint16_t(hdr2),
        "WOM_DONE for each header, in order (%zu)", r.size());
  bool flat = true;
  for (const auto& x : r) flat &= x[3] == 1;
  CHECK(flat, "never re-entered: the callback's own API call delivered nothing");
  // The instance and the DS: the callback ran with its module's (none here: the caller's).
  g.api("MMSYSTEM", "waveOutClose", {w16(h)});
  g.tick();
  r = recs();
  CHECK(r.size() == 4 && r[3][0] == 0x3BC, "WOM_CLOSE after the DONEs");
}

// ---- MCI -------------------------------------------------------------------------------------------------

void test_mci() {
  Rig g;
  g.fake.song_length_us = 2'000'000;
  win32::Vfs& vfs = g.rt.vfs();
  vfs.mount_overlay("C:\\AFTERDRK", "", "");
  std::vector<uint8_t> smf = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96};
  vfs.add_virtual_file("C:\\AFTERDRK\\SONG.MID", smf);
  vfs.add_virtual_file("C:\\AFTERDRK\\JUNK.MID", std::vector<uint8_t>(32, 'j'));
  vfs.set_cwd("C:\\AFTERDRK");
  uint16_t rec = g.data(512);
  uint16_t hwnd = make_window(g, rec);
  std::string ret;
  // The engines' init probe: a device-only open, closed again.
  CHECK(g.mci("open sequencer", &ret) == 0 && ret == "1", "open sequencer -> device 1 (%s)", ret.c_str());
  CHECK(g.mci("open sequencer") == 265, "open sequencer again: MCIERR_DEVICE_OPEN");
  CHECK(g.mci("close sequencer") == 0 && g.fake.songs.empty(), "close sequencer");
  // "open sequencer!%s alias fred wait" (ADXPL310 DGROUP 0x477).
  CHECK(g.mci("open sequencer!C:\\AFTERDRK\\SONG.MID alias fred wait", &ret) == 0 && ret == "1" &&
            g.fake.songs.size() == 1,
        "open sequencer!path alias fred wait -> %s", ret.c_str());
  CHECK(g.mci("open sequencer!SONG.MID alias FRED") == 289, "a second fred: MCIERR_DUPLICATE_ALIAS");
  CHECK(g.mci("status fred length", &ret) == 0 && ret == "2000", "length 2000 ms (%s)", ret.c_str());
  CHECK(g.mci("status Fred mode", &ret) == 0 && ret == "stopped", "mode stopped (%s)", ret.c_str());
  CHECK(g.mci("status fred position", &ret) == 0 && ret == "0", "position 0");
  CHECK(g.mci("status fred ready", &ret) == 0 && ret == "true", "ready");
  CHECK(g.mci("status fred bogus") == 259 && g.mci("status fred") == 273, "unknown / missing status items");
  CHECK(g.mci("set fred time format milliseconds") == 0 && g.mci("set fred time format ms") == 0 &&
            g.mci("set fred time format frames") == 293,
        "time format ms only");
  CHECK(g.mci("play fred wait") == 274, "play ... wait: MCIERR_UNSUPPORTED_FUNCTION");
  // play ... notify: MM_MCINOTIFY(SUCCESSFUL, id) at the song's end, to hwndCallback.
  uint64_t t0 = g.now();
  CHECK(g.mci("play fred notify", &ret, hwnd) == 0 && ret.empty(), "play fred notify");
  CHECK(g.mci("status fred mode", &ret) == 0 && ret == "playing", "mode playing (%s)", ret.c_str());
  g.to(t0 + 1990000);
  g.tick();
  audio16_pump(g.rt);
  CHECK(window_records(g, rec).empty(), "no notify before the end");
  g.to(t0 + 2000000 + 1000);
  g.tick();
  audio16_pump(g.rt);
  std::vector<Rec> r = window_records(g, rec);
  CHECK(r.size() == 1 && r[0].msg == 0x3B9 && r[0].wp == 1 && r[0].lp == 1,
        "MM_MCINOTIFY(SUCCESSFUL, device 1) at 2 s, dispatched by the pump: %s", records_text(r).c_str());
  CHECK(g.mci("status fred mode", &ret) == 0 && ret == "stopped" && g.mci("status fred position", &ret) == 0 &&
            ret == "2000",
        "stopped at the end (%s)", ret.c_str());
  // The engines' loop: seek to start, play notify. A second notify supersedes the first.
  clear_records(g, rec);
  CHECK(g.mci("seek fred to start wait") == 0 && g.mci("play fred notify", nullptr, hwnd) == 0 &&
            g.mci("play fred notify", nullptr, hwnd) == 0,
        "seek, play notify twice");
  audio16_pump(g.rt);
  r = window_records(g, rec);
  CHECK(r.size() == 1 && r[0].wp == 2, "the first play's notify: SUPERSEDED: %s", records_text(r).c_str());
  // stop ... notify: the play's ABORTED first, then the stop's own SUCCESSFUL.
  clear_records(g, rec);
  CHECK(g.mci("stop fred notify", nullptr, hwnd) == 0, "stop notify");
  audio16_pump(g.rt);
  r = window_records(g, rec);
  CHECK(r.size() == 2 && r[0].wp == 4 && r[1].wp == 1, "ABORTED, then the stop's SUCCESSFUL: %s",
        records_text(r).c_str());
  // play from/to: SUCCESSFUL when the song reaches `to`, stopped there.
  clear_records(g, rec);
  uint64_t t1 = g.now();
  CHECK(g.mci("play fred from 500 to 1000 notify", nullptr, hwnd) == 0, "play from 500 to 1000 notify");
  g.to(t1 + 490000);
  g.tick();
  audio16_pump(g.rt);
  CHECK(window_records(g, rec).empty(), "not yet at 490 ms");
  g.to(t1 + 501000);
  g.tick();
  audio16_pump(g.rt);
  r = window_records(g, rec);
  CHECK(r.size() == 1 && r[0].wp == 1 && g.mci("status fred mode", &ret) == 0 && ret == "stopped" &&
            g.mci("status fred position", &ret) == 0 && ret == "1000",
        "SUCCESSFUL at `to`, stopped at 1000 ms (%s)", ret.c_str());
  CHECK(g.mci("play fred from 3000") == 282, "from past the end: MCIERR_OUTOFRANGE");
  // close with a pending notify: ABORTED.
  clear_records(g, rec);
  g.mci("play fred notify", nullptr, hwnd);
  CHECK(g.mci("close fred") == 0 && g.fake.songs.empty(), "close fred");
  audio16_pump(g.rt);
  r = window_records(g, rec);
  CHECK(r.size() == 1 && r[0].wp == 4, "close: ABORTED: %s", records_text(r).c_str());
  // Errors.
  CHECK(g.mci("open NOSUCH.MID type sequencer alias x") == 275, "a missing file: MCIERR_FILE_NOT_FOUND");
  CHECK(g.mci("open JUNK.MID type sequencer alias x") == 296, "not an SMF: MCIERR_INVALID_FILE");
  CHECK(g.mci("open cdaudio alias qwanza wait") == 306 && g.mci("open waveaudio!x.wav") == 306,
        "other devices: MCIERR_DEVICE_NOT_INSTALLED");
  CHECK(g.mci("open") == 292, "open alone: MCIERR_MISSING_DEVICE_NAME");
  CHECK(g.mci("pause fred") == 261 && g.mci("") == 267, "unknown verb / empty: %u", g.mci(""));
  CHECK(g.mci("play nobody") == 263, "an unknown device: MCIERR_INVALID_DEVICE_NAME");
  CHECK(g.mci("open \"C:\\AFTERDRK\\SONG.MID") == 294, "an unclosed quote");
  // The other open forms, and close all.
  CHECK(g.mci("open SONG.MID alias a", &ret) == 0 && ret == "1", "open by extension");
  CHECK(g.mci("open \"C:\\AFTERDRK\\SONG.MID\" type Sequencer alias b", &ret) == 0 && ret == "2",
        "open <quoted path> type sequencer alias b");
  CHECK(g.mci("status b length", &ret, 0, 3) == 268 && ret == "20", "a short return buffer: overflow, NUL-terminated (%s)",
        ret.c_str());
  CHECK(g.mci("close all wait") == 0 && g.fake.songs.empty() && g.mci("status a mode") == 263, "close all");
}

// The ADXPL3xx/40 music gates (AUDIO.md §2.9, §8.5).
void test_gates() {
  Rig g;
  uint16_t th = g.api16("KERNEL", "LoadLibrary", {l16(g.str("TOOLHELP.DLL"))});
  uint16_t mh = g.api16("KERNEL", "LoadLibrary", {l16(g.str("MCISEQ.DRV"))});
  CHECK(th >= 32 && mh >= 32, "TOOLHELP.DLL (%u) and MCISEQ.DRV (%u) load", th, mh);
  CHECK(g.api("KERNEL", "GetProcAddress", {w16(th), l16(g.str("GLOBALFIRST"))}) != 0 &&
            g.api("KERNEL", "GetProcAddress", {w16(th), l16(g.str("GLOBALNEXT"))}) != 0,
        "GetProcAddress(TOOLHELP, GLOBALFIRST/GLOBALNEXT)");
  CHECK((g.api("KERNEL", "GetModuleHandle", {l16(mh)}) & 0xFFFF) != 0, "GetModuleHandle(MAKELONG(hMciSeq, 0))");
  g.api("KERNEL", "FreeLibrary", {w16(mh)});
  CHECK(g.api16("KERNEL", "LoadLibrary", {l16(g.str("MCISEQ.DRV"))}) >= 32, "MCISEQ.DRV reloads (every 100 songs)");
  CHECK(g.api16("MMSYSTEM", "midiOutGetNumDevs", {}) >= 1, "IsMusicAvail: a MIDI output device");
}

// ---- the real engine ------------------------------------------------------------------------------------------

// A format-0 SMF: 480 PPQN, 500000 µs per quarter, a note from 0 to 480
// ticks, end of track at 960 ticks (1 s).
std::vector<uint8_t> one_second_smf() {
  std::vector<uint8_t> trk = {0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,   // tempo 500000
                              0x00, 0x90, 60, 64,                       // note on
                              0x83, 0x60, 0x80, 60, 0,                   // +480: note off
                              0x83, 0x60, 0xFF, 0x2F, 0x00};             // +480: end of track
  std::vector<uint8_t> f = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k'};
  f.push_back(0), f.push_back(0), f.push_back(uint8_t(trk.size() >> 8)), f.push_back(uint8_t(trk.size()));
  append(f, trk);
  return f;
}

void test_real_engine() {
  audio::Config cfg;
  cfg.guest_sound = true;
  std::unique_ptr<audio::Engine> engine = audio::make_engine(cfg);
  CHECK(engine && engine->enabled(), "make_engine(guest_sound)");
  if (!engine) return;
  Rig g(true, true, engine.get());
  // A 11025-frame 22050 Hz sound: 500 ms, charged in full by a synchronous call.
  uint32_t p = g.bytes(pcm8(22050, 11025));
  g.to(1000);
  uint64_t t0 = g.now();
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(p), w16(0x0006)}) == 1, "synchronous, real engine");
  CHECK(g.now() >= t0 + 500000 && g.now() < t0 + 501000, "returned 500 ms later (%llu us)",
        (unsigned long long)(g.now() - t0));
  // MS-ADPCM through the real decoder.
  audio::WaveFormat ms = audio::ms_adpcm_format(11025, 1);
  std::vector<uint8_t> data(size_t(ms.block_align) * 2, 0x5A);
  data[0] = 0, data[1] = 16, data[2] = 0, data[256] = 1, data[257] = 16, data[258] = 0;
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(g.bytes(riff(audio::waveformat_bytes(ms), data))), w16(7)}) == 1,
        "MS-ADPCM plays on the real engine");
  g.api("MMSYSTEM", "sndPlaySound", {l16(0), w16(0)});
  // A real song: MM_MCINOTIFY at its 1 s end.
  g.rt.vfs().mount_overlay("C:\\AFTERDRK", "", "");
  g.rt.vfs().add_virtual_file("C:\\AFTERDRK\\ONE.MID", one_second_smf());
  uint16_t rec = g.data(512);
  uint16_t hwnd = make_window(g, rec);
  std::string ret;
  CHECK(g.mci("open sequencer!C:\\AFTERDRK\\ONE.MID alias fred wait", &ret) == 0 &&
            g.mci("status fred length", &ret) == 0 && ret == "1000",
        "a real SMF opens, 1000 ms (%s)", ret.c_str());
  uint64_t t1 = g.now();
  g.mci("play fred notify", nullptr, hwnd);
  g.to(t1 + 990000);
  g.tick();
  audio16_pump(g.rt);
  CHECK(window_records(g, rec).empty(), "no notify at 990 ms");
  g.to(t1 + 1001000);
  g.tick();
  audio16_pump(g.rt);
  std::vector<Rec> r = window_records(g, rec);
  CHECK(r.size() == 1 && r[0].msg == 0x3B9 && r[0].wp == 1, "MM_MCINOTIFY(SUCCESSFUL) at 1 s: %s",
        records_text(r).c_str());
  g.mci("close all");
  audio16_close(g.rt);
  engine->shutdown(g.now());
}

}  // namespace

int main() {
  // AD_TEST_TRACE=sound,user16: the shims' trace lines.
  if (const char* t = getenv("AD_TEST_TRACE")) {
    std::set<std::string> cats;
    for (std::string c; *t; t++) {
      if (*t != ',') c.push_back(*t);
      if (*t == ',' || !t[1]) cats.insert(c), c.clear();
    }
    set_trace_categories(cats);
  }
  try {
    test_disabled();
    test_devices();
    test_snd();
    test_waveout();
    test_wom_window();
    test_wom_function();
    test_mci();
    test_gates();
    test_real_engine();
  } catch (const std::exception& e) {
    printf("FAIL: exception %s\n", e.what());
    return 1;
  }
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}
