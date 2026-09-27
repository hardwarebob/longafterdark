// adhostwin --test-pattern: a synthetic lane that exercises the whole host
// protocol before any emulated module exists, so front-ends can be built and
// tested against a real adhostwin.
//
// Every pixel is a pure function of (frame number, seed, input received so
// far), never of the clock, so the frame stream is identical run to run —
// headless or streamed, whatever the machine's speed. What each part proves:
//   rainbow bands         palette cycling: the band indices never change, only
//                         the palette entries 10..137 rotate (control 0 = speed)
//   bouncing box/circle   index-plane animation
//   corner markers/border orientation and cropping (red TL, green TR, blue BL,
//                         yellow BR — a flipped or mirrored image is obvious)
//   FRAME counter         dropped/duplicated frames
//   control bars          SET / ADCVSET reach the lane (controls 0..7, 0..100)
//   status line + cursor  KEY / CAPS / MOUSE reach the lane
//
// ADTESTINTERACTIVE=1 makes it a stand-in game for the interaction protocol
// (INTERACTION.md §3.4): CAPS 1 sets interactive (CAPS 0 clears it), and
// while interactive every KEY and MOUSE line is consumed (status().eaten).
// ADTESTINTERACTIVE=cursor also raises the cursor flag while interactive.
//
// ADTESTAUDIO=1, with sound on (ADSOUND=1 or ADAUDIOOUT), drives the audio
// engine without a module (AUDIO.md §6.8): a 100 ms 440 Hz PCM16 blip at every
// whole second of virtual time, and a MIDI note (C4, 200 ms, from a one-note
// Standard MIDI File) at every second second. Each is stamped with its exact
// time, so the capture has them at n s whatever the frame rate. The pixels do
// not change.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "adw/core/audio.h"
#include "adw/core/lane.h"
#include "adw/core/log.h"
#include "adw/core/rng.h"
#include "font5x7.h"

namespace adw {
namespace {

constexpr int kCycleBase = 10, kCycleCount = 128;  // 10..137 rotate
constexpr int kGrayBase = 138, kGrayCount = 32;    // 138..169 static ramp
// Static system colours (see kStaticColors).
constexpr uint8_t kBlack = 0, kSilver = 7, kGray = 248, kRed = 249, kGreen = 250, kYellow = 251,
                  kBlue = 252, kWhite = 255;
constexpr int kControlsShown = 8;

// Triangle wave: bounces t across [0, range].
int bounce(int64_t t, int range) {
  if (range <= 0) return 0;
  int64_t period = 2 * int64_t(range);
  int64_t m = t % period;
  if (m < 0) m += period;
  return int(m <= range ? m : period - m);
}

// Hue wheel position (0..127) -> RGB, integer only (bit-exact everywhere).
void hue(int pos, uint8_t& r, uint8_t& g, uint8_t& b) {
  int h = (pos & 127) * 1536 / 128;  // 6 segments of 256
  int f = h & 255;
  switch (h >> 8) {
    case 0: r = 255; g = uint8_t(f); b = 0; break;
    case 1: r = uint8_t(255 - f); g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = uint8_t(f); break;
    case 3: r = 0; g = uint8_t(255 - f); b = 255; break;
    case 4: r = uint8_t(f); g = 0; b = 255; break;
    default: r = 255; g = 0; b = uint8_t(255 - f); break;
  }
}

class TestPatternLane : public Lane {
 public:
  const char* name() const override { return "test-pattern"; }

  bool init(const std::string&, LaneContext& ctx) override {
    ctx_ = &ctx;
    Screen& s = ctx.screen;
    w_ = s.width();
    h_ = s.height();
    s.reset_system_palette();
    for (int i = 0; i < kGrayCount; i++) {
      uint8_t v = uint8_t(i * 255 / (kGrayCount - 1));
      s.set_entry(kGrayBase + i, v, v, v);
    }
    // Star field from the run seed (ADSEED changes it; nothing else does).
    Rng rng = Rng::derive(ctx.env.seed, 0x53544152 /* 'STAR' */);
    int n = std::clamp(w_ * h_ / 2000, 8, 400);
    for (int i = 0; i < n; i++) stars_.push_back({int(rng.below(uint32_t(w_))), int(rng.below(uint32_t(h_)))});
    big_ = std::max(1, w_ / 200);
    small_ = std::max(1, w_ / 400);
    if (const std::string* v = ctx.env.get("ADTESTINTERACTIVE")) {
      interactive_mode_ = env_truthy(*v);
      cursor_mode_ = *v == "cursor";
    }
    if (ctx.env.flag("ADTESTAUDIO") && ctx.audio && ctx.audio->enabled()) start_audio(*ctx.audio);
    log("test pattern %dx%d, seed %" PRIu64 ", %zu stars", w_, h_, ctx.env.seed, stars_.size());
    return true;
  }

