// core.audio — the host audio engine (docs/AUDIO.md §10.1): formats,
// ADPCM decoders byte for byte against the host's own msacm32 codecs, gains,
// mixer determinism (a golden hash), timing, streams, SMF, captures, config.
//
//   adw_core_audio_tests [<test-filter>]
//
// Nothing here opens a sound device, except the one opt-in check at the end
// (AD_AUDIO_LIVE_TEST=1: a person listening, never CI): 1 s of 440 Hz at
// -30 dBFS through WASAPI, then a soft MIDI note; 3 s in all.
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <msacm.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "adw/core/audio.h"
#include "adw/core/clock.h"
#include "adw/core/env.h"
#include "adw/core/fnv.h"
#include "adw/core/host.h"
#include "adw/core/lane.h"
#include "adw/core/log.h"
#include "adw/core/text.h"
#include "audio_internal.h"
#include "check.h"

using namespace adw;
using namespace adw::audio;

namespace {

// ---- helpers ----------------------------------------------------------------------------

std::string temp_path(const std::string& name) {
  wchar_t buf[MAX_PATH];
  GetTempPathW(MAX_PATH, buf);
  return narrow(buf) + "adw_audio_" + std::to_string(GetCurrentProcessId()) + "_" + name;
}

std::vector<uint8_t> read_file(const std::string& path) {
  std::vector<uint8_t> out;
  FILE* f = _wfopen(widen(path).c_str(), L"rb");
  if (!f) return out;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  fclose(f);
  return out;
}

void remove_file(const std::string& path) { DeleteFileW(widen(path).c_str()); }

Env env_of(std::map<std::string, std::string> vars) { return Env::parse(vars); }

void le16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(uint8_t(x));
  v.push_back(uint8_t(x >> 8));
}
void le32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; i++) v.push_back(uint8_t(x >> (8 * i)));
}
void be32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 3; i >= 0; i--) v.push_back(uint8_t(x >> (8 * i)));
}
void be16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(uint8_t(x >> 8));
  v.push_back(uint8_t(x));
}
void chunk(std::vector<uint8_t>& v, const char* id, const std::vector<uint8_t>& body, bool pad = true) {
  v.insert(v.end(), id, id + 4);
  le32(v, uint32_t(body.size()));
  v.insert(v.end(), body.begin(), body.end());
  if (pad && (body.size() & 1)) v.push_back(0);
}
std::vector<uint8_t> riff_wave(const std::vector<uint8_t>& chunks, int32_t size_adjust = 0) {
  std::vector<uint8_t> v = {'R', 'I', 'F', 'F'};
  le32(v, uint32_t(int32_t(chunks.size() + 4) + size_adjust));
  v.insert(v.end(), {'W', 'A', 'V', 'E'});
  v.insert(v.end(), chunks.begin(), chunks.end());
  return v;
}

// Deterministic noise.
struct Lcg {
  uint32_t s;
  uint32_t next() { return s = s * 1664525u + 1013904223u; }
};

// PCM16 test material: a sweep, noise, silence and a clipping square wave.
std::vector<int16_t> test_signal(size_t frames, int ch, uint32_t seed) {
  std::vector<int16_t> v(frames * size_t(ch));
  Lcg r{seed};
  for (size_t i = 0; i < frames; i++) {
    for (int c = 0; c < ch; c++) {
      double t = double(i) / double(frames);
      int32_t s;
      switch ((i * 4 / frames + size_t(c)) % 4) {
        case 0: s = int32_t(20000 * sin(6.283185 * (50 + 4000 * t) * double(i) / 22050.0)); break;
        case 1: s = int32_t(int16_t(r.next() >> 16)); break;
        case 2: s = 0; break;
        default: s = (i / 37) % 2 ? 32767 : -32768; break;
      }
      v[i * size_t(ch) + size_t(c)] = int16_t(s);
    }
  }
  return v;
}

// ---- ACM ----
struct AcmFormat {
  std::vector<uint8_t> b;
  WAVEFORMATEX* w() { return reinterpret_cast<WAVEFORMATEX*>(b.data()); }
};

AcmFormat acm_pcm(uint32_t rate, int ch) {
  AcmFormat f;
  f.b = waveformat_bytes(pcm_format(rate, uint16_t(ch), 16));
  return f;
}

// Converts with the host codec. ok = false when the stream cannot open.
std::vector<uint8_t> acm_convert(WAVEFORMATEX* src, WAVEFORMATEX* dst, const std::vector<uint8_t>& in, bool* ok) {
  *ok = false;
  HACMSTREAM h = nullptr;
  if (acmStreamOpen(&h, nullptr, src, dst, nullptr, 0, 0, ACM_STREAMOPENF_NONREALTIME)) return {};
  DWORD out = 0;
  if (acmStreamSize(h, DWORD(std::max<size_t>(in.size(), 1)), &out, ACM_STREAMSIZEF_SOURCE)) out = 0;
  std::vector<uint8_t> o(size_t(out) + 65536), i2 = in;
  i2.resize(std::max<size_t>(in.size(), 1));
  ACMSTREAMHEADER hd{};
  hd.cbStruct = sizeof(hd);
  hd.pbSrc = i2.data();
  hd.cbSrcLength = DWORD(in.size());
  hd.pbDst = o.data();
  hd.cbDstLength = DWORD(o.size());
  if (!acmStreamPrepareHeader(h, &hd, 0)) {
    if (!acmStreamConvert(h, &hd, ACM_STREAMCONVERTF_START | ACM_STREAMCONVERTF_END)) *ok = true;
    o.resize(hd.cbDstLengthUsed);
    acmStreamUnprepareHeader(h, &hd, 0);
  }
  acmStreamClose(h, 0);
  return o;
}

// The host codec's format for (tag, rate, ch), by enumeration.
struct EnumCtx {
  uint16_t tag;
  std::vector<std::vector<uint8_t>> formats;
};
BOOL CALLBACK enum_cb(HACMDRIVERID, LPACMFORMATDETAILSW d, DWORD_PTR inst, DWORD) {
  auto* c = reinterpret_cast<EnumCtx*>(inst);
  if (d->pwfx->wFormatTag == c->tag) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(d->pwfx);
    c->formats.emplace_back(p, p + 18 + d->pwfx->cbSize);
  }
  return TRUE;
}
std::vector<std::vector<uint8_t>> acm_enum(uint16_t tag) {
  DWORD max = 0;
  acmMetrics(nullptr, ACM_METRIC_MAX_SIZE_FORMAT, &max);
  std::vector<uint8_t> buf(max, 0);
  auto* w = reinterpret_cast<WAVEFORMATEX*>(buf.data());
  w->wFormatTag = tag;
  w->cbSize = uint16_t(max - 18);
  ACMFORMATDETAILSW d{};
  d.cbStruct = sizeof(d);
  d.dwFormatTag = tag;
  d.pwfx = w;
  d.cbwfx = max;
  EnumCtx c{tag, {}};
  acmFormatEnumW(nullptr, &d, enum_cb, DWORD_PTR(&c), ACM_FORMATENUMF_WFORMATTAG);
  return c.formats;
}

bool have_codec(uint16_t tag) { return !acm_enum(tag).empty(); }

// ---- SMF building ----
struct Trk {
  std::vector<uint8_t> b;
  Trk& ev(uint32_t delta, std::initializer_list<uint8_t> bytes) {
    put_vlq(b, delta);
    b.insert(b.end(), bytes);
    return *this;
  }
  Trk& eot(uint32_t delta = 0) { return ev(delta, {0xFF, 0x2F, 0x00}); }
  Trk& tempo(uint32_t delta, uint32_t us) {
    return ev(delta, {0xFF, 0x51, 0x03, uint8_t(us >> 16), uint8_t(us >> 8), uint8_t(us)});
  }
};
std::vector<uint8_t> smf(uint16_t format, int16_t division, const std::vector<Trk>& tracks) {
  std::vector<uint8_t> v = {'M', 'T', 'h', 'd'};
  be32(v, 6);
  be16(v, format);
  be16(v, uint16_t(tracks.size()));
  be16(v, uint16_t(division));
  for (const Trk& t : tracks) {
    v.insert(v.end(), {'M', 'T', 'r', 'k'});
    be32(v, uint32_t(t.b.size()));
    v.insert(v.end(), t.b.begin(), t.b.end());
  }
  return v;
}

// A parsed .mid log: (ms, message bytes).
struct LogEv {
  uint64_t ms;
  std::vector<uint8_t> m;
};
std::vector<LogEv> read_midi_log(const std::string& path, SmfSong* song_out = nullptr) {
  std::vector<LogEv> out;
  SmfSong s;
  std::vector<uint8_t> bytes = read_file(path);
  if (!parse_smf(bytes, s)) return out;
  for (const SmfEvent& e : s.events) {
    if (e.status == 0xFF) continue;
    LogEv l;
    l.ms = e.us / 1000;
    if (e.is_channel()) {
      l.m = {e.status, e.d1};
      if (e.channel_len() == 3) l.m.push_back(e.d2);
    } else {
      l.m.push_back(e.status);
      l.m.insert(l.m.end(), s.blob.begin() + e.off, s.blob.begin() + e.off + e.len);
    }
    out.push_back(l);
  }
  if (song_out) *song_out = std::move(s);
  return out;
}

// Capture samples (interleaved int16) of a WAV written by the engine.
std::vector<int16_t> read_capture(const std::string& path, uint32_t* rate = nullptr, uint32_t* hdr_frames = nullptr) {
  std::vector<uint8_t> b = read_file(path);
  Wave w;
  if (!parse_wave(b, w)) return {};
  if (rate) *rate = w.format.rate;
  if (hdr_frames) *hdr_frames = uint32_t(w.data.size() / 4);
  std::vector<int16_t> s(w.data.size() / 2);
  memcpy(s.data(), w.data.data(), s.size() * 2);
  return s;
}

