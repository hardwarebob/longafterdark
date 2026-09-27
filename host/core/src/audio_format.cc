// Formats, ADPCM decoders and gains of the audio engine (audio.h,
// docs/AUDIO.md §2.2, §5, §6.2).
//
// The decoders follow Windows' own codecs, which core.audio checks byte for
// byte against the host's msacm32 (EMPIRICAL on Windows 11, whose imaadp32 /
// msadp32 are the codecs Windows has shipped since the 1990s):
//   * imaadp32 decodes whole blocks only: a trailing partial block yields
//     nothing, with or without ACM_STREAMCONVERTF_END.
//   * msadp32 decodes a trailing partial block too, once it holds the 7-byte
//     per-channel header: the two header samples, then one per nibble.
//   * acmStreamSize(SOURCE) rounds the source up to whole blocks and refuses
//     less than one block; acmStreamSize(DESTINATION) rounds the destination
//     down to whole blocks' worth (decoded_size / encoded_size_for).
#include <algorithm>
#include <cstring>

#include "adw/core/audio.h"
#include "audio_internal.h"

namespace adw::audio {

namespace {

#include "audio_db_table.inc"

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
void wr16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}
void wr32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i));
}

// MS-ADPCM's seven standard predictor coefficient pairs (8.8 fixed point).
constexpr std::array<std::array<int16_t, 2>, 7> kMsCoefs = {{
    {256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}}};
constexpr int kMsAdapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};

constexpr int16_t kImaSteps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int kImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

int16_t clamp16(int32_t v) { return int16_t(std::clamp<int32_t>(v, -32768, 32767)); }

// Standard block alignment per channel: 256 bytes up to 11025 Hz, 512 at
// 22050, 1024 at 44100 (and above), as both codecs enumerate them.
uint16_t std_block(uint32_t rate) { return rate <= 11025 ? 256 : rate <= 22050 ? 512 : 1024; }

bool is_adpcm(const WaveFormat& f) { return f.tag == kTagImaAdpcm || f.tag == kTagMsAdpcm; }

// Samples per channel a whole block of `f` holds at most.
uint32_t max_block_samples(const WaveFormat& f) {
  uint32_t ch = f.channels;
  if (f.tag == kTagImaAdpcm) return f.block_align < 4 * ch ? 0 : (uint32_t(f.block_align) - 4 * ch) * 8 / (4 * ch) + 1;
  if (f.tag == kTagMsAdpcm) return f.block_align < 7 * ch ? 0 : (uint32_t(f.block_align) - 7 * ch) * 2 / ch + 2;
  return 0;
}

// ---- IMA ADPCM (imaadp32) ----
struct ImaState {
  int32_t sample = 0;
  int index = 0;
  int16_t step(uint8_t nib) {
    int32_t s = kImaSteps[index];
    int32_t diff = s >> 3;
    if (nib & 4) diff += s;
    if (nib & 2) diff += s >> 1;
    if (nib & 1) diff += s >> 2;
    sample = (nib & 8) ? sample - diff : sample + diff;
    sample = clamp16(sample);
    index = std::clamp(index + kImaIndex[nib], 0, 88);
    return int16_t(sample);
  }
};

// One whole IMA block -> spb frames of PCM16 at `out` (interleaved).
void ima_block(const WaveFormat& f, const uint8_t* in, int16_t* out) {
  const int ch = f.channels;
  const uint32_t spb = f.samples_per_block;
  ImaState st[2];
  for (int c = 0; c < ch; c++) {
    st[c].sample = int16_t(rd16(in + 4 * c));
    st[c].index = std::clamp<int>(in[4 * c + 2], 0, 88);
    out[c] = int16_t(st[c].sample);
  }
  const uint8_t* p = in + 4 * ch;
  if (ch == 1) {
    for (uint32_t i = 1; i < spb; i += 2) {
      uint8_t b = *p++;
      out[i] = st[0].step(b & 15);
      if (i + 1 < spb) out[i + 1] = st[0].step(b >> 4);
    }
  } else {
    // Groups of 4 bytes (8 samples) per channel, left then right.
    for (uint32_t i = 1; i < spb; i += 8) {
      for (int c = 0; c < 2; c++) {
        for (uint32_t k = 0; k < 8; k++) {
          uint8_t b = p[k / 2];
          uint8_t nib = (k & 1) ? (b >> 4) : (b & 15);
          int16_t s = st[c].step(nib);
          if (i + k < spb) out[(i + k) * 2 + c] = s;
        }
        p += 4;
      }
    }
  }
}