  uint32_t frame_interval_us() const override { return 33333; }  // 30 fps

  void on_command(const Command& c) override {
    if (!interactive_mode_) return;
    if (c.kind == Command::Kind::caps) {
      interactive_ = c.a != 0;
    } else if ((c.kind == Command::Kind::key || c.kind == Command::Kind::mouse) && interactive_ && c.seq) {
      eaten_ = std::max(eaten_, c.seq);
    }
  }

  LaneStatus status() const override {
    LaneStatus st;
    st.interactive = interactive_;
    st.cursor = interactive_ && cursor_mode_;
    st.source = interactive_ ? kStatusSourceAd4 : kStatusSourceNone;
    st.eaten = eaten_;
    return st;
  }

  StepResult step() override {
    Screen& s = ctx_->screen;
    const InputState& in = ctx_->input;
    if (audio_) sound(ctx_->clock.now_us());

    // Palette cycling. The phase is accumulated (not frame * speed) so a SET
    // mid-run changes the speed from that frame on without a jump — and it
    // advances before drawing, so the frame right after a SET already shows it.
    if (frame_ > 0) phase_ = (phase_ + 1 + std::clamp<int32_t>(in.control(0, 0), 0, 15)) & (kCycleCount - 1);
    for (int i = 0; i < kCycleCount; i++) {
      uint8_t r, g, b;
      hue(i + phase_, r, g, b);
      // Dimmed so the white readouts and saturated shapes stand out.
      s.set_entry(kCycleBase + i, uint8_t(r * 5 / 8), uint8_t(g * 5 / 8), uint8_t(b * 5 / 8));
    }

    // Diagonal bands over the cycling range.
    for (int y = 0; y < h_; y++) {
      uint8_t* row = s.row(y);
      for (int x = 0; x < w_; x++) row[x] = uint8_t(kCycleBase + (((x + 2 * y) >> 2) & (kCycleCount - 1)));
    }
    for (size_t i = 0; i < stars_.size(); i++) {
      int level = int((i * 7 + frame_) & (kGrayCount - 1));
      put(stars_[i].x, stars_[i].y, uint8_t(kGrayBase + level));
    }

    // Moving shapes.
    int bw = std::max(4, w_ / 8), bh = std::max(4, h_ / 8);
    int bx = bounce(frame_ * 3, w_ - bw), by = bounce(frame_ * 2, h_ - bh);
    fill(bx, by, bw, bh, kWhite);
    fill(bx + 1, by + 1, bw - 2, bh - 2, kRed);
    int r = std::max(3, std::min(w_, h_) / 10);
    int cx = r + bounce(frame_ * 2 + w_ / 3, w_ - 2 * r);
    int cy = r + bounce(frame_ * 3 + h_ / 5, h_ - 2 * r);
    for (int dy = -r; dy <= r; dy++)
      for (int dx = -r; dx <= r; dx++) {
        int d2 = dx * dx + dy * dy;
        if (d2 <= r * r) put(cx + dx, cy + dy, d2 >= (r - 2) * (r - 2) ? kWhite : kBlue);
      }
    int scan = int(frame_ * 4 % uint64_t(w_));
    fill(scan, 0, 2, h_, kGreen);

    // Frame edges: 1px border, orientation markers in the corners.
    fill(0, 0, w_, 1, kWhite);
    fill(0, h_ - 1, w_, 1, kWhite);
    fill(0, 0, 1, h_, kWhite);
    fill(w_ - 1, 0, 1, h_, kWhite);
    int m = std::max(4, std::min(w_, h_) / 40);
    fill(0, 0, m, m, kRed);
    fill(w_ - m, 0, m, m, kGreen);
    fill(0, h_ - m, m, m, kBlue);
    fill(w_ - m, h_ - m, m, m, kYellow);

    // Readouts.
    char buf[96];
    int x0 = m + 4, y = m + 4;
    snprintf(buf, sizeof(buf), "FRAME %06" PRIu64, frame_);
    y += label(x0, y, buf, big_, kWhite);
    snprintf(buf, sizeof(buf), "TEST PATTERN %dX%d SEED %" PRIu64, w_, h_, ctx_->env.seed);
    y += label(x0, y, buf, small_, kSilver);
    if (in.last_key >= 0)
      snprintf(buf, sizeof(buf), "KEY %d %s  CAPS %d", in.last_key, in.last_key_down ? "DN" : "UP", in.caps ? 1 : 0);
    else
      snprintf(buf, sizeof(buf), "KEY -  CAPS %d", in.caps ? 1 : 0);
    y += label(x0, y, buf, small_, kSilver);
    if (interactive_) y += label(x0, y, "INTERACTIVE", small_, kYellow);
    if (in.mouse_seen) {
      snprintf(buf, sizeof(buf), "MOUSE %d,%d %s%s%s", in.mouse_x, in.mouse_y, in.mouse_button ? "DN" : "UP",
               (in.mouse_buttons & kMouseRight) ? " R" : "", (in.mouse_buttons & kMouseMiddle) ? " M" : "");
      label(x0, y, buf, small_, kSilver);
    }

    // Control bars, bottom left: "C<i>" + bar (0..100 of a third of the width) + value.
    int line_h = 8 * small_ + 4;
    int bar_max = std::max(10, w_ / 3);
    int yb = h_ - m - 4 - kControlsShown * line_h;
    for (int i = 0; i < kControlsShown; i++, yb += line_h) {
      if (yb < y) continue;  // tiny screens: never draw over the readouts
      int32_t v = in.control(i, 0);
      int len = int(int64_t(std::clamp<int32_t>(v, 0, 100)) * bar_max / 100);
      snprintf(buf, sizeof(buf), "C%d", i);
      int lx = x0;
      lx += label(lx, yb, buf, small_, kWhite, /*advance_x=*/true);
      fill(lx, yb, bar_max + 2, 8 * small_ + 2, kBlack);
      fill(lx + 1, yb + 1, len, 8 * small_, in.controls.count(i) ? kGreen : kGray);
      snprintf(buf, sizeof(buf), "%d", v);
      label(lx + bar_max + 4, yb, buf, small_, kWhite);
    }

    // Mouse cursor on top of everything.
    if (in.mouse_seen) {
      // MOUSE carries any int32; clamp to just past the edges first so the
      // offsets below cannot overflow (a far-off pointer draws nothing, as before).
      int mx = std::clamp<int32_t>(in.mouse_x, -16, w_ + 16);
      int my = std::clamp<int32_t>(in.mouse_y, -16, h_ + 16);
      for (int d = -6; d <= 6; d++) {
        put(mx + d, my - 1, kBlack);
        put(mx + d, my + 1, kBlack);
        put(mx - 1, my + d, kBlack);
        put(mx + 1, my + d, kBlack);
      }
      for (int d = -6; d <= 6; d++) {
        put(mx + d, my, kWhite);
        put(mx, my + d, kWhite);
      }
      if (in.mouse_button) fill(mx - 3, my - 3, 7, 7, kYellow);
    }

    s.mark_dirty();
    frame_++;
    return StepResult::ok;
  }