Config capture_config(const std::string& wav, uint32_t rate = 44100) {
  Config c;
  c.guest_sound = true;
  c.capture_wav = wav;
  c.capture_mid = wav.substr(0, wav.size() - 4) + ".mid";
  c.rate = rate;
  return c;
}

Config quiet_config(uint32_t rate = 44100) {
  Config c;
  c.guest_sound = true;
  c.rate = rate;
  return c;
}

std::vector<uint8_t> pcm16(const std::vector<int16_t>& s) {
  std::vector<uint8_t> b(s.size() * 2);
  memcpy(b.data(), s.data(), b.size());
  return b;
}

}  // namespace

// ============================================================================================
// formats

TEST(format_constructors_match_the_host_codecs) {
  // §2.2: the 8 standard formats, in the codecs' enumeration order.
  const uint32_t rates[4] = {8000, 11025, 22050, 44100};
  for (uint16_t tag : {kTagImaAdpcm, kTagMsAdpcm}) {
    auto host = acm_enum(tag);
    if (host.empty()) {
      fprintf(stderr, "  (no host codec for tag 0x%x: skipped)\n", tag);
      continue;
    }
    CHECK_EQ(host.size(), size_t(8));
    for (size_t i = 0; i < 8 && i < host.size(); i++) {
      WaveFormat f = tag == kTagImaAdpcm ? ima_adpcm_format(rates[i / 2], uint16_t(1 + i % 2))
                                         : ms_adpcm_format(rates[i / 2], uint16_t(1 + i % 2));
      std::vector<uint8_t> ours = waveformat_bytes(f);
      CHECK(ours == host[i]);
      if (ours != host[i]) fprintf(stderr, "  tag 0x%x index %zu differs\n", tag, i);
      WaveFormat back;
      CHECK(parse_waveformat(host[i], back));
      CHECK(back == f);
      CHECK(decodable(f));
      CHECK(!playable(f));
    }
  }
  DWORD max = 0;
  acmMetrics(nullptr, ACM_METRIC_MAX_SIZE_FORMAT, &max);
  CHECK_EQ(uint32_t(max), 50u);  // §2.2; MS-ADPCM's 18 + 32
  CHECK_EQ(waveformat_bytes(ms_adpcm_format(22050, 1)).size(), size_t(50));
  // The corpus's IMA formats (§2.2): 22050 mono align 512, 11025 256, 44100 1024.
  CHECK_EQ(ima_adpcm_format(22050, 1).block_align, 512);
  CHECK_EQ(ima_adpcm_format(22050, 1).samples_per_block, 1017);
  CHECK_EQ(ima_adpcm_format(11025, 1).samples_per_block, 505);
  CHECK_EQ(ima_adpcm_format(44100, 1).block_align, 1024);
  CHECK_EQ(ms_adpcm_format(11025, 1).samples_per_block, 500);
  CHECK_EQ(ms_adpcm_format(44100, 2).samples_per_block, 2036);
}

TEST(parse_waveformat_shapes) {
  WaveFormat f;
  // PCMWAVEFORMAT: 16 bytes; what follows is not a cbSize.
  std::vector<uint8_t> pcm;
  le16(pcm, 1);
  le16(pcm, 2);
  le32(pcm, 22050);
  le32(pcm, 88200);
  le16(pcm, 4);
  le16(pcm, 16);
  CHECK(parse_waveformat(pcm, f));
  CHECK(f == pcm_format(22050, 2, 16));
  CHECK(playable(f));
  std::vector<uint8_t> garbage = pcm;
  le16(garbage, 0xFFFF);  // a bogus "cbSize" after a PCMWAVEFORMAT
  CHECK(parse_waveformat(garbage, f));
  CHECK(f == pcm_format(22050, 2, 16));
  CHECK(!parse_waveformat(std::span<const uint8_t>(pcm.data(), 15), f));  // truncated
  CHECK(!parse_waveformat({}, f));
  // 8-bit mono, as AD_SND probes it.
  CHECK(playable(pcm_format(11025, 1, 8)));
  CHECK_EQ(waveformat_bytes(pcm_format(11025, 1, 8)).size(), size_t(18));
  // ADPCM needs its extra bytes.
  std::vector<uint8_t> ima = waveformat_bytes(ima_adpcm_format(22050, 1));
  CHECK_EQ(ima.size(), size_t(20));
  CHECK(!parse_waveformat(std::span<const uint8_t>(ima.data(), 18), f));
  CHECK(!parse_waveformat(std::span<const uint8_t>(ima.data(), 16), f));
  CHECK(parse_waveformat(ima, f) && f == ima_adpcm_format(22050, 1));
  std::vector<uint8_t> ms = waveformat_bytes(ms_adpcm_format(11025, 1));
  CHECK(!parse_waveformat(std::span<const uint8_t>(ms.data(), 49), f));
  CHECK(parse_waveformat(ms, f) && f.coefs.size() == 7 && f.coefs[1][0] == 512 && f.coefs[1][1] == -256);
  // playable / decodable edges.
  WaveFormat bad = pcm_format(22050, 3, 16);
  CHECK(!playable(bad));
  bad = pcm_format(22050, 1, 12);
  CHECK(!playable(bad));
  bad = pcm_format(500, 1, 8);
  CHECK(!playable(bad));
  bad = pcm_format(22050, 2, 16);
  bad.block_align = 2;
  CHECK(!playable(bad));
  WaveFormat tiny = ima_adpcm_format(22050, 1);
  tiny.block_align = 3;  // cannot hold its header
  CHECK(!decodable(tiny));
  WaveFormat many = ima_adpcm_format(22050, 1);
  many.samples_per_block = 1018;  // more than the block holds
  CHECK(!decodable(many));
  WaveFormat nocoef = ms_adpcm_format(22050, 1);
  nocoef.coefs.clear();
  CHECK(!decodable(nocoef));
  CHECK(decodable(pcm_format(11025, 1, 8)));
  CHECK(decoded_format(ima_adpcm_format(22050, 2)) == pcm_format(22050, 2, 16));
  CHECK(decoded_format(pcm_format(11025, 1, 8)) == pcm_format(11025, 1, 8));
}

TEST(parse_wave_and_riff_extent) {
  std::vector<uint8_t> fmt = waveformat_bytes(pcm_format(11025, 1, 8));
  fmt.resize(16);  // a PCMWAVEFORMAT fmt chunk
  std::vector<uint8_t> data = {0x80, 0x90, 0xA0};  // odd size
  std::vector<uint8_t> list = {'I', 'N', 'F', 'O', 'x'};  // odd, padded
  std::vector<uint8_t> c;
  chunk(c, "fmt ", fmt);
  chunk(c, "LIST", list);
  std::vector<uint8_t> fact;
  le32(fact, 3);
  chunk(c, "fact", fact);
  chunk(c, "data", data);
  std::vector<uint8_t> img = riff_wave(c);
  Wave w;
  CHECK(parse_wave(img, w));
  CHECK(w.format == pcm_format(11025, 1, 8));
  CHECK_EQ(w.data.size(), size_t(3));
  CHECK(w.data.size() == 3 && w.data[2] == 0xA0);
  CHECK_EQ(w.fact_samples, 3u);
  CHECK_EQ(riff_extent(img), img.size() - 1 + 1);  // 8 + RIFF size (the pad byte included)
  CHECK_EQ(riff_extent(std::span<const uint8_t>(img.data(), 12)), img.size());
  CHECK_EQ(riff_extent(std::span<const uint8_t>(img.data(), 11)), size_t(0));
  std::vector<uint8_t> notwave = img;
  notwave[8] = 'X';
  CHECK_EQ(riff_extent(notwave), size_t(0));
  CHECK(!parse_wave(notwave, w));
  // A RIFF size too large: the image clips it. Too small: not trusted.
  CHECK(parse_wave(riff_wave(c, 1000), w) && w.data.size() == 3);
  CHECK(parse_wave(riff_wave(c, -20), w) && w.data.size() == 3);
  // A data chunk that claims more than the image holds is clipped.
  std::vector<uint8_t> c2;
  chunk(c2, "fmt ", fmt);
  c2.insert(c2.end(), {'d', 'a', 't', 'a'});
  le32(c2, 100000);
  c2.insert(c2.end(), {1, 2, 3, 4, 5});
  CHECK(parse_wave(riff_wave(c2), w) && w.data.size() == 5);
  // Missing pieces.
  std::vector<uint8_t> nodata;
  chunk(nodata, "fmt ", fmt);
  CHECK(!parse_wave(riff_wave(nodata), w));
  std::vector<uint8_t> nofmt;
  chunk(nofmt, "data", data);
  CHECK(!parse_wave(riff_wave(nofmt), w));
  CHECK(!parse_wave(std::span<const uint8_t>(img.data(), 11), w));
  // A truncated fmt chunk.
  std::vector<uint8_t> c3;
  c3.insert(c3.end(), {'f', 'm', 't', ' '});
  le32(c3, 16);
  c3.insert(c3.end(), fmt.begin(), fmt.begin() + 10);
  CHECK(!parse_wave(riff_wave(c3), w));
  // Stereo 16-bit, bogus chunk sizes after data are harmless.
  std::vector<uint8_t> c4;
  chunk(c4, "fmt ", waveformat_bytes(pcm_format(44100, 2, 16)));
  chunk(c4, "data", {1, 0, 2, 0, 3, 0, 4, 0});
  c4.insert(c4.end(), {'j', 'u', 'n', 'k', 0xFF, 0xFF, 0xFF, 0xFF});
  CHECK(parse_wave(riff_wave(c4), w) && w.format == pcm_format(44100, 2, 16) && w.data.size() == 8);
}

// ============================================================================================
// decoders

