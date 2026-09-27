// MSACM32.DLL — the audio compression manager, emulated (API_SURFACE.md §1
// "MSACM32.DLL", AUDIO.md §2.2, §7.3). The engine reaches it only from
// XNoise::DecompressWave (ADXPL510 0x429308), after DirectSound opened, to
// turn its IMA-ADPCM WAV resources into PCM16:
//
//   acmMetrics(MAX_SIZE_FORMAT) → acmFormatEnumA(tag 0x11, a GUEST callback
//   that takes the first format of the source's rate) → acmFormatSuggest(PCM)
//   → acmStreamOpen(NONREALTIME) → acmStreamSize(SOURCE) → Prepare →
//   Convert(START) → Unprepare → Close.
//
// No host codec is involved: the formats are the standard ones of Windows'
// imaadp32/msadp32 codecs (audio::ima_adpcm_format / ms_adpcm_format, the
// enumeration order of §2.2), and conversion is core's decoder
// (audio::decode, bit-exact with those codecs). Only ADPCM → PCM16 streams
// exist; everything else is ACMERR_NOTPOSSIBLE.
//
// Sound off (the engine disabled): every call answers MMSYSERR_NODRIVER, as
// the silent host always did, so the engine's noises keep duration 0.
//
// Known gaps, deliberately left (no module of the 202 in the five releases
// asks for them): encoding, PCM format conversion, filters and asynchronous
// streams answer "not supported".
#include <algorithm>
#include <cstdio>
#include <map>
#include <span>
#include <vector>

#include "adw/core/log.h"
#include "win32/audio.hh"
#include "win32/runtime.hh"
#include "win32/shim_families.hh"