 private:
  struct Pt {
    int x, y;
  };

  void put(int x, int y, uint8_t c) {
    if (x >= 0 && y >= 0 && x < w_ && y < h_) ctx_->screen.row(y)[x] = c;
  }

  void fill(int x, int y, int w, int h, uint8_t c) {
    int x1 = std::min(w_, x + w), y1 = std::min(h_, y + h);
    x = std::max(0, x);
    y = std::max(0, y);
    for (int yy = y; yy < y1; yy++) {
      uint8_t* row = ctx_->screen.row(yy);
      for (int xx = x; xx < x1; xx++) row[xx] = c;
    }
  }

  // Text on a black plate. Returns the height consumed (or the width, when
  // advance_x), plate included.
  int label(int x, int y, const char* text, int scale, uint8_t color, bool advance_x = false) {
    int n = int(strlen(text));
    int tw = n * 6 * scale - scale, th = 7 * scale;
    fill(x - 2, y - 2, tw + 4, th + 4, kBlack);
    for (int i = 0; i < n; i++) {
      const uint8_t* g = font5x7::glyph(text[i]);
      for (int row = 0; row < font5x7::kH; row++)
        for (int col = 0; col < font5x7::kW; col++)
          if (g[row] & (0x10 >> col)) fill(x + (i * 6 + col) * scale, y + row * scale, scale, scale, color);
    }
    return advance_x ? tw + 4 * scale : th + 4 + scale;
  }