TEST(adpcm_decoders_match_the_host_acm) {
  for (uint16_t tag : {kTagImaAdpcm, kTagMsAdpcm}) {
    if (!have_codec(tag)) {
      fprintf(stderr, "  (no host codec for tag 0x%x: skipped)\n", tag);
      continue;
    }
    for (uint32_t rate : {11025u, 22050u, 44100u}) {
      for (int ch : {1, 2}) {
        WaveFormat af = tag == kTagImaAdpcm ? ima_adpcm_format(rate, uint16_t(ch)) : ms_adpcm_format(rate, uint16_t(ch));
        AcmFormat adpcm{waveformat_bytes(af)}, pcm = acm_pcm(rate, ch);
        std::vector<int16_t> sig = test_signal(size_t(af.samples_per_block) * 6 + 333, ch, rate + uint32_t(ch));
        bool ok = false;
        std::vector<uint8_t> enc = acm_convert(pcm.w(), adpcm.w(), pcm16(sig), &ok);
        CHECK(ok);
        CHECK(enc.size() >= af.block_align * 6u);
        // Whole blocks plus every interesting partial tail.
        std::vector<size_t> cuts = {enc.size()};
        size_t whole = (enc.size() / af.block_align) * af.block_align;
        for (size_t extra : {size_t(0), size_t(1), size_t(3), size_t(4 * ch), size_t(7 * ch - 1), size_t(7 * ch),
                             size_t(7 * ch + 1), size_t(100), size_t(af.block_align - 1)})
          if (whole - af.block_align + extra <= enc.size()) cuts.push_back(whole - af.block_align + extra);
        for (size_t cut : cuts) {
          std::vector<uint8_t> in(enc.begin(), enc.begin() + ptrdiff_t(cut));
          std::vector<uint8_t> want = acm_convert(adpcm.w(), pcm.w(), in, &ok);
          std::vector<uint8_t> got = decode(af, in);
          CHECK(want == got);
          if (want != got) {
            size_t d = 0;
            while (d < want.size() && d < got.size() && want[d] == got[d]) d++;
            fprintf(stderr, "  tag 0x%x %u Hz ch %d cut %zu: acm %zu bytes, ours %zu, first difference at %zu\n", tag,
                    rate, ch, cut, want.size(), got.size(), d);
          }
          // The span form writes the same, and never past its buffer.
          std::vector<uint8_t> out(got.size() + 7, 0xEE);
          CHECK_EQ(decode(af, in, out), got.size());
          CHECK(std::equal(got.begin(), got.end(), out.begin()));
          CHECK_EQ(out.back(), 0xEE);
          std::vector<uint8_t> small(af.samples_per_block * size_t(ch) * 2 + 1);
          size_t n = decode(af, in, small);
          CHECK(n <= small.size());
        }
      }
    }
  }
}

// Blocks no encoder makes, which a guest WAV can still hold: a header delta
// of 0x7FFF (or any int16) and nibbles 0x8/0x9/0x7 (adapt 768: delta triples
// each step) overflow 32-bit products within ~7 steps, and an int16 predictor
// sum overflows int32 with extreme coefficients. The decoder's integer rule
// for them is msadp32's, not undefined behaviour: compared with the host ACM
// where it exists.
TEST(ms_adpcm_extreme_blocks_match_the_host_acm) {
  if (!have_codec(kTagMsAdpcm)) {
    fprintf(stderr, "  (no host MS-ADPCM codec: skipped)\n");
    return;
  }
  for (int ch : {1, 2}) {
    WaveFormat af = ms_adpcm_format(22050, uint16_t(ch));
    AcmFormat adpcm{waveformat_bytes(af)}, pcm = acm_pcm(22050, ch);
    for (uint8_t fill : {0x88, 0x99, 0x77, 0x8F, 0xF8, 0x07, 0x70}) {
      for (int16_t delta : {int16_t(0x7FFF), int16_t(-32768), int16_t(-1), int16_t(0), int16_t(4000)}) {
        for (uint8_t pred : {uint8_t(0), uint8_t(1), uint8_t(4), uint8_t(6)}) {
          std::vector<uint8_t> block(af.block_align, fill);
          for (int c = 0; c < ch; c++) {
            block[size_t(c)] = pred;
            block[size_t(ch + 2 * c)] = uint8_t(delta & 0xFF);
            block[size_t(ch + 2 * c + 1)] = uint8_t(uint16_t(delta) >> 8);
            block[size_t(3 * ch + 2 * c)] = 0xFF;  // s1 = 32767
            block[size_t(3 * ch + 2 * c + 1)] = 0x7F;
            block[size_t(5 * ch + 2 * c)] = 0x00;  // s2 = -32768
            block[size_t(5 * ch + 2 * c + 1)] = 0x80;
          }
          std::vector<uint8_t> in;
          for (int k = 0; k < 3; k++) in.insert(in.end(), block.begin(), block.end());
          bool ok = false;
          std::vector<uint8_t> want = acm_convert(adpcm.w(), pcm.w(), in, &ok);
          std::vector<uint8_t> got = decode(af, in);
          CHECK(ok);
          CHECK(want == got);
          if (want != got) {
            size_t d = 0;
            while (d < want.size() && d < got.size() && want[d] == got[d]) d++;
            fprintf(stderr, "  ch %d fill %02X delta %d pred %u: acm %zu bytes, ours %zu, first difference at %zu", ch,
                    fill, delta, pred, want.size(), got.size(), d);
            if (d + 1 < want.size() && d + 1 < got.size())
              fprintf(stderr, " (acm %d, ours %d)", int16_t(want[d & ~size_t(1)] | (want[(d & ~size_t(1)) + 1] << 8)),
                      int16_t(got[d & ~size_t(1)] | (got[(d & ~size_t(1)) + 1] << 8)));
            fprintf(stderr, "\n");
          }
        }
      }
    }
  }
}

TEST(stream_sizes_match_acmStreamSize) {
  for (uint16_t tag : {kTagImaAdpcm, kTagMsAdpcm}) {
    if (!have_codec(tag)) continue;
    for (uint32_t rate : {11025u, 22050u, 44100u}) {
      for (int ch : {1, 2}) {
        WaveFormat af = tag == kTagImaAdpcm ? ima_adpcm_format(rate, uint16_t(ch)) : ms_adpcm_format(rate, uint16_t(ch));
        AcmFormat adpcm{waveformat_bytes(af)}, pcm = acm_pcm(rate, ch);
        HACMSTREAM h = nullptr;
        CHECK(!acmStreamOpen(&h, nullptr, adpcm.w(), pcm.w(), nullptr, 0, 0, ACM_STREAMOPENF_NONREALTIME));
        if (!h) continue;
        const size_t ba = af.block_align, pb = size_t(af.samples_per_block) * ch * 2;
        for (size_t s : {size_t(1), size_t(3), ba - 1, ba, ba + 1, 3 * ba + 100, 10 * ba}) {
          DWORD out = 0;
          MMRESULT r = acmStreamSize(h, DWORD(s), &out, ACM_STREAMSIZEF_SOURCE);
          CHECK_EQ(decoded_size(af, s), size_t(r ? 0 : out));
        }
        for (size_t s : {size_t(1), pb - 1, pb, pb + 1, 3 * pb + 17, 10 * pb}) {
          DWORD out = 0;
          MMRESULT r = acmStreamSize(h, DWORD(s), &out, ACM_STREAMSIZEF_DESTINATION);
          CHECK_EQ(encoded_size_for(af, s), size_t(r ? 0 : out));
        }
        acmStreamClose(h, 0);
      }
    }
  }
  // PCM is returned unchanged.
  CHECK_EQ(decoded_size(pcm_format(11025, 1, 8), 1001), size_t(1001));
  CHECK_EQ(encoded_size_for(pcm_format(11025, 1, 8), 1001), size_t(1001));
  std::vector<uint8_t> raw = {1, 2, 3, 4, 5};
  CHECK(decode(pcm_format(11025, 1, 16), raw) == std::vector<uint8_t>({1, 2, 3, 4}));
  CHECK(decode(pcm_format(11025, 1, 8), raw) == raw);
  CHECK(decode(pcm_format(11025, 3, 8), raw).empty());
}

// ============================================================================================
// gains

TEST(gain_tables) {
  CHECK_EQ(db_table(0), 0x8000);
  CHECK_EQ(db_table(10000), 0);
  for (int i = 0; i <= 10000; i++) {
    uint16_t want = uint16_t(floor(32768.0 * pow(10.0, -double(i) / 2000.0) + 0.5));
    if (db_table(i) != want) {
      CHECK_EQ(db_table(i), want);
      break;
    }
    if (i) CHECK(db_table(i) <= db_table(i - 1));
  }
  CHECK(gain_from_ds(0, 0) == Gain{});
  CHECK(gain_from_ds(-10000, 0) == (Gain{0, 0}));
  CHECK(gain_from_ds(-20000, 0) == (Gain{0, 0}));
  CHECK(gain_from_ds(100, 0) == Gain{});
  CHECK_EQ(gain_from_ds(-2000, 0).left, 3277);  // -20 dB
  CHECK_EQ(gain_from_ds(-1000, 0).left, 10362);  // ADVOLUME 50 on AD4: -10 dB
  CHECK(gain_from_ds(0, 10000) == (Gain{0, 0x8000}));
  CHECK(gain_from_ds(0, -10000) == (Gain{0x8000, 0}));
  CHECK(gain_from_ds(-1000, 500) == (Gain{db_table(1500), db_table(1000)}));
  CHECK(gain_from_ds(-1000, -500) == (Gain{db_table(1000), db_table(1500)}));
  CHECK(gain_from_ds(-9000, 5000) == (Gain{0, db_table(9000)}));
  // WINMM volumes.
  CHECK(gain_from_mm(0xFFFFFFFF) == Gain{});
  CHECK(gain_from_mm(0) == (Gain{0, 0}));
  CHECK(gain_from_mm(0x7FFF7FFF) == (Gain{0x4000, 0x4000}));  // AD_SND at volume 50
  CHECK(gain_from_mm(0x0000FFFF) == (Gain{0x8000, 0}));
  CHECK_EQ(mm_from_gain(Gain{}), 0xFFFFFFFFu);
  CHECK_EQ(mm_from_gain(Gain{0, 0}), 0u);
  CHECK_EQ(mm_from_gain(Gain{0xFFFF, 0}), 0x0000FFFFu);  // above unity saturates
  bool round_trip = true;
  for (uint32_t g = 0; g <= 0x8000; g++)
    if (!(gain_from_mm(mm_from_gain(Gain{uint16_t(g), uint16_t(0x8000 - g)})) == Gain{uint16_t(g), uint16_t(0x8000 - g)}))
      round_trip = false;
  CHECK(round_trip);
  bool monotonic = true;
  for (uint32_t w = 1; w <= 0xFFFF; w++)
    if (gain_from_mm(w).left < gain_from_mm(w - 1).left) monotonic = false;
  CHECK(monotonic);
}

