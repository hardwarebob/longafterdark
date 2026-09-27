// Standard MIDI File parsing for the engine's song player (AUDIO.md §6.4).
#include <algorithm>
#include <cstring>

#include "audio_internal.h"

namespace adw::audio {

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

// A RIFF RMID file wraps the SMF in its "data" chunk.
std::span<const uint8_t> unwrap_rmid(std::span<const uint8_t> b) {
  if (b.size() < 12 || memcmp(b.data(), "RIFF", 4) != 0 || memcmp(b.data() + 8, "RMID", 4) != 0) return b;
  size_t pos = 12;
  while (pos + 8 <= b.size()) {
    uint32_t size = le32(b.data() + pos + 4);
    if (memcmp(b.data() + pos, "data", 4) == 0) return b.subspan(pos + 8, std::min<size_t>(size, b.size() - pos - 8));
    size_t next = pos + 8 + size_t(size) + (size & 1);
    if (next <= pos) break;
    pos = next;
  }
  return {};
}

struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  bool vlq(uint32_t& v) {
    v = 0;
    for (int i = 0; i < 4; i++) {
      if (p >= end) return false;
      uint8_t b = *p++;
      v = (v << 7) | (b & 0x7F);
      if (!(b & 0x80)) return true;
    }
    return false;  // more than 4 bytes
  }
};

}  // namespace

void put_vlq(std::vector<uint8_t>& out, uint32_t v) {
  uint8_t tmp[5];
  int n = 0;
  tmp[n++] = uint8_t(v & 0x7F);
  while (v >>= 7) tmp[n++] = uint8_t(0x80 | (v & 0x7F));
  while (n) out.push_back(tmp[--n]);
}

uint16_t SmfSong::channels_used() const {
  uint16_t m = 0;
  for (const SmfEvent& e : events)
    if (e.is_channel()) m |= uint16_t(1u << e.channel());
  return m;
}

std::array<uint32_t, 16> SmfSong::note_ons() const {
  std::array<uint32_t, 16> n{};
  for (const SmfEvent& e : events)
    if (e.is_channel() && e.type() == 0x90 && e.d2 > 0) n[e.channel()]++;
  return n;
}

