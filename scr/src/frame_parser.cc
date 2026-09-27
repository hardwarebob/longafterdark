#include "frame_parser.h"

#include <cstring>

namespace adw::scr {

namespace {

// A header is believed only if it describes a screen a host could really be
// streaming. The saver asks for at most 2x the 4:3 width at Scale 4.0
// (5120x1920, 9.8 Mpx); anything past these bounds is garbage that happens
// to start with "P8"/"P6". Believing it would stall the stream: the parser
// would wait for a body of hundreds of megabytes while the lockstep host
// waits for a GO that only a delivered frame releases. Rejected, it costs
// one resync.
constexpr int kMaxDim = 8192;                        // per axis
constexpr size_t kMaxPixels = size_t(4096) * 4096;   // per frame (16.7 Mpx; P6 body <= 48 MiB)
constexpr size_t kMaxHeader = 4096;                  // longer than this is not a header

bool is_delim(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '#'; }

struct Cursor {
  const uint8_t* p;
  size_t n;
  size_t i = 0;

  // Skips whitespace and '#'-to-end-of-line comments. False if the buffer
  // ends first (the header is not complete yet).
  bool skip_ws() {
    while (i < n) {
      uint8_t c = p[i];
      if (c == '#') {
        while (i < n && p[i] != '\n') ++i;
      } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++i;
      } else {
        return true;
      }
    }
    return false;
  }

  // A token needs its trailing delimiter to be buffered, otherwise "64" might
  // still become "640".
  bool token(size_t& start, size_t& len) {
    if (!skip_ws()) return false;
    start = i;
    while (i < n && !is_delim(p[i])) ++i;
    if (i >= n) return false;
    len = i - start;
    return true;
  }
};

// Strict decimal: digits only (never a sign, since the value must be
// positive).
bool parse_dim(const uint8_t* s, size_t len, int& out) {
  if (len == 0 || len > 9) return false;
  int v = 0;
  for (size_t k = 0; k < len; ++k) {
    if (s[k] < '0' || s[k] > '9') return false;
    v = v * 10 + (s[k] - '0');
  }
  out = v;
  return true;
}

} // namespace

HeaderResult parse_frame_header(const uint8_t* p, size_t n, FrameHeader& out) {
  Cursor c{p, n};
  size_t s, len;
  auto need_more = [&] { return n > kMaxHeader ? HeaderResult::invalid : HeaderResult::need_more; };

  if (!c.token(s, len)) return need_more();
  if (len != 2 || p[s] != 'P' || (p[s + 1] != '6' && p[s + 1] != '8')) return HeaderResult::invalid;
  int format = p[s + 1] == '8' ? 8 : 6;

  int w = 0, h = 0;
  if (!c.token(s, len)) return need_more();
  if (!parse_dim(p + s, len, w) || w <= 0 || w > kMaxDim) return HeaderResult::invalid;
  if (!c.token(s, len)) return need_more();
  if (!parse_dim(p + s, len, h) || h <= 0 || h > kMaxDim) return HeaderResult::invalid;
  // Judged as soon as both numbers are in, before waiting for any more bytes.
  if ((size_t)w * (size_t)h > kMaxPixels) return HeaderResult::invalid;
  if (format == 6) {
    int maxv = 0;
    if (!c.token(s, len)) return need_more();
    if (!parse_dim(p + s, len, maxv) || maxv <= 0 || maxv >= 256) return HeaderResult::invalid;
  }
  out.format = format;
  out.width = w;
  out.height = h;
  out.body_start = c.i + 1;     // exactly one delimiter byte, then the body
  return HeaderResult::ok;
}

size_t frame_body_size(const FrameHeader& h) {
  size_t px = (size_t)h.width * (size_t)h.height;
  return h.format == 8 ? 768 + px : px * 3;
}

void FrameParser::feed(const void* data, size_t n) {
  if (n == 0) return;
  compact();
  const uint8_t* b = static_cast<const uint8_t*>(data);
  buf_.insert(buf_.end(), b, b + n);
}

bool FrameParser::take(RawFrame& out) {
  for (;;) {
    const uint8_t* p = buf_.data() + head_;
    size_t n = buf_.size() - head_;
    if (n == 0) return false;
    FrameHeader h;
    HeaderResult r = parse_frame_header(p, n, h);
    if (r == HeaderResult::need_more) return false;
    if (r == HeaderResult::invalid) {
      resync();
      continue;
    }
    size_t body = frame_body_size(h);
    if (n < h.body_start + body) return false;
    const uint8_t* b = p + h.body_start;
    out.width = h.width;
    out.height = h.height;
    out.format = h.format;
    if (h.format == 8) {
      out.palette.assign(b, b + 768);
      out.pixels.assign(b + 768, b + body);
    } else {
      out.palette.clear();
      out.pixels.assign(b, b + body);
    }
    head_ += h.body_start + body;
    compact();
    return true;
  }
}

// Drop bytes up to the next plausible magic ("P6"/"P8", or a trailing 'P'
// whose digit hasn't arrived). Always advances at least one byte.
void FrameParser::resync() {
  ++resyncs_;
  size_t i = head_ + 1;
  for (; i < buf_.size(); ++i) {
    if (buf_[i] != 'P') continue;
    if (i + 1 == buf_.size() || buf_[i + 1] == '6' || buf_[i + 1] == '8') break;
  }
  head_ = i;
  compact();
}

void FrameParser::compact() {
  if (head_ == 0) return;
  if (head_ >= buf_.size()) {
    buf_.clear();
    head_ = 0;
  } else if (head_ >= (1u << 20) || head_ * 2 >= buf_.size()) {
    buf_.erase(buf_.begin(), buf_.begin() + (ptrdiff_t)head_);
    head_ = 0;
  }
}

} // namespace adw::scr