// ============================================================================================
// the engine: basics

TEST(null_and_disabled_engines) {
  Engine& n = null_engine();
  CHECK(!n.enabled());
  CHECK(!n.config().guest_sound);
  CHECK_EQ(n.create_buffer(pcm_format(22050, 1, 16), 100), 0u);
  CHECK_EQ(n.create_voice(1, Bus::wave), 0u);
  CHECK_EQ(n.open_stream(pcm_format(22050, 1, 16), Bus::wave, 0), 0u);
  std::string err;
  CHECK_EQ(n.load_song(smf(0, 96, {Trk().eot()}), &err), 0u);
  CHECK(n.bus_gain(Bus::midi) == Gain{});
  std::vector<Event> ev;
  n.poll(1000000, ev);
  CHECK(ev.empty());
  CHECK_EQ(n.next_event_time(), Time(0));
  Config off;
  off.capture_wav = temp_path("never.wav");  // guest_sound false: no capture
  auto e = make_engine(off);
  CHECK(!e->enabled());
  e->advance(1000000);
  e->shutdown(1000000);
  CHECK(read_file(off.capture_wav).empty());
  auto on = make_engine(quiet_config());
  CHECK(on->enabled());
}

TEST(buffers_and_voice_limits) {
  auto e = make_engine(quiet_config());
  CHECK_EQ(e->create_buffer(pcm_format(22050, 1, 16), 0), 0u);
  CHECK_EQ(e->create_buffer(pcm_format(22050, 1, 16), (256u << 20) + 1), 0u);
  CHECK_EQ(e->create_buffer(ima_adpcm_format(22050, 1), 1024), 0u);  // PCM only
  BufferId b = e->create_buffer(pcm_format(22050, 1, 16), 1000);
  CHECK(b != 0);
  CHECK_EQ(e->buffer_size(b), 1000u);
  CHECK(e->buffer_format(b) && *e->buffer_format(b) == pcm_format(22050, 1, 16));
  std::vector<VoiceId> v;
  for (int i = 0; i < 300; i++) {
    VoiceId id = e->create_voice(b, Bus::wave);
    if (id) v.push_back(id);
  }
  CHECK_EQ(v.size(), size_t(256));
  // The buffer lives on, still usable, until its last voice is destroyed.
  e->release_buffer(b);
  e->release_buffer(b);  // twice is harmless
  CHECK(e->buffer_format(b) != nullptr);
  CHECK_EQ(e->buffer_size(b), 1000u);
  e->play(v[0], true, 0);
  CHECK(e->playing(v[0], 1000));
  for (size_t i = 1; i < v.size(); i++) e->destroy_voice(v[i], 2000);
  VoiceId dup = e->create_voice(b, Bus::wave);  // a duplicate after the release
  CHECK(dup != 0);
  e->write_buffer(b, 0, std::vector<uint8_t>{1, 2}, 2000);
  e->destroy_voice(v[0], 3000);
  CHECK(e->buffer_format(b) != nullptr);
  e->destroy_voice(dup, 3000);
  CHECK(e->buffer_format(b) == nullptr);  // gone with its last voice
  CHECK_EQ(e->buffer_size(b), 0u);
  CHECK_EQ(e->create_voice(b, Bus::wave), 0u);
  BufferId unused = e->create_buffer(pcm_format(22050, 1, 16), 10);
  e->release_buffer(unused);  // no voice: gone at once
  CHECK(e->buffer_format(unused) == nullptr);
  BufferId b2 = e->create_buffer(pcm_format(8000, 2, 8), 64);
  CHECK(e->create_voice(b2, Bus::wave) != 0);
}

TEST(voice_timing_by_hand) {
  // AUDIO.md §10.1: a 11025-frame 22050 Hz voice started at 1000 µs ends at 501000.
  for (uint32_t R : {22050u, 44100u, 48000u}) {
    auto e = make_engine(quiet_config(R));
    BufferId b = e->create_buffer(pcm_format(22050, 1, 16), 11025 * 2);
    VoiceId v = e->create_voice(b, Bus::wave);
    CHECK_EQ(e->rate(v), 22050u);
    CHECK(!e->playing(v, 0));
    e->play(v, false, 1000);
    CHECK_EQ(e->end_time(v, 1000), Time(501000));
    CHECK_EQ(e->next_event_time(), Time(501000));
    CHECK_EQ(e->cursor(v, 251000), 5512u * 2);  // floor(250000 * 22050 / 10^6) frames
    CHECK(e->playing(v, 500999));
    CHECK_EQ(e->cursor(v, 500999), 11024u * 2);
    std::vector<Event> ev;
    e->poll(500999, ev);
    CHECK(ev.empty());
    CHECK(!e->playing(v, 501000));
    CHECK_EQ(e->cursor(v, 501000), 0u);  // back at 0 at the end
    CHECK_EQ(e->end_time(v, 501000), Time(0));
    e->poll(501000, ev);
    CHECK(ev.size() == 1 && ev[0].kind == Event::Kind::voice_end && ev[0].id == v && ev[0].at == 501000);
    // Looping wraps; stop keeps the cursor where playback got to.
    e->play(v, true, 600000);
    CHECK(e->looping(v, 600000));
    CHECK_EQ(e->end_time(v, 600000), Time(0));
    CHECK_EQ(e->cursor(v, 600000 + 500000 + 100000), (2205u) * 2);  // 600000 µs in: 13230 - 11025
    e->stop(v, 1200000);
    CHECK(!e->playing(v, 1300000));
    CHECK_EQ(e->cursor(v, 1300000), 2205u * 2);
    // Played again from there: its end is the rest of the buffer.
    e->play(v, false, 2000000);
    CHECK_EQ(e->end_time(v, 2000000), Time(2000000 + 400000));
    // A rate change re-anchors: the rest plays twice as fast.
    e->set_rate(v, 44100, 2100000);  // at 2205 + 2205 = 4410 frames
    CHECK_EQ(e->cursor(v, 2100000), 4410u * 2);
    CHECK_EQ(e->end_time(v, 2100000), Time(2100000 + 150000));
    e->set_rate(v, 0, 2100000);  // 0 = the buffer's own
    CHECK_EQ(e->rate(v), 22050u);
    e->set_rate(v, 5, 2100000);
    CHECK_EQ(e->rate(v), 100u);
    e->set_rate(v, 1000000, 2100000);
    CHECK_EQ(e->rate(v), 200000u);
    // set_cursor rounds down to a frame and clamps.
    e->stop(v, 2200000);
    e->set_cursor(v, 1001, 2200000);
    CHECK_EQ(e->cursor(v, 2200000), 1000u);
    e->set_cursor(v, 999999, 2200000);
    CHECK_EQ(e->cursor(v, 2200000), 11024u * 2);
    // A time earlier than the latest seen is taken as the latest.
    CHECK_EQ(e->cursor(v, 5), 11024u * 2);
    e->play(v, false, 10);  // = 2200000
    CHECK_EQ(e->end_time(v, 0), Time(2200000 + 5));  // one frame at 200000 Hz… rounded up
  }
}

TEST(events_in_time_then_issue_order) {
  auto e = make_engine(quiet_config());
  BufferId b = e->create_buffer(pcm_format(10000, 1, 8), 1000);  // 100 ms
  VoiceId a = e->create_voice(b, Bus::wave), c = e->create_voice(b, Bus::wave), d = e->create_voice(b, Bus::wave);
  e->play(c, false, 0);
  e->play(a, false, 0);
  e->play(d, false, 50000);
  e->stop(d, 60000);  // cancelled before its end: no event
  std::vector<Event> ev;
  e->poll(1000000, ev);
  CHECK_EQ(ev.size(), size_t(2));
  if (ev.size() == 2) {
    CHECK(ev[0].id == c && ev[1].id == a);  // same time: in the order issued
    CHECK(ev[0].at == 100000 && ev[1].at == 100000);
  }
  ev.clear();
  e->poll(2000000, ev);
  CHECK(ev.empty());  // each returned once
  // Stopping after the end keeps the (past) event.
  e->play(a, false, 2000000);
  e->stop(a, 2200000);
  e->poll(3000000, ev);
  CHECK(ev.size() == 1 && ev[0].at == 2100000);
  // destroy_voice before its end drops it.
  e->play(a, false, 3000000);
  e->destroy_voice(a, 3050000);
  ev.clear();
  e->poll(4000000, ev);
  CHECK(ev.empty());
}

// ============================================================================================
// rendering