bool parse_smf(std::span<const uint8_t> bytes, SmfSong& out, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  bytes = unwrap_rmid(bytes);
  if (bytes.size() < 14 || memcmp(bytes.data(), "MThd", 4) != 0) return fail("not a Standard MIDI File");
  uint32_t hlen = be32(bytes.data() + 4);
  if (hlen < 6 || size_t(8) + hlen > bytes.size()) return fail("truncated MThd header");
  SmfSong song;
  song.format = be16(bytes.data() + 8);
  uint16_t ntrks = be16(bytes.data() + 10);
  song.division = int16_t(be16(bytes.data() + 12));
  if (song.format > 2) return fail("unknown SMF format");
  if (song.format == 2) return fail("SMF format 2 (independent sequences) is not supported");
  if (song.division == 0) return fail("SMF division 0");
  int smpte_fps = 0, smpte_tpf = 0;
  if (song.division < 0) {
    smpte_fps = -int8_t(uint16_t(song.division) >> 8);
    smpte_tpf = song.division & 0xFF;
    if ((smpte_fps != 24 && smpte_fps != 25 && smpte_fps != 29 && smpte_fps != 30) || smpte_tpf == 0)
      return fail("bad SMPTE division");
  }

  // Tracks.
  std::vector<uint64_t> track_end;
  size_t pos = 8 + size_t(hlen);
  uint32_t track = 0;
  while (pos + 8 <= bytes.size() && track < ntrks) {
    const uint8_t* h = bytes.data() + pos;
    uint32_t size = be32(h + 4);
    size_t body = pos + 8;
    size_t avail = std::min<size_t>(size, bytes.size() - body);
    pos = body + avail;
    if (memcmp(h, "MTrk", 4) != 0) continue;  // unknown chunk: skipped
    if (avail < size) song.warnings.push_back("track " + std::to_string(track) + " truncated by the end of the file");
    Reader r{bytes.data() + body, bytes.data() + body + avail};
    uint64_t tick = 0;
    uint8_t running = 0;
    bool eot = false;
    while (r.p < r.end && !eot) {
      uint32_t delta = 0;
      if (!r.vlq(delta)) {
        song.warnings.push_back("track " + std::to_string(track) + ": bad delta time");
        break;
      }
      tick += delta;
      if (r.p >= r.end) break;
      uint8_t b = *r.p;
      SmfEvent e;
      e.tick = tick;
      e.track = track;
      if (b == 0xFF) {
        r.p++;
        uint32_t len = 0;
        if (r.p >= r.end) break;
        e.status = 0xFF;
        e.meta = *r.p++;
        if (!r.vlq(len) || size_t(r.end - r.p) < len) {
          song.warnings.push_back("track " + std::to_string(track) + ": truncated meta event");
          break;
        }
        e.off = uint32_t(song.blob.size());
        e.len = len;
        song.blob.insert(song.blob.end(), r.p, r.p + len);
        r.p += len;
        if (e.meta == 0x2F) eot = true;
        song.events.push_back(e);
      } else if (b == 0xF0 || b == 0xF7) {
        r.p++;
        uint32_t len = 0;
        if (!r.vlq(len) || size_t(r.end - r.p) < len) {
          song.warnings.push_back("track " + std::to_string(track) + ": truncated SysEx");
          break;
        }
        e.status = b;
        e.off = uint32_t(song.blob.size());
        e.len = len;
        song.blob.insert(song.blob.end(), r.p, r.p + len);
        r.p += len;
        song.events.push_back(e);
      } else {
        uint8_t status = running;
        if (b & 0x80) {
          if (b > 0xF0) {  // system common/real-time bytes have no place in a file
            song.warnings.push_back("track " + std::to_string(track) + ": unexpected status byte");
            break;
          }
          status = b;
          r.p++;
        }
        if (!(status & 0x80)) {
          song.warnings.push_back("track " + std::to_string(track) + ": data byte without running status");
          break;
        }
        running = status;
        e.status = status;
        int n = e.channel_len() - 1;
        if (r.end - r.p < n) {
          song.warnings.push_back("track " + std::to_string(track) + ": truncated channel message");
          break;
        }
        e.d1 = r.p[0] & 0x7F;
        if (n == 2) e.d2 = r.p[1] & 0x7F;
        r.p += n;
        song.events.push_back(e);
      }
    }
    track_end.push_back(tick);
    track++;
  }
  song.tracks = uint16_t(track);
  if (track == 0) return fail("no MTrk chunk");

  // Merge: (tick, track, order within the track) — the track order is the
  // push order, so a stable sort on (tick, track) keeps it.
  std::stable_sort(song.events.begin(), song.events.end(), [](const SmfEvent& a, const SmfEvent& b) {
    return a.tick != b.tick ? a.tick < b.tick : a.track < b.track;
  });
  song.length_ticks = track_end.empty() ? 0 : *std::max_element(track_end.begin(), track_end.end());

  // Tempo map -> absolute microseconds.
  if (song.division > 0) {
    const uint64_t ppqn = uint64_t(song.division);
    // acc = sum of delta_ticks * tempo (µs * ppqn), exact.
    unsigned __int128 acc = 0;
    uint64_t last = 0;
    uint32_t tempo = 500000;
    for (SmfEvent& e : song.events) {
      acc += (unsigned __int128)(e.tick - last) * tempo;
      last = e.tick;
      e.us = uint64_t(acc / ppqn);
      if (e.status == 0xFF && e.meta == 0x51 && e.len >= 3) {
        const uint8_t* t = song.blob.data() + e.off;
        uint32_t v = (uint32_t(t[0]) << 16) | (uint32_t(t[1]) << 8) | t[2];
        if (v > 0) tempo = v;
      }
    }
    acc += (unsigned __int128)(song.length_ticks >= last ? song.length_ticks - last : 0) * tempo;
    song.length_us = uint64_t(acc / ppqn);
  } else {
    // SMPTE: tempo events do not apply. 29 = 29.97 fps (drop frame).
    const uint64_t num = smpte_fps == 29 ? 100000000ull : 1000000ull;
    const uint64_t den = (smpte_fps == 29 ? 2997ull : uint64_t(smpte_fps)) * uint64_t(smpte_tpf);
    for (SmfEvent& e : song.events) e.us = uint64_t((unsigned __int128)e.tick * num / den);
    song.length_us = uint64_t((unsigned __int128)song.length_ticks * num / den);
  }
  out = std::move(song);
  return true;
}

bool apply_mpc_rule(SmfSong& song) {
  std::array<uint32_t, 16> n = song.note_ons();
  bool extended = false;
  for (int c = 0; c < 10; c++) extended |= n[c] > 0;
  if (!(n[12] > 0 && extended)) return false;
  song.events.erase(std::remove_if(song.events.begin(), song.events.end(),
                                   [](const SmfEvent& e) { return e.is_channel() && e.channel() >= 12; }),
                    song.events.end());
  return true;
}

}  // namespace adw::audio