// ---- MS ADPCM (msadp32) ----
// msadp32's arithmetic is 32-bit two's complement: delta has no upper bound
// (a header's is any int16, and nibble 8 triples it each step), so products
// wrap after a few steps of a block no encoder makes, and extreme
// coefficients wrap the predictor sum. Done in uint32 here, where signed
// overflow would be undefined behaviour; the results are msadp32's, bit for
// bit (core.audio ms_adpcm_extreme_blocks_match_the_host_acm).
inline int32_t mul32(int32_t a, int32_t b) { return int32_t(uint32_t(a) * uint32_t(b)); }
inline int32_t add32(int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); }

struct MsState {
  int32_t c1 = 0, c2 = 0, delta = 0, s1 = 0, s2 = 0;
  int16_t step(uint8_t nib) {
    int32_t pred = add32(mul32(s1, c1), mul32(s2, c2)) >> 8;
    int32_t sn = nib & 8 ? int32_t(nib) - 16 : int32_t(nib);
    int32_t v = clamp16(add32(pred, mul32(sn, delta)));
    s2 = s1;
    s1 = v;
    delta = mul32(kMsAdapt[nib], delta) >> 8;
    if (delta < 16) delta = 16;
    return int16_t(v);
  }
};

// One MS block of `bytes` (a whole block, or a trailing partial one holding
// at least the header) -> frames written at `out`; 0 on a bad predictor.
uint32_t ms_block(const WaveFormat& f, const uint8_t* in, uint32_t bytes, int16_t* out) {
  const int ch = f.channels;
  const uint32_t spb = f.samples_per_block;
  MsState st[2];
  for (int c = 0; c < ch; c++) {
    uint8_t pred = in[c];
    if (pred >= f.coefs.size()) return 0;
    st[c].c1 = f.coefs[pred][0];
    st[c].c2 = f.coefs[pred][1];
    st[c].delta = int16_t(rd16(in + ch + 2 * c));
    st[c].s1 = int16_t(rd16(in + 3 * ch + 2 * c));
    st[c].s2 = int16_t(rd16(in + 5 * ch + 2 * c));
    out[c] = int16_t(st[c].s2);
    out[ch + c] = int16_t(st[c].s1);
  }
  uint32_t frames = 2;
  const uint8_t* p = in + 7 * ch;
  const uint8_t* end = in + bytes;
  if (ch == 1) {
    while (frames < spb && p < end) {
      uint8_t b = *p++;
      out[frames++] = st[0].step(b >> 4);
      if (frames < spb) out[frames++] = st[0].step(b & 15);
    }
  } else {
    while (frames < spb && p < end) {
      uint8_t b = *p++;
      out[frames * 2] = st[0].step(b >> 4);
      out[frames * 2 + 1] = st[1].step(b & 15);
      frames++;
    }
  }
  return frames;
}

}  // namespace

uint16_t db_table(int attenuation_mb) { return kDbTable[std::clamp(attenuation_mb, 0, 10000)]; }

// ---- formats ------------------------------------------------------------------

WaveFormat pcm_format(uint32_t rate, uint16_t channels, uint16_t bits) {
  WaveFormat f;
  f.tag = kTagPcm;
  f.channels = channels;
  f.rate = rate;
  f.bits = bits;
  f.block_align = uint16_t(channels * bits / 8);
  f.avg_bytes = rate * f.block_align;
  f.samples_per_block = 0;
  return f;
}

WaveFormat ima_adpcm_format(uint32_t rate, uint16_t channels) {
  WaveFormat f;
  f.tag = kTagImaAdpcm;
  f.channels = channels;
  f.rate = rate;
  f.bits = 4;
  f.block_align = uint16_t(std_block(rate) * channels);
  f.samples_per_block = uint16_t(max_block_samples(f));
  // As the codec reports it: rate * nBlockAlign / nSamplesPerBlock, rounded down.
  f.avg_bytes = uint32_t(uint64_t(rate) * f.block_align / f.samples_per_block);
  return f;
}

WaveFormat ms_adpcm_format(uint32_t rate, uint16_t channels) {
  WaveFormat f;
  f.tag = kTagMsAdpcm;
  f.channels = channels;
  f.rate = rate;
  f.bits = 4;
  f.block_align = uint16_t(std_block(rate) * channels);
  f.samples_per_block = uint16_t(max_block_samples(f));
  f.avg_bytes = uint32_t(uint64_t(rate) * f.block_align / f.samples_per_block);
  f.coefs.assign(kMsCoefs.begin(), kMsCoefs.end());
  return f;
}