TEST(rendered_position_matches_cursor) {
  // A ramp buffer at the mixer rate: output frame k holds the frame number the
  // voice was at, which must be what cursor() says for that time.
  std::string wav = temp_path("ramp.wav");
  {
    auto e = make_engine(capture_config(wav));
    std::vector<int16_t> ramp(30000);
    for (size_t i = 0; i < ramp.size(); i++) ramp[i] = int16_t(i + 1);
    BufferId b = e->create_buffer(pcm_format(44100, 1, 16), uint32_t(ramp.size() * 2));
    e->write_buffer(b, 0, pcm16(ramp), 0);
    VoiceId v = e->create_voice(b, Bus::wave);
    e->play(v, true, 1000000);
    std::vector<std::pair<uint64_t, uint32_t>> expect;  // (output frame, cursor frame)
    for (uint64_t n = 100; n < 150; n++) {  // every 10 ms from 1 s on
      Time t = n * 10000;
      uint32_t cur = e->cursor(v, t) / 2;
      expect.push_back({n * 441, cur});
      e->advance(t);
    }
    e->shutdown(1600000);
    std::vector<int16_t> s = read_capture(wav);
    CHECK_EQ(s.size(), size_t(1600000ull * 44100 / 1000000) * 2);
    for (auto& [k, cur] : expect) {
      if (k * 2 + 1 >= s.size()) break;
      CHECK_EQ(int(s[k * 2]), int(cur + 1));
      CHECK_EQ(int(s[k * 2 + 1]), int(cur + 1));
    }
    CHECK_EQ(int(s[44099 * 2]), 0);  // before the start: silence
    CHECK_EQ(int(s[44100 * 2]), 1);  // exactly at 1 s: frame 0
  }
  remove_file(wav);
  remove_file(temp_path("ramp.mid"));
}

// The scripted scenario of §10.1, returning the capture's bytes.
std::vector<uint8_t> mixer_scenario(uint32_t R, std::vector<std::string>* answers = nullptr) {
  std::string wav = temp_path("scenario_" + std::to_string(R) + ".wav");
  std::vector<uint8_t> bytes;
  {
    auto e = make_engine(capture_config(wav, R));
    auto note = [&](const std::string& s) {
      if (answers) answers->push_back(s);
    };
    // Buffer A: 8-bit mono 11025 Hz, a sawtooth with noise, 0.5 s.
    std::vector<uint8_t> a(5512);
    Lcg r{7};
    for (size_t i = 0; i < a.size(); i++) a[i] = uint8_t((i * 3) + (r.next() >> 29));
    BufferId ba = e->create_buffer(pcm_format(11025, 1, 8), uint32_t(a.size()));
    e->write_buffer(ba, 0, a, 0);
    // Buffer B: 16-bit stereo 22050 Hz, 0.25 s of two triangle tones (integer
    // math only: the golden hash must not depend on a C library's sin()).
    auto tri = [](size_t i, size_t period, int amp) {
      int64_t ph = int64_t(i % period), half = int64_t(period / 2);
      int64_t v = ph < half ? ph : 2 * half - ph;  // 0..half..0
      return int16_t((v * 2 - half) * amp / half);
    };
    std::vector<int16_t> bs(5512 * 2);
    for (size_t i = 0; i < 5512; i++) {
      bs[i * 2] = tri(i, 126, 12000);
      bs[i * 2 + 1] = tri(i, 48, 9000);
    }
    BufferId bb = e->create_buffer(pcm_format(22050, 2, 16), uint32_t(bs.size() * 2));
    e->write_buffer(bb, 0, pcm16(bs), 0);
    VoiceId v1 = e->create_voice(ba, Bus::wave), v2 = e->create_voice(bb, Bus::wave),
            v3 = e->create_voice(bb, Bus::wave);  // a duplicate sharing B
    e->set_gain(v1, gain_from_ds(-600, -2000), 0);
    e->play(v1, true, 0);
    e->play(v2, false, 123456);
    e->set_rate(v1, 16000, 400000);
    e->set_gain(v2, gain_from_ds(-300, 3000), 200000);
    // write_buffer while playing: audible from its time.
    std::vector<uint8_t> patch(2000, 0x40);
    e->write_buffer(ba, 1000, patch, 700000);
    e->play(v3, true, 750000);
    e->set_bus_gain(Bus::wave, gain_from_mm(0xC000C000), 900000);
    e->stop(v3, 1100000);
    note("v1 " + std::to_string(e->cursor(v1, 1100000)) + " v3 " + std::to_string(e->cursor(v3, 1100000)));
    // A stream with pause and reset.
    StreamId s = e->open_stream(pcm_format(8000, 1, 16), Bus::wave, 1000000);
    std::vector<int16_t> ch(800);
    for (int k = 0; k < 4; k++) {
      for (size_t i = 0; i < ch.size(); i++) ch[i] = int16_t((k + 1) * 3000 * ((i / 20) % 2 ? 1 : -1));
      e->stream_write(s, pcm16(ch), 0x1000 + k, 1000000);
    }
    e->stream_pause(s, 1150000);
    note("pos " + std::to_string(e->stream_position(s, 1200000)));
    e->stream_restart(s, 1300000);
    e->set_stream_gain(s, Gain{0x6000, 0x2000}, 1350000);
    e->stream_reset(s, 1500000);
    e->stream_write(s, pcm16(ch), 0x2000, 1600000);
    std::vector<Event> ev;
    e->poll(2000000, ev);
    for (const Event& x : ev)
      note(std::to_string(int(x.kind)) + " " + std::to_string(x.id) + " " + std::to_string(x.cookie) + " @" +
           std::to_string(x.at));
    note("v1 " + std::to_string(e->cursor(v1, 2000000)) + " v2 playing " + std::to_string(e->playing(v2, 2000000)));
    e->shutdown(2000000);
  }
  bytes = read_file(wav);
  remove_file(wav);
  remove_file(temp_path("scenario_" + std::to_string(R) + ".mid"));
  return bytes;
}

TEST(mixer_determinism_and_golden_hash) {
  std::vector<uint8_t> a = mixer_scenario(44100), b = mixer_scenario(44100);
  CHECK(!a.empty());
  CHECK(a == b);
  CHECK_EQ(a.size(), size_t(44 + 88200 * 4));
  uint64_t h = fnv1a64(a.data(), a.size());
  fprintf(stderr, "  scenario capture fnv1a64 %016llx\n", (unsigned long long)h);
  // The golden value: any change to the mixer's arithmetic shows here.
  CHECK_EQ(h, 0x23e7083ff93590a2ull);
  // Not silent, and saturating nowhere near every sample.
  size_t nonzero = 0;
  for (size_t i = 44; i + 1 < a.size(); i += 2) nonzero += (a[i] | a[i + 1]) != 0;
  CHECK(nonzero > 100000);
}

TEST(answers_do_not_depend_on_the_mixer_rate) {
  std::vector<std::string> a, b, c;
  mixer_scenario(22050, &a);
  mixer_scenario(44100, &b);
  mixer_scenario(48000, &c);
  CHECK(!a.empty());
  CHECK(a == b);
  CHECK(b == c);
  for (auto& s : b) fprintf(stderr, "  %s\n", s.c_str());
}

// ============================================================================================
// streams

TEST(streams_chunks_pause_reset) {
  std::string wav = temp_path("stream.wav");
  {
    auto e = make_engine(capture_config(wav, 8000));
    StreamId s = e->open_stream(pcm_format(8000, 1, 16), Bus::wave, 0);
    CHECK(s != 0);
    CHECK_EQ(e->open_stream(ima_adpcm_format(8000, 1), Bus::wave, 0), 0u);
    std::vector<int16_t> c(800);  // 100 ms each, a distinct value per chunk
    for (int k = 0; k < 3; k++) {
      std::fill(c.begin(), c.end(), int16_t(1000 * (k + 1)));
      e->stream_write(s, pcm16(c), 0xA0 + k, 0);
    }
    e->stream_write(s, {}, 0xAF, 0);  // empty: done when playback reaches it
    CHECK_EQ(e->stream_position(s, 250000), uint64_t(2000 * 2));
    std::vector<Event> ev;
    e->poll(299999, ev);
    CHECK_EQ(ev.size(), size_t(2));
    e->poll(300000, ev);
    CHECK_EQ(ev.size(), size_t(4));
    if (ev.size() == 4) {
      CHECK(ev[0].cookie == 0xA0 && ev[0].at == 100000 && ev[0].kind == Event::Kind::chunk_done);
      CHECK(ev[1].cookie == 0xA1 && ev[1].at == 200000);
      CHECK(ev[2].cookie == 0xA2 && ev[2].at == 300000);
      CHECK(ev[3].cookie == 0xAF && ev[3].at == 300000);
    }
    CHECK_EQ(e->stream_position(s, 250000), uint64_t(2400 * 2));  // taken as 300000, the latest seen
    CHECK_EQ(e->stream_position(s, 900000), uint64_t(2400 * 2));  // starved: holds
    // Written after starving: plays from its write time.
    std::fill(c.begin(), c.end(), int16_t(4000));
    e->stream_write(s, pcm16(c), 0xB0, 1000000);
    e->stream_pause(s, 1050000);
    CHECK(e->stream_paused(s));
    CHECK_EQ(e->stream_position(s, 1500000), uint64_t(2800 * 2));
    e->stream_restart(s, 2000000);
    CHECK(!e->stream_paused(s));
    ev.clear();
    e->poll(2049999, ev);
    CHECK(ev.empty());
    e->poll(2050000, ev);
    CHECK(ev.size() == 1 && ev[0].cookie == 0xB0 && ev[0].at == 2050000);
    // Reset: everything queued is done at once, in order; position back to 0.
    for (int k = 0; k < 3; k++) e->stream_write(s, pcm16(c), 0xC0 + k, 3000000);
    ev.clear();
    e->poll(3100000, ev);
    CHECK(ev.size() == 1 && ev[0].cookie == 0xC0);
    e->stream_reset(s, 3150000);
    CHECK_EQ(e->stream_position(s, 3150000), uint64_t(0));
    ev.clear();
    e->poll(3150000, ev);
    CHECK(ev.size() == 2 && ev[0].cookie == 0xC1 && ev[1].cookie == 0xC2 && ev[0].at == 3150000);
    // Close: its events are still polled.
    e->stream_write(s, pcm16(c), 0xD0, 4000000);
    e->close_stream(s, 4010000);
    ev.clear();
    e->poll(5000000, ev);
    CHECK(ev.size() == 1 && ev[0].cookie == 0xD0 && ev[0].at == 4010000);
    CHECK_EQ(e->stream_position(s, 5000000), uint64_t(0));
    e->shutdown(5000000);
  }
  // Gapless: at the mixer rate = the stream rate, each chunk's value exactly
  // over its 800 frames.
  std::vector<int16_t> out = read_capture(wav);
  CHECK_EQ(out.size(), size_t(40000 * 2));
  if (out.size() == 80000) {
    for (int k = 0; k < 3; k++) {
      CHECK_EQ(int(out[size_t(k * 800) * 2]), 1000 * (k + 1));
      CHECK_EQ(int(out[size_t(k * 800 + 799) * 2]), 1000 * (k + 1));
    }
    CHECK_EQ(int(out[2400 * 2]), 0);          // starved
    CHECK_EQ(int(out[8000 * 2]), 4000);       // resumed at 1 s
    CHECK_EQ(int(out[8399 * 2]), 4000);
    CHECK_EQ(int(out[8400 * 2]), 0);          // paused at 1.05 s
    CHECK_EQ(int(out[16000 * 2]), 4000);      // restarted at 2 s
    CHECK_EQ(int(out[16399 * 2]), 4000);
    CHECK_EQ(int(out[16400 * 2]), 0);
  }
  remove_file(wav);
  remove_file(temp_path("stream.mid"));
}