  // ---- ADTESTAUDIO ----
  void start_audio(audio::Engine& e) {
    // 100 ms of 440 Hz at 22050 Hz (mono PCM16, -6 dBFS) from an integer
    // oscillator, y[n] = 2cos(w) y[n-1] - y[n-2] (Q30 coefficient, Q16
    // amplitude), so the samples are the same on every machine; 1 ms fades.
    constexpr int kRate = 22050, kFrames = 2205, kFade = 22;
    constexpr int64_t kTwoCos = 2130626707;  // round(2 cos(2 pi 440 / 22050) * 2^30)
    constexpr int64_t kSin1 = 134271977;     // round(16384 * 2^16 * sin(2 pi 440 / 22050))
    std::vector<uint8_t> pcm(size_t(kFrames) * 2);
    int64_t y2 = 0, y1 = kSin1;
    for (int n = 0; n < kFrames; n++) {
      int64_t y = n == 0 ? 0 : n == 1 ? kSin1 : ((kTwoCos * y1) >> 30) - y2;
      if (n >= 2) {
        y2 = y1;
        y1 = y;
      }
      int32_t v = int32_t(y >> 16);
      int ramp = std::min({n, kFrames - 1 - n, kFade});
      v = v * ramp / kFade;
      pcm[size_t(n) * 2] = uint8_t(v & 0xFF);
      pcm[size_t(n) * 2 + 1] = uint8_t((v >> 8) & 0xFF);
    }
    audio::WaveFormat f = audio::pcm_format(kRate, 1, 16);
    audio::BufferId b = e.create_buffer(f, uint32_t(pcm.size()));
    e.write_buffer(b, 0, pcm, 0);
    blip_ = e.create_voice(b, audio::Bus::wave);
    e.release_buffer(b);  // lives on with its voice
    // Format 0, 500 PPQN at the default tempo (1 tick = 1 ms): note-on C4 at 0,
    // note-off at 200 ms, end of track.
    static const uint8_t kNote[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xF4,
                                    'M', 'T', 'r', 'k', 0, 0, 0, 13,
                                    0x00, 0x90, 60, 100, 0x81, 0x48, 0x80, 60, 0, 0x00, 0xFF, 0x2F, 0x00};
    note_ = e.load_song(kNote);
    audio_ = &e;
    log("test pattern audio: a 440 Hz blip every second, a MIDI note every 2 s");
  }

  // Everything due at or before `now`, each at its exact whole second (never
  // earlier than what the engine has seen: the previous step's time).
  void sound(uint64_t now) {
    while (next_second_us_ <= now) {
      uint64_t t = next_second_us_;
      audio_->set_cursor(blip_, 0, t);
      audio_->play(blip_, false, t);
      if ((t / 1000000) % 2 == 0) {
        audio_->song_seek(note_, 0, t);
        audio_->song_play(note_, t);
      }
      next_second_us_ += 1000000;
    }
  }

  audio::Engine* audio_ = nullptr;
  audio::VoiceId blip_ = 0;
  audio::SongId note_ = 0;
  uint64_t next_second_us_ = 0;

  LaneContext* ctx_ = nullptr;
  bool interactive_mode_ = false, cursor_mode_ = false, interactive_ = false;
  uint64_t eaten_ = 0;
  int w_ = 0, h_ = 0, big_ = 1, small_ = 1;
  uint64_t frame_ = 0;
  int phase_ = 0;
  std::vector<Pt> stars_;
};

}  // namespace

std::unique_ptr<Lane> make_test_pattern_lane() { return std::make_unique<TestPatternLane>(); }

}  // namespace adw