bool parse_waveformat(std::span<const uint8_t> bytes, WaveFormat& out) {
  if (bytes.size() < 16) return false;
  const uint8_t* p = bytes.data();
  WaveFormat f;
  f.tag = rd16(p);
  f.channels = rd16(p + 2);
  f.rate = rd32(p + 4);
  f.avg_bytes = rd32(p + 8);
  f.block_align = rd16(p + 12);
  f.bits = rd16(p + 14);
  f.samples_per_block = 0;
  f.coefs.clear();
  if (f.tag == kTagImaAdpcm || f.tag == kTagMsAdpcm) {
    // WAVEFORMATEX with the codec's extra bytes.
    if (bytes.size() < 18) return false;
    uint16_t cb = rd16(p + 16);
    if (bytes.size() < size_t(18) + cb) return false;
    if (cb < 2) return false;
    f.samples_per_block = rd16(p + 18);
    if (f.tag == kTagMsAdpcm) {
      if (cb < 4) return false;
      uint16_t n = rd16(p + 20);
      if (n == 0 || size_t(4) + size_t(n) * 4 > cb) return false;
      f.coefs.resize(n);
      for (uint16_t i = 0; i < n; i++) {
        f.coefs[i][0] = int16_t(rd16(p + 22 + 4 * i));
        f.coefs[i][1] = int16_t(rd16(p + 24 + 4 * i));
      }
    }
  }
  // PCM (and any other tag): a PCMWAVEFORMAT's 16 bytes are the whole format;
  // what follows in guest memory is not trusted as a cbSize.
  out = std::move(f);
  return true;
}

std::vector<uint8_t> waveformat_bytes(const WaveFormat& f) {
  uint16_t cb = 0;
  if (f.tag == kTagImaAdpcm) cb = 2;
  if (f.tag == kTagMsAdpcm) cb = uint16_t(4 + 4 * f.coefs.size());
  std::vector<uint8_t> b(size_t(18) + cb, 0);
  wr16(&b[0], f.tag);
  wr16(&b[2], f.channels);
  wr32(&b[4], f.rate);
  wr32(&b[8], f.avg_bytes);
  wr16(&b[12], f.block_align);
  wr16(&b[14], f.bits);
  wr16(&b[16], cb);
  if (f.tag == kTagImaAdpcm) wr16(&b[18], f.samples_per_block);
  if (f.tag == kTagMsAdpcm) {
    wr16(&b[18], f.samples_per_block);
    wr16(&b[20], uint16_t(f.coefs.size()));
    for (size_t i = 0; i < f.coefs.size(); i++) {
      wr16(&b[22 + 4 * i], uint16_t(f.coefs[i][0]));
      wr16(&b[24 + 4 * i], uint16_t(f.coefs[i][1]));
    }
  }
  return b;
}

bool playable(const WaveFormat& f) {
  return f.tag == kTagPcm && (f.bits == 8 || f.bits == 16) && (f.channels == 1 || f.channels == 2) &&
         f.rate >= 1000 && f.rate <= 192000 && f.block_align == f.channels * f.bits / 8;
}

bool decodable(const WaveFormat& f) {
  if (playable(f)) return true;
  if (!is_adpcm(f) || f.bits != 4 || (f.channels != 1 && f.channels != 2)) return false;
  if (f.rate < 1000 || f.rate > 192000) return false;
  uint32_t max = max_block_samples(f);
  if (f.tag == kTagImaAdpcm) {
    // A block is the per-channel header plus whole 4-byte groups per channel.
    if (f.block_align < 4 * f.channels || (f.block_align % (4 * f.channels)) != 0) return false;
    return f.samples_per_block >= 1 && f.samples_per_block <= max;
  }
  if (f.block_align < 7 * f.channels || f.coefs.empty()) return false;
  return f.samples_per_block >= 2 && f.samples_per_block <= max;
}

size_t riff_extent(std::span<const uint8_t> first12) {
  if (first12.size() < 12) return 0;
  const uint8_t* p = first12.data();
  if (memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) return 0;
  return size_t(8) + rd32(p + 4);
}

bool parse_wave(std::span<const uint8_t> riff, Wave& out) {
  if (riff.size() < 12 || memcmp(riff.data(), "RIFF", 4) != 0 || memcmp(riff.data() + 8, "WAVE", 4) != 0)
    return false;
  // The chunks are walked to the end of the image: a RIFF size that is too
  // large is clipped by it, and one that is too small is not trusted.
  bool have_fmt = false, have_data = false;
  Wave w;
  size_t pos = 12;
  while (pos + 8 <= riff.size()) {
    const uint8_t* h = riff.data() + pos;
    uint32_t size = rd32(h + 4);
    size_t body = pos + 8;
    size_t avail = std::min<size_t>(size, riff.size() - body);
    std::span<const uint8_t> chunk = riff.subspan(body, avail);
    if (!have_fmt && memcmp(h, "fmt ", 4) == 0) {
      if (!parse_waveformat(chunk, w.format)) return false;
      have_fmt = true;
    } else if (!have_data && memcmp(h, "data", 4) == 0) {
      w.data = chunk;
      have_data = true;
    } else if (memcmp(h, "fact", 4) == 0 && avail >= 4) {
      w.fact_samples = rd32(chunk.data());
    }
    size_t next = body + size_t(size) + (size & 1);
    if (next <= pos) break;
    pos = next;
  }
  if (!have_fmt || !have_data) return false;
  out = std::move(w);
  return true;
}