// The live ring between the virtual clock (the engine writes 735 frames per
// 60 Hz frame) and a device crystal 300 ppm off either way, over an hour of
// simulated 10 ms device periods: drift control holds the fill near its
// target, so there is never an underrun (silence + re-prefill) nor a 100 ms
// jump. Without it the hour drifts ~1 s: an underrun, or a jump, every few
// minutes (the residual gap audio's reports listed).
TEST(live_ring_drift_control) {
  for (double ppm : {300.0, -300.0}) {
    const uint32_t rate = 44100;
    const size_t target = rate * 60 / 1000, slack = rate / 10, band = rate / 50;
    PcmRing ring(size_t(rate) * 2 + target);
    PcmConsumer c(ring, target, slack, band);
    std::vector<int16_t> in(735 * 2, 1000), out(441 * 2);
    const double device_period_us = 10000.0 * (1.0 + ppm * 1e-6);
    double next_device = 0;
    uint64_t next_frame = 0;
    size_t lo = SIZE_MAX, hi = 0;
    size_t lost = 0;
    const uint64_t hour_us = 3600ull * 1000000ull;
    for (uint64_t frame = 0; next_frame < hour_us; frame++) {
      lost += ring.push(in.data(), 735);
      next_frame = (frame + 1) * 16667;
      while (next_device < double(next_frame)) {
        c.fill(out.data(), 441);
        next_device += device_period_us;
        if (next_device > 10.0e6) {  // after the first 10 s: settled
          lo = std::min(lo, ring.fill());
          hi = std::max(hi, ring.fill());
        }
      }
    }
    CHECK_EQ(c.underruns, uint64_t(0));
    CHECK_EQ(c.dropped, uint64_t(0));
    CHECK_EQ(lost, size_t(0));
    CHECK(ppm > 0 ? c.trimmed > 0 : c.padded > 0);  // a slow device: trimmed; a fast one: padded
    CHECK(lo + band + 735 + 441 >= target && hi <= target + band + 735 + 441);
    fprintf(stderr, "  %+.0f ppm: fill %zu..%zu (target %zu), %llu trimmed, %llu padded\n", ppm, lo, hi, target,
            (unsigned long long)c.trimmed, (unsigned long long)c.padded);
  }
}

// Guest-sized memory has a ceiling: all buffers together at most 512 MB, and
// a stream holds at most 64 MB unplayed (a paused one keeps every write) —
// past that a write is handed back at once, so a guest waiting on it goes on.
TEST(guest_sized_memory_has_a_ceiling) {
  auto e = make_engine(quiet_config());
  BufferId a = e->create_buffer(pcm_format(22050, 1, 16), 256u << 20);
  BufferId b = e->create_buffer(pcm_format(22050, 1, 16), 256u << 20);
  CHECK(a != 0 && b != 0);
  CHECK_EQ(e->create_buffer(pcm_format(22050, 1, 16), 2), 0u);
  e->release_buffer(a);
  BufferId c = e->create_buffer(pcm_format(22050, 1, 16), 2);
  CHECK(c != 0);
  e->release_buffer(b);
  e->release_buffer(c);

  StreamId s = e->open_stream(pcm_format(44100, 2, 16), Bus::wave, 0);
  e->stream_pause(s, 0);
  std::vector<uint8_t> mb(1u << 20);
  for (uint64_t k = 0; k < 64; k++) e->stream_write(s, mb, 0x100 + k, 1000);
  std::vector<Event> ev;
  e->poll(1000, ev);
  CHECK(ev.empty());  // 64 MB queued, paused: nothing done
  e->stream_write(s, mb, 0x200, 2000);
  e->poll(2000, ev);
  CHECK(ev.size() == 1 && ev[0].cookie == 0x200 && ev[0].at == 2000 && ev[0].kind == Event::Kind::chunk_done);
  e->stream_reset(s, 3000);
  ev.clear();
  e->poll(3000, ev);
  CHECK_EQ(ev.size(), size_t(64));  // the queued ones, handed back by the reset
  e->stream_write(s, mb, 0x300, 4000);  // room again
  ev.clear();
  e->poll(4000, ev);
  CHECK(ev.empty());
  e->close_stream(s, 5000);
  e->shutdown(5000);
}

// ============================================================================================
// SMF

TEST(smf_parse_times) {
  // Format 1: tempo map in track 0, notes (running status) in track 1.
  Trk t0, t1;
  t0.tempo(0, 500000).tempo(192, 250000).eot(96);
  t1.ev(0, {0x90, 60, 100}).ev(96, {62, 100}).ev(96, {0x80, 60, 0}).ev(96, {0xF0, 0x03, 0x7E, 0x09, 0xF7})
      .ev(0, {0x90, 64, 0})
      .eot(10);
  SmfSong s;
  std::string err;
  CHECK(parse_smf(smf(1, 96, {t0, t1}), s, &err));
  CHECK_EQ(s.format, 1);
  CHECK_EQ(s.tracks, 2);
  std::vector<std::pair<uint64_t, uint8_t>> notes;
  for (auto& e : s.events)
    if (e.status != 0xFF) notes.push_back({e.us, e.status});
  CHECK_EQ(notes.size(), size_t(5));
  if (notes.size() == 5) {
    CHECK(notes[0].first == 0 && notes[0].second == 0x90);
    CHECK(notes[1].first == 500000 && notes[1].second == 0x90);  // running status
    CHECK(notes[2].first == 1000000 && notes[2].second == 0x80);
    CHECK(notes[3].first == 1250000 && notes[3].second == 0xF0);  // after the tempo change
    CHECK(notes[4].first == 1250000 && notes[4].second == 0x90);
  }
  CHECK_EQ(s.length_ticks, uint64_t(298));
  CHECK_EQ(s.length_us, uint64_t(1250000 + 10 * 250000 / 96));
  CHECK_EQ(s.channels_used(), 1);
  // SMPTE: 25 fps x 40 ticks = 1000 ticks per second; tempo ignored.
  Trk sm;
  sm.tempo(0, 1000000).ev(1000, {0x91, 60, 1}).eot(500);
  CHECK(parse_smf(smf(0, int16_t(0xE728), {sm}), s));
  CHECK(s.events.size() == 3 && s.events[1].us == 1000000);
  CHECK_EQ(s.length_us, uint64_t(1500000));
  CHECK(parse_smf(smf(0, int16_t(0xE302), {Trk().ev(2997, {0x90, 1, 1}).eot()}), s));  // 29.97 fps x 2
  CHECK_EQ(s.events[0].us, uint64_t(50000000));  // 2997 ticks at 59.94 per second
  // Refusals and tolerance.
  CHECK(!parse_smf(smf(2, 96, {Trk().eot()}), s, &err));
  CHECK(err.find("format 2") != std::string::npos);
  CHECK(!parse_smf(std::vector<uint8_t>{'M', 'T', 'h', 'd', 0, 0}, s));
  CHECK(!parse_smf(std::vector<uint8_t>{'R', 'I', 'F', 'F'}, s));
  std::vector<uint8_t> cut = smf(0, 96, {Trk().ev(0, {0x90, 60, 100}).ev(96, {0x80, 60, 0}).eot()});
  cut.resize(cut.size() - 6);  // the track ends mid-event
  CHECK(parse_smf(cut, s));
  CHECK_EQ(s.events.size(), size_t(1));
  CHECK(!s.warnings.empty());
  // RMID wrapper.
  std::vector<uint8_t> inner = smf(0, 96, {Trk().ev(0, {0x90, 60, 100}).eot(96)});
  std::vector<uint8_t> rmid = {'R', 'I', 'F', 'F'};
  le32(rmid, uint32_t(inner.size() + 12));
  rmid.insert(rmid.end(), {'R', 'M', 'I', 'D', 'd', 'a', 't', 'a'});
  le32(rmid, uint32_t(inner.size()));
  rmid.insert(rmid.end(), inner.begin(), inner.end());
  CHECK(parse_smf(rmid, s) && s.length_us == 500000);
}