namespace adw::win32 {

namespace {

constexpr const char* A = "MSACM32.DLL";

constexpr uint32_t kOk = 0, kInvalidHandle = 5, kNoDriver = 6, kNotSupported = 8, kInvalidParam = 11;
constexpr uint32_t ACMERR_NOTPOSSIBLE = 512, ACMERR_BUSY = 513, ACMERR_UNPREPARED = 514;

// acmMetrics.
constexpr uint32_t ACM_METRIC_COUNT_DRIVERS = 1, ACM_METRIC_COUNT_CODECS = 2, ACM_METRIC_MAX_SIZE_FORMAT = 50;
// acmFormatEnum / acmFormatSuggest.
constexpr uint32_t ENUM_WFORMATTAG = 0x10000, ENUM_NCHANNELS = 0x20000, ENUM_NSAMPLESPERSEC = 0x40000,
                   ENUM_WBITSPERSAMPLE = 0x80000, ENUM_CONVERT = 0x100000, ENUM_SUGGEST = 0x200000;
constexpr uint32_t SUPPORTF_CODEC = 0x1, SUPPORTF_CONVERTER = 0x2;
constexpr uint32_t kFormatDetailsSize = 0x98;  // ACMFORMATDETAILSA
// acmStreamOpen / Size / Convert.
constexpr uint32_t OPENF_QUERY = 0x1, OPENF_ASYNC = 0x2;
constexpr uint32_t SIZEF_SOURCE = 0, SIZEF_DESTINATION = 1, SIZEF_QUERYMASK = 0xF;
constexpr uint32_t CONVERTF_BLOCKALIGN = 0x4;
constexpr uint32_t kStreamHeaderSize = 0x54;  // ACMSTREAMHEADER
constexpr uint32_t STATUSF_DONE = 0x10000, STATUSF_PREPARED = 0x20000, STATUSF_INQUEUE = 0x100000;

// The pseudo driver ids handed to enumeration callbacks, by format tag.
constexpr uint32_t kDriverIdBase = 0x0ACD0000;
constexpr uint32_t kStreamHandleBase = 0x0ACA0001;

struct Stream {
  audio::WaveFormat src, dst;
};

struct AcmState : RuntimeState {
  std::map<uint32_t, Stream> streams;
  uint32_t next = kStreamHandleBase;
};

AcmState& as(Runtime& rt) { return rt.state<AcmState>(); }

// The standard formats of a tag, in the codec's enumeration order (§2.2).
std::vector<audio::WaveFormat> standard_formats(uint16_t tag) {
  std::vector<audio::WaveFormat> out;
  static constexpr uint32_t kRates[] = {8000, 11025, 22050, 44100};
  for (uint32_t rate : kRates) {
    if (tag == audio::kTagPcm) {
      for (uint16_t bits : {uint16_t(8), uint16_t(16)})
        for (uint16_t ch : {uint16_t(1), uint16_t(2)}) out.push_back(audio::pcm_format(rate, ch, bits));
    } else {
      for (uint16_t ch : {uint16_t(1), uint16_t(2)})
        out.push_back(tag == audio::kTagImaAdpcm ? audio::ima_adpcm_format(rate, ch) : audio::ms_adpcm_format(rate, ch));
    }
  }
  return out;
}

// "22.050 kHz, 4 Bit, Mono".
std::string format_name(const audio::WaveFormat& f) {
  char s[64];
  snprintf(s, sizeof(s), "%u.%03u kHz, %u Bit, %s", f.rate / 1000, f.rate % 1000, f.bits,
           f.channels == 2 ? "Stereo" : "Mono");
  return s;
}

bool is_adpcm(const audio::WaveFormat& f) { return f.tag == audio::kTagImaAdpcm || f.tag == audio::kTagMsAdpcm; }

// The PCM16 an ADPCM source decodes to (same rate and channels).
audio::WaveFormat pcm16_of(const audio::WaveFormat& src) { return audio::pcm_format(src.rate, src.channels, 16); }

uint32_t format_enum(Call& c) {
  // (had, pafd, fnCallback, dwInstance, fdwEnum)
  Runtime& rt = c.rt;
  auto& mem = c.mem();
  uint32_t had = c.arg(0), pafd = c.arg(1), cb = c.arg(2), inst = c.arg(3), flags = c.arg(4);
  if (had) return kInvalidHandle;
  if (!pafd || !cb || mem.read_u32l(pafd) < kFormatDetailsSize) return kInvalidParam;
  uint32_t pwfx = mem.read_u32l(pafd + 0x10), cbwfx = mem.read_u32l(pafd + 0x14);
  if (!pwfx || cbwfx < 16) return kInvalidParam;
  // The filter, from the caller's pwfx before anything is written there.
  uint16_t want_tag = mem.read_u16l(pwfx), want_ch = mem.read_u16l(pwfx + 2), want_bits = mem.read_u16l(pwfx + 14);
  uint32_t want_rate = mem.read_u32l(pwfx + 4);
  std::vector<std::pair<uint16_t, std::vector<audio::WaveFormat>>> lists;
  if (flags & (ENUM_CONVERT | ENUM_SUGGEST)) {
    // What pwfx converts to: an ADPCM source decodes to PCM16, nothing else.
    audio::WaveFormat src;
    if (read_guest_waveformat(rt, pwfx, src) && is_adpcm(src) && audio::decodable(src))
      lists.push_back({audio::kTagPcm, {pcm16_of(src)}});
  } else if (flags & ENUM_WFORMATTAG) {
    if (want_tag == audio::kTagPcm || want_tag == audio::kTagMsAdpcm || want_tag == audio::kTagImaAdpcm)
      lists.push_back({want_tag, standard_formats(want_tag)});
  } else {
    for (uint16_t t : {audio::kTagPcm, audio::kTagMsAdpcm, audio::kTagImaAdpcm}) lists.push_back({t, standard_formats(t)});
  }
  for (auto& [tag, formats] : lists) {
    uint32_t index = 0;
    for (const audio::WaveFormat& f : formats) {
      uint32_t i = index++;
      if ((flags & ENUM_NCHANNELS) && f.channels != want_ch) continue;
      if ((flags & ENUM_NSAMPLESPERSEC) && f.rate != want_rate) continue;
      if ((flags & ENUM_WBITSPERSAMPLE) && f.bits != want_bits) continue;
      std::vector<uint8_t> bytes = audio::waveformat_bytes(f);
      if (bytes.size() > cbwfx) continue;  // does not fit the caller's buffer
      uint32_t support = tag == audio::kTagPcm ? SUPPORTF_CONVERTER : SUPPORTF_CODEC;
      mem.write_u32l(pafd + 0x04, i);
      mem.write_u32l(pafd + 0x08, tag);
      mem.write_u32l(pafd + 0x0C, support);
      mem.memcpy(pwfx, bytes.data(), bytes.size());
      write_cstr(mem, pafd + 0x18, format_name(f), 128);
      // BOOL CALLBACK acmFormatEnumCallback(HACMDRIVERID, LPACMFORMATDETAILS, DWORD_PTR, DWORD)
      uint32_t more = rt.call_guest(cb, {kDriverIdBase | tag, pafd, inst, support}, Conv::stdcall_);
      trace("sound", "acmFormatEnum: tag 0x%X #%u %s -> %s", tag, i, format_name(f).c_str(), more ? "next" : "stop");
      if (!more) return kOk;
    }
  }
  return kOk;
}

uint32_t format_suggest(Call& c) {
  // (had, pwfxSrc, pwfxDst, cbwfxDst, fdwSuggest)
  Runtime& rt = c.rt;
  auto& mem = c.mem();
  uint32_t had = c.arg(0), psrc = c.arg(1), pdst = c.arg(2), cb = c.arg(3), flags = c.arg(4);
  if (had) return kInvalidHandle;
  if (!psrc || !pdst || cb < 16) return kInvalidParam;
  audio::WaveFormat src;
  if (!read_guest_waveformat(rt, psrc, src) || !audio::decodable(src)) return ACMERR_NOTPOSSIBLE;
  audio::WaveFormat dst = audio::decoded_format(src);
  if ((flags & ENUM_WFORMATTAG) && mem.read_u16l(pdst) != audio::kTagPcm) return ACMERR_NOTPOSSIBLE;
  if ((flags & ENUM_NCHANNELS) && mem.read_u16l(pdst + 2) != dst.channels) return ACMERR_NOTPOSSIBLE;
  if ((flags & ENUM_NSAMPLESPERSEC) && mem.read_u32l(pdst + 4) != dst.rate) return ACMERR_NOTPOSSIBLE;
  if ((flags & ENUM_WBITSPERSAMPLE) && mem.read_u16l(pdst + 14) != dst.bits) return ACMERR_NOTPOSSIBLE;
  write_guest_waveformat(rt, pdst, dst, cb);
  return kOk;
}

uint32_t stream_open(Call& c) {
  // (phas, had, pwfxSrc, pwfxDst, pwfltr, dwCallback, dwInstance, fdwOpen)
  Runtime& rt = c.rt;
  uint32_t phas = c.arg(0), psrc = c.arg(2), pdst = c.arg(3), pfltr = c.arg(4), flags = c.arg(7);
  if (phas && !(flags & OPENF_QUERY)) c.mem().write_u32l(phas, 0);
  if (!psrc || !pdst) return kInvalidParam;
  audio::WaveFormat src, dst;
  if (!read_guest_waveformat(rt, psrc, src) || !read_guest_waveformat(rt, pdst, dst)) return ACMERR_NOTPOSSIBLE;
  bool ok = !pfltr && is_adpcm(src) && audio::decodable(src) && dst.tag == audio::kTagPcm && dst.bits == 16 &&
            dst.channels == src.channels && dst.rate == src.rate && !(flags & OPENF_ASYNC);
  trace("sound", "acmStreamOpen(tag 0x%X %u Hz -> tag 0x%X %u Hz %u-bit%s): %s", src.tag, src.rate, dst.tag, dst.rate,
        dst.bits, (flags & OPENF_QUERY) ? ", query" : "", ok ? "ok" : "not possible");
  if (!ok) return ACMERR_NOTPOSSIBLE;
  if (flags & OPENF_QUERY) return kOk;
  if (!phas) return kInvalidParam;
  AcmState& s = as(rt);
  uint32_t h = s.next++;
  s.streams[h] = Stream{src, dst};
  c.mem().write_u32l(phas, h);
  return kOk;
}

Stream* stream_of(Runtime& rt, uint32_t h) {
  AcmState& s = as(rt);
  auto it = s.streams.find(h);
  return it == s.streams.end() ? nullptr : &it->second;
}

uint32_t stream_size(Call& c) {
  // (has, cbInput, pdwOutputBytes, fdwSize)
  Stream* st = stream_of(c.rt, c.arg(0));
  if (!st) return kInvalidHandle;
  if (!c.arg(2)) return kInvalidParam;
  uint32_t how = c.arg(3) & SIZEF_QUERYMASK;
  size_t n;
  if (how == SIZEF_SOURCE) n = audio::decoded_size(st->src, c.arg(1));
  else if (how == SIZEF_DESTINATION) n = audio::encoded_size_for(st->src, c.arg(1));
  else return kInvalidParam;
  n = std::min<size_t>(n, 0xFFFFFFFFu);
  c.mem().write_u32l(c.arg(2), uint32_t(n));
  return n || !c.arg(1) ? kOk : ACMERR_NOTPOSSIBLE;
}

// ACMSTREAMHEADER checks shared by Prepare/Unprepare/Convert.
uint32_t header_status(Call& c, uint32_t* status) {
  if (!stream_of(c.rt, c.arg(0))) return kInvalidHandle;
  uint32_t h = c.arg(1);
  if (!h || c.mem().read_u32l(h) < kStreamHeaderSize) return kInvalidParam;
  *status = c.mem().read_u32l(h + 4);
  return kOk;
}

uint32_t stream_convert(Call& c) {
  // (has, pash, fdwConvert)
  uint32_t status = 0;
  if (uint32_t e = header_status(c, &status)) return e;
  if (!(status & STATUSF_PREPARED)) return ACMERR_UNPREPARED;
  if (status & STATUSF_INQUEUE) return ACMERR_BUSY;
  Stream& st = *stream_of(c.rt, c.arg(0));
  auto& mem = c.mem();
  uint32_t h = c.arg(1);
  uint32_t src = mem.read_u32l(h + 0x0C), src_len = mem.read_u32l(h + 0x10);
  uint32_t dst = mem.read_u32l(h + 0x1C), dst_len = mem.read_u32l(h + 0x20);
  // Whole blocks, plus the partial last block unless the caller asked for
  // block-aligned conversion (the decoder decides what a partial block holds:
  // msadp32 decodes it, imaadp32 nothing); only whole blocks when the output
  // would not hold the rest.
  uint32_t align = std::max<uint32_t>(st.src.block_align, 1);
  uint32_t whole = src_len / align, rest = src_len % align;
  size_t per_block = std::max<size_t>(audio::decoded_size(st.src, align), 1);
  uint32_t take = whole * align + ((c.arg(2) & CONVERTF_BLOCKALIGN) ? 0 : rest);
  if (audio::decoded_size(st.src, take) > dst_len) {
    whole = uint32_t(std::min<size_t>(whole, dst_len / per_block));
    take = whole * align;
  }
  uint32_t wrote = 0, used = 0;
  if (take) {
    std::span<const uint8_t> in(mem.at<uint8_t>(src, take), take);
    std::vector<uint8_t> pcm = audio::decode(st.src, in);
    wrote = uint32_t(std::min<size_t>(pcm.size(), dst_len));
    if (wrote) mem.memcpy(dst, pcm.data(), wrote);
    // The source consumed: the whole blocks, and the partial one if it produced samples.
    used = pcm.size() > size_t(whole) * per_block ? take : whole * align;
  }
  mem.write_u32l(h + 0x14, used);
  mem.write_u32l(h + 0x24, wrote);
  mem.write_u32l(h + 4, (status | STATUSF_DONE) & ~STATUSF_INQUEUE);
  trace("sound", "acmStreamConvert(0x%X): %u of %u source bytes -> %u PCM bytes", c.arg(0), used, src_len, wrote);
  return kOk;
}

}  // namespace

void register_msacm32(ShimRegistry& r) {
  // Registered with the pump-free handlers: MSACM32 is not WINMM.
  auto impl = [&r](const char* name, uint32_t (*fn)(Call&)) {
    r.impl(A, name, [fn](Call& c) {
      if (!audio_enabled(c.rt)) return c.ret(kNoDriver);
      c.ret(fn(c));
    });
  };
  impl("acmMetrics", [](Call& c) -> uint32_t {
    // (hao, uMetric, pMetric)
    if (!c.arg(2)) return kInvalidParam;
    uint32_t v;
    switch (c.arg(1)) {
      case ACM_METRIC_MAX_SIZE_FORMAT: v = 50; break;  // MS-ADPCM: 18 + 32 (§2.2)
      case ACM_METRIC_COUNT_DRIVERS:
      case ACM_METRIC_COUNT_CODECS: v = 2; break;      // IMA-ADPCM and MS-ADPCM
      default: return kNotSupported;
    }
    c.mem().write_u32l(c.arg(2), v);
    return kOk;
  });
  impl("acmFormatEnumA", format_enum);
  impl("acmFormatSuggest", format_suggest);
  impl("acmStreamOpen", stream_open);
  impl("acmStreamSize", stream_size);
  impl("acmStreamPrepareHeader", [](Call& c) -> uint32_t {
    uint32_t status = 0;
    if (uint32_t e = header_status(c, &status)) return e;
    c.mem().write_u32l(c.arg(1) + 4, status | STATUSF_PREPARED);
    return kOk;
  });
  impl("acmStreamUnprepareHeader", [](Call& c) -> uint32_t {
    uint32_t status = 0;
    if (uint32_t e = header_status(c, &status)) return e;
    if (!(status & STATUSF_PREPARED)) return ACMERR_UNPREPARED;
    if (status & STATUSF_INQUEUE) return ACMERR_BUSY;
    c.mem().write_u32l(c.arg(1) + 4, status & ~STATUSF_PREPARED);
    return kOk;
  });
  impl("acmStreamConvert", stream_convert);
  impl("acmStreamClose", [](Call& c) -> uint32_t {
    AcmState& s = as(c.rt);
    return s.streams.erase(c.arg(0)) ? kOk : kInvalidHandle;
  });
}

}  // namespace adw::win32