// ---- decoding -----------------------------------------------------------------

WaveFormat decoded_format(const WaveFormat& src) {
  if (src.tag == kTagPcm) return src;
  return pcm_format(src.rate, src.channels, 16);
}

size_t decoded_size(const WaveFormat& src, size_t src_bytes) {
  if (playable(src)) return src_bytes;
  if (!decodable(src)) return 0;
  // acmStreamSize(ACM_STREAMSIZEF_SOURCE): whole blocks, rounded up; less
  // than one block is ACMERR_NOTPOSSIBLE (0 here).
  if (src_bytes < src.block_align) return 0;
  size_t blocks = (src_bytes + src.block_align - 1) / src.block_align;
  return blocks * src.samples_per_block * src.channels * 2;
}

size_t encoded_size_for(const WaveFormat& src, size_t pcm_bytes) {
  if (playable(src)) return pcm_bytes;
  if (!decodable(src)) return 0;
  // acmStreamSize(ACM_STREAMSIZEF_DESTINATION): the whole blocks whose
  // decoding fits in pcm_bytes (rounded down; 0 = not even one).
  size_t per_block = size_t(src.samples_per_block) * src.channels * 2;
  return (pcm_bytes / per_block) * src.block_align;
}

size_t decode(const WaveFormat& src, std::span<const uint8_t> in, std::span<uint8_t> out) {
  if (playable(src)) {
    size_t n = std::min(in.size(), out.size());
    n -= n % src.block_align;
    memcpy(out.data(), in.data(), n);
    return n;
  }
  if (!decodable(src)) return 0;
  const size_t ba = src.block_align, ch = src.channels;
  const size_t block_out = size_t(src.samples_per_block) * ch * 2;
  std::vector<int16_t> tmp(size_t(src.samples_per_block) * ch);
  size_t written = 0, pos = 0;
  while (pos + ba <= in.size()) {
    if (written + block_out > out.size()) return written;
    if (src.tag == kTagImaAdpcm) {
      ima_block(src, in.data() + pos, tmp.data());
    } else if (ms_block(src, in.data() + pos, uint32_t(ba), tmp.data()) != src.samples_per_block) {
      return written;
    }
    memcpy(out.data() + written, tmp.data(), block_out);
    written += block_out;
    pos += ba;
  }
  // A trailing partial block: msadp32 decodes what it holds; imaadp32 nothing.
  size_t rest = in.size() - pos;
  if (src.tag == kTagMsAdpcm && rest >= 7 * ch) {
    uint32_t frames = ms_block(src, in.data() + pos, uint32_t(rest), tmp.data());
    size_t bytes = std::min(size_t(frames) * ch * 2, out.size() - written);
    bytes -= bytes % (ch * 2);
    memcpy(out.data() + written, tmp.data(), bytes);
    written += bytes;
  }
  return written;
}

std::vector<uint8_t> decode(const WaveFormat& src, std::span<const uint8_t> in) {
  if (playable(src)) {
    size_t n = in.size() - in.size() % src.block_align;
    return std::vector<uint8_t>(in.begin(), in.begin() + ptrdiff_t(n));
  }
  if (!decodable(src)) return {};
  size_t blocks = in.size() / src.block_align + 1;
  std::vector<uint8_t> out(blocks * src.samples_per_block * src.channels * 2);
  out.resize(decode(src, in, std::span<uint8_t>(out)));
  return out;
}

// ---- gains --------------------------------------------------------------------

Gain gain_from_ds(int32_t volume, int32_t pan) {
  volume = std::clamp<int32_t>(volume, -10000, 0);
  pan = std::clamp<int32_t>(pan, -10000, 10000);
  int32_t left = volume - std::max<int32_t>(pan, 0);   // pan right attenuates the left
  int32_t right = volume + std::min<int32_t>(pan, 0);  // pan left attenuates the right
  return Gain{db_table(-left), db_table(-right)};
}

namespace {
uint16_t q15_from_word(uint32_t w) { return uint16_t((uint64_t(w & 0xFFFF) * 65536 + 65535) / 131070); }
uint16_t word_from_q15(uint16_t g) {
  uint64_t v = (uint64_t(g) * 131070 + 32768) / 65536;
  return uint16_t(std::min<uint64_t>(v, 0xFFFF));
}
}  // namespace

Gain gain_from_mm(uint32_t volume) { return Gain{q15_from_word(volume), q15_from_word(volume >> 16)}; }

uint32_t mm_from_gain(Gain g) { return uint32_t(word_from_q15(g.left)) | (uint32_t(word_from_q15(g.right)) << 16); }

}  // namespace adw::audio