TEST(smf_mpc_rule) {
  // Channels 1 and 13 (0-based 0 and 12): dual-mode, 13..16 dropped.
  auto dual = smf(0, 96, {Trk()
                              .ev(0, {0x90, 60, 100})
                              .ev(0, {0x9C, 60, 100})
                              .ev(0, {0xBE, 7, 90})
                              .ev(0, {0x9F, 40, 100})
                              .ev(96, {0x80, 60, 0})
                              .eot()});
  SmfSong s;
  CHECK(parse_smf(dual, s));
  CHECK(apply_mpc_rule(s));
  CHECK_EQ(s.channels_used(), 0x0001);
  // 15/16 without 13 (TOASTER1.MID's shape): left alone.
  auto keep = smf(0, 96, {Trk().ev(0, {0x90, 60, 100}).ev(0, {0x9E, 60, 100}).ev(0, {0x9F, 60, 100}).eot(96)});
  CHECK(parse_smf(keep, s));
  CHECK(!apply_mpc_rule(s));
  CHECK_EQ(s.channels_used(), 0xC001);
  // 13 alone (no 1..10): left alone; 11-12 do not count as extended.
  auto base_only = smf(0, 96, {Trk().ev(0, {0x9A, 60, 100}).ev(0, {0x9C, 60, 100}).eot(96)});
  CHECK(parse_smf(base_only, s));
  CHECK(!apply_mpc_rule(s));
  // Through the engine: dropped, unless ADMIDIBASE.
  for (bool base : {false, true}) {
    std::string wav = temp_path(base ? "mpc1.wav" : "mpc0.wav");
    Config c = capture_config(wav);
    c.mpc_base_channels = base;
    {
      auto e = make_engine(c);
      SongId id = e->load_song(dual);
      CHECK(id != 0);
      e->song_play(id, 0);
      e->shutdown(2000000);
    }
    bool high = false;
    for (auto& l : read_midi_log(c.capture_mid))
      if (l.m[0] < 0xF0 && (l.m[0] & 15) >= 12) high = true;
    CHECK_EQ(high, base);
    remove_file(wav);
    remove_file(c.capture_mid);
  }
}

TEST(song_playback_log_seek_chase_and_volume) {
  std::string wav = temp_path("song.wav");
  Config c = capture_config(wav);
  // 500 PPQN at 120 bpm: 1 tick = 1 ms.
  auto bytes = smf(0, 500, {Trk()
                                .ev(0, {0xC0, 5})
                                .ev(0, {0xB0, 7, 80})
                                .ev(0, {0x90, 60, 100})
                                .ev(100, {0xB0, 10, 20})
                                .ev(100, {0xE0, 0, 80})
                                .ev(100, {0x80, 60, 0})
                                .ev(0, {0x91, 62, 90})
                                .ev(0, {0xF0, 0x03, 0x7E, 0x09, 0xF7})
                                .ev(700, {0x81, 62, 0})
                                .eot(0)});
  {
    auto e = make_engine(c);
    std::string err;
    CHECK_EQ(e->load_song(std::vector<uint8_t>{1, 2, 3}, &err), 0u);
    CHECK(!err.empty());
    SongId s = e->load_song(bytes);
    CHECK(s != 0);
    CHECK_EQ(e->song_length(s), uint64_t(1000000));
    e->song_play(s, 1000000);
    CHECK(e->song_playing(s, 1000000));
    CHECK_EQ(e->song_position(s, 1250000), uint64_t(250000));
    // Half MIDI volume: CC7 re-sent, scaled.
    e->set_bus_gain(Bus::midi, gain_from_mm(0x7FFF7FFF), 1250000);
    e->song_stop(s, 1350000);  // the note on channel 2 is sounding
    CHECK(!e->song_playing(s, 1400000));
    CHECK_EQ(e->song_position(s, 1400000), uint64_t(350000));
    e->song_play(s, 2000000);  // resumes; no chase without a seek
    std::vector<Event> ev;
    e->poll(2650000, ev);
    CHECK(ev.size() == 1 && ev[0].kind == Event::Kind::song_end && ev[0].id == s && ev[0].at == 2650000);
    CHECK(!e->song_playing(s, 2650000));
    // At the end already: song_end at once.
    e->song_play(s, 3000000);
    ev.clear();
    e->poll(3000000, ev);
    CHECK(ev.size() == 1 && ev[0].at == 3000000);
    // Seek into the middle: the chase restores program, pan and bend.
    e->song_seek(s, 250000, 4000000);
    CHECK_EQ(e->song_position(s, 4000000), uint64_t(250000));
    e->song_play(s, 4000000);
    e->song_seek(s, 5000000, 4100000);  // clamped to the length; stops first
    CHECK_EQ(e->song_position(s, 4100000), uint64_t(1000000));
    CHECK(!e->song_playing(s, 4100000));
    // close drops what is pending.
    e->song_seek(s, 0, 5000000);
    e->song_play(s, 5000000);
    e->close_song(s, 5100000);
    ev.clear();
    e->poll(9000000, ev);
    CHECK(ev.empty());
    CHECK_EQ(e->song_length(s), uint64_t(0));
    e->shutdown(9000000);
  }
  auto log = read_midi_log(c.capture_mid);
  auto find = [&](uint64_t ms, std::vector<uint8_t> m) {
    for (auto& l : log)
      if (l.ms == ms && l.m == m) return true;
    return false;
  };
  auto count = [&](std::vector<uint8_t> m) {
    int n = 0;
    for (auto& l : log) n += l.m == m;
    return n;
  };
  CHECK(find(1000, {0xB0, 7, 100}));   // at play: CC7 of every channel used (default 100)
  CHECK(find(1000, {0xB1, 7, 100}));
  CHECK(find(1000, {0xC0, 5}));
  CHECK(find(1000, {0xB0, 7, 80}));    // the song's own CC7, at full bus gain
  CHECK(find(1000, {0x90, 60, 100}));
  CHECK(find(1100, {0xB0, 10, 20}));
  CHECK(find(1200, {0xE0, 0, 80}));
  CHECK(find(1250, {0xB0, 7, 40}));    // bus at half: 80 -> 40
  CHECK(find(1250, {0xB1, 7, 50}));    // 100 -> 50
  CHECK(find(1300, {0x91, 62, 90}));
  CHECK(find(1300, {0xF0, 0x7E, 0x09, 0xF7}));
  CHECK(find(1350, {0x81, 62, 0}));    // stop: note-off for what sounds
  CHECK(find(1350, {0xB0, 123, 0}));   // then CC123 on the channels used
  CHECK(find(1350, {0xB1, 123, 0}));
  CHECK(find(2000, {0xB0, 7, 40}));    // resumed: CC7 again (scaled)
  CHECK(find(2650, {0x81, 62, 0}));    // the song's own note-off, at its end
  // The seek to 250 ms: program, pan, bend chased; the channel volume scaled.
  CHECK(find(4000, {0xC0, 5}));
  CHECK(find(4000, {0xB0, 10, 20}));
  CHECK(find(4000, {0xE0, 0, 80}));
  CHECK(find(4000, {0xB0, 7, 40}));
  CHECK(find(4050, {0x91, 62, 90}));
  CHECK_EQ(count({0x90, 60, 100}), 2);  // at 1000 and at 5000 (not replayed by the seek)
  CHECK(find(5100, {0xB0, 123, 0}));    // close
  // Times are nondecreasing in the log.
  for (size_t i = 1; i < log.size(); i++) CHECK(log[i].ms >= log[i - 1].ms);
  remove_file(wav);
  remove_file(c.capture_mid);
}

// ============================================================================================
// captures

TEST(capture_headers_patch_on_the_five_second_grid) {
  std::string wav = temp_path("grid.wav");
  Config c = capture_config(wav, 22050);
  auto e = make_engine(c);
  SongId s = e->load_song(smf(0, 500, {Trk().ev(0, {0x90, 60, 100}).ev(6000, {0x80, 60, 0}).eot()}));
  e->song_play(s, 0);
  e->advance(4900000);
  uint32_t frames = 0;
  // Before the first patch: a readable header with no data yet.
  read_capture(wav, nullptr, &frames);
  CHECK_EQ(frames, 0u);
  e->advance(5500000);
  // A host killed now leaves readable files: the WAV to its last patch, the
  // .mid log without its end of track.
  uint32_t rate = 0;
  std::vector<int16_t> s1 = read_capture(wav, &rate, &frames);
  CHECK_EQ(rate, 22050u);
  CHECK(frames >= 5 * 22050);
  SmfSong log;
  auto ev = read_midi_log(c.capture_mid, &log);
  CHECK(ev.size() >= 1 && ev[0].m == std::vector<uint8_t>({0xB0, 7, 100}));
  e->shutdown(6543210);
  std::vector<int16_t> s2 = read_capture(wav, &rate, &frames);
  CHECK_EQ(frames, uint32_t(6543210ull * 22050 / 1000000));
  std::vector<uint8_t> hdr = read_file(wav);
  CHECK(hdr.size() >= 44 && hdr[22] == 2 && hdr[34] == 16);
  ev = read_midi_log(c.capture_mid, &log);
  CHECK_EQ(log.division, 500);
  CHECK_EQ(log.format, 0);
  CHECK(!ev.empty() && ev.back().ms == 6000 && ev.back().m == std::vector<uint8_t>({0x80, 60, 0}));
  CHECK_EQ(log.length_us / 1000, uint64_t(6543));  // the end of track at shutdown
  // After shutdown: accepted, nothing written.
  e->song_play(s, 7000000);
  e->advance(9000000);
  CHECK_EQ(read_file(wav).size(), hdr.size());
  e.reset();
  remove_file(wav);
  remove_file(c.capture_mid);
  // An uncreatable capture is logged and the engine still works.
  Config bad = capture_config("Z:\\no\\such\\dir\\x.wav");
  auto e2 = make_engine(bad);
  CHECK(e2->enabled());
  BufferId b = e2->create_buffer(pcm_format(22050, 1, 8), 100);
  VoiceId v = e2->create_voice(b, Bus::wave);
  e2->play(v, false, 0);
  CHECK(e2->playing(v, 1000));
}

TEST(shutdown_silences_playing_songs) {
  std::string wav = temp_path("shut.wav");
  Config c = capture_config(wav);
  {
    auto e = make_engine(c);
    SongId s = e->load_song(smf(0, 500, {Trk().ev(0, {0x93, 60, 100}).ev(5000, {0x83, 60, 0}).eot()}));
    e->song_play(s, 0);
    e->shutdown(1000000);
    CHECK(e->song_playing(s, 1000000));  // the guest's view is unchanged
    CHECK_EQ(e->stats().songs_started, uint64_t(1));
  }
  auto log = read_midi_log(c.capture_mid);
  CHECK(log.size() >= 3);
  if (log.size() >= 3) {
    CHECK(log[log.size() - 2].m == std::vector<uint8_t>({0x83, 60, 0}) && log[log.size() - 2].ms == 1000);
    CHECK(log.back().m == std::vector<uint8_t>({0xB3, 123, 0}));
  }
  remove_file(wav);
  remove_file(c.capture_mid);
}

// ============================================================================================
// config

TEST(config_from_env) {
  std::vector<std::string> w;
  Config c = Config::from_env(env_of({}), &w);
  CHECK(!c.guest_sound);
  CHECK(!c.live);
  CHECK_EQ(c.volume, 50);
  CHECK_EQ(c.rate, 44100u);
  CHECK_EQ(c.latency_ms, 80u);
  CHECK(c.live_midi);
  CHECK_EQ(c.midi_device, -1);
  CHECK(!c.mpc_base_channels);
  CHECK(c.capture_wav.empty() && c.capture_mid.empty());
  CHECK(w.empty());

  c = Config::from_env(env_of({{"ADSOUND", "1"}}));
  CHECK(c.guest_sound && !c.live);  // headless: never live
  c = Config::from_env(env_of({{"ADSOUND", "1"}, {"ADSTREAM", "1"}}));
  CHECK(c.guest_sound && c.live);
  c = Config::from_env(env_of({{"ADSOUND", "1"}, {"ADSTREAM", "1"}, {"ADAUDIOLIVE", "0"}}));
  CHECK(c.guest_sound && !c.live);
  c = Config::from_env(env_of({{"ADSOUND", "0"}, {"ADSTREAM", "1"}, {"ADAUDIOOUT", "C:\\x\\cap.WAV"}}));
  CHECK(c.guest_sound && !c.live);  // a capture alone never plays
  CHECK_EQ(c.capture_wav, std::string("C:\\x\\cap.WAV"));
  CHECK_EQ(c.capture_mid, std::string("C:\\x\\cap.mid"));
  c = Config::from_env(env_of({{"ADAUDIOOUT", "C:\\a.b\\out"}}));
  CHECK_EQ(c.capture_mid, std::string("C:\\a.b\\out.mid"));
  c = Config::from_env(env_of({{"ADAUDIOOUT", "run.mid"}}));
  CHECK_EQ(c.capture_mid, std::string("run.mid.mid"));
  c = Config::from_env(env_of({{"ADAUDIOOUT", "  "}}));
  CHECK(!c.guest_sound);

  w.clear();
  c = Config::from_env(env_of({{"ADVOLUME", "150"}, {"ADAUDIORATE", "1000"}, {"ADAUDIOLATENCYMS", "9999"},
                               {"ADMIDIDEV", "x"}}),
                       &w);
  CHECK_EQ(c.volume, 100);
  CHECK_EQ(c.rate, 8000u);
  CHECK_EQ(c.latency_ms, 500u);
  CHECK_EQ(c.midi_device, -1);
  CHECK_EQ(w.size(), size_t(4));
  w.clear();
  c = Config::from_env(env_of({{"ADVOLUME", "loud"}, {"ADAUDIORATE", "48000"}, {"ADMIDIDEV", "2"}}), &w);
  CHECK_EQ(c.volume, 50);
  CHECK_EQ(c.rate, 48000u);
  CHECK_EQ(c.midi_device, 2);
  CHECK_EQ(w.size(), size_t(1));
  c = Config::from_env(env_of({{"ADVOLUME", "-5"}, {"ADMIDI", "0"}, {"ADMIDIBASE", "1"}}));
  CHECK_EQ(c.volume, 0);
  CHECK(!c.live_midi);
  CHECK(c.mpc_base_channels);
  c = Config::from_env(env_of({{"ADVOLUME", "0"}}));
  CHECK_EQ(c.volume, 0);
}

// ============================================================================================
// run_host's wiring (AUDIO.md §3)

namespace {
class AudioProbeLane : public Lane {
 public:
  const char* name() const override { return "probe"; }
  bool init(const std::string&, LaneContext& ctx) override {
    ctx_ = &ctx;
    engine = ctx.audio;
    return true;
  }
  StepResult step() override {
    // advance() ran after the previous step, at that step's time.
    if (steps > 0 && engine)
      rendered_before.push_back(engine->stats().rendered_frames);
    last_step_us = ctx_->clock.now_us();
    steps++;
    return StepResult::ok;
  }
  void shutdown() override {
    // The engine was shut down first: the capture is already finalized.
    if (!wav.empty()) {
      std::vector<uint8_t> b = read_file(wav);
      Wave w;
      if (parse_wave(b, w)) wav_frames_at_shutdown = w.data.size() / 4;
    }
    if (engine) engine_latest_at_shutdown = engine->stats().rendered_frames;
  }
  LaneContext* ctx_ = nullptr;
  Engine* engine = nullptr;
  int steps = 0;
  uint64_t last_step_us = 0;
  std::vector<uint64_t> rendered_before;
  std::string wav;
  size_t wav_frames_at_shutdown = size_t(-1);
  uint64_t engine_latest_at_shutdown = 0;
};
}  // namespace

TEST(run_host_wires_the_engine) {
  std::string wav = temp_path("host.wav");
  {
    AudioProbeLane lane;
    lane.wav = wav;
    Env env = env_of({{"ADFRAMES", "5"}, {"ADAUDIOOUT", wav}});
    HostIo io;
    io.go_wait = false;
    HostResult r = run_host(lane, "probe", env, io);
    CHECK_EQ(r.exit_code, 0);
    CHECK(lane.engine != nullptr && lane.engine->enabled());
    CHECK_EQ(lane.steps, 5);
    CHECK_EQ(lane.rendered_before.size(), size_t(4));
    for (size_t k = 0; k < lane.rendered_before.size(); k++)
      CHECK_EQ(lane.rendered_before[k], uint64_t(k) * 33333 * 44100 / 1000000);
    CHECK_EQ(lane.last_step_us, uint64_t(4 * 33333));
    CHECK_EQ(lane.wav_frames_at_shutdown, size_t(uint64_t(4) * 33333 * 44100 / 1000000));
    // Still usable after run_host returned (a lane's destructor may release).
    lane.engine->destroy_voice(12345, 0);
    CHECK(lane.engine->stats().rendered_frames == lane.engine_latest_at_shutdown);
  }
  remove_file(wav);
  remove_file(temp_path("host.mid"));
  {
    AudioProbeLane lane;
    Env env = env_of({{"ADFRAMES", "2"}});
    HostIo io;
    io.go_wait = false;
    run_host(lane, "probe", env, io);
    CHECK(lane.engine != nullptr && !lane.engine->enabled());  // sound off: a disabled engine
  }
  {
    // --configure: the disabled null engine.
    class ConfigureProbe : public AudioProbeLane {
     public:
      bool can_configure() const override { return true; }
      ConfigureResult configure(const std::string&, LaneContext& ctx, const ConfigureRequest&, std::string*) override {
        seen = ctx.audio;
        return ConfigureResult::nothing;
      }
      Engine* seen = nullptr;
    } lane;
    Env env = env_of({{"ADSOUND", "1"}});
    std::string json;
    configure_module(lane, "probe", env, ConfigureRequest{}, &json);
    CHECK(lane.seen == &null_engine());
  }
}

// ============================================================================================
// the one live check (opt-in)

TEST(live_device_check) {
  const char* v = getenv("AD_AUDIO_LIVE_TEST");
  if (!v || strcmp(v, "1") != 0) {
    fprintf(stderr, "  (AD_AUDIO_LIVE_TEST=1 plays 3 s on the default device: skipped)\n");
    return;
  }
  // The sinks say which path they took (WASAPI or waveOut, the MIDI device).
  set_trace_categories({"audio"});
  Config c;
  c.guest_sound = true;
  c.live = true;
  c.live_midi = true;
  auto e = make_engine(c);
  // 1 s of 440 Hz at -30 dBFS (amplitude 1036), then a soft MIDI note.
  std::vector<int16_t> tone(44100);
  for (size_t i = 0; i < tone.size(); i++) {
    double fade = std::min<double>({1.0, double(i) / 441.0, double(tone.size() - 1 - i) / 441.0});
    tone[i] = int16_t(1036.0 * fade * sin(6.283185307 * 440.0 * double(i) / 44100.0));
  }
  BufferId b = e->create_buffer(pcm_format(44100, 1, 16), uint32_t(tone.size() * 2));
  e->write_buffer(b, 0, pcm16(tone), 0);
  VoiceId voice = e->create_voice(b, Bus::wave);
  SongId s = e->load_song(smf(0, 500, {Trk().ev(0, {0x90, 69, 40}).ev(1000, {0x80, 69, 0}).eot()}));
  e->set_bus_gain(Bus::midi, Gain{0x3000, 0x3000}, 0);  // CC7 about 37: quiet
  const uint64_t t0 = VirtualClock::system_wall_us();
  uint64_t underruns_after_prefill = 0;
  bool started = false;
  for (;;) {
    uint64_t now = VirtualClock::system_wall_us() - t0;
    if (now >= 2600000) break;
    if (!started) {
      e->play(voice, false, now);
      started = true;
    }
    if (now >= 1200000 && !e->song_playing(s, now) && e->song_position(s, now) == 0) e->song_play(s, now);
    e->advance(now);
    if (now > 500000) underruns_after_prefill = e->stats().underruns;
    Sleep(10);
  }
  uint64_t before = e->stats().underruns;
  e->shutdown(VirtualClock::system_wall_us() - t0);
  Stats st = e->stats();
  fprintf(stderr, "  live: %llu frames, %llu underruns, %llu dropped, %llu MIDI events\n",
          (unsigned long long)st.rendered_frames, (unsigned long long)st.underruns,
          (unsigned long long)st.dropped_frames, (unsigned long long)st.midi_events);
  CHECK_EQ(before, underruns_after_prefill);
  CHECK_EQ(st.underruns, uint64_t(0));
  CHECK(st.midi_events >= 3);
}

int main(int argc, char** argv) { return adw_test::run_all(argc > 1 ? argv[1] : nullptr); }
