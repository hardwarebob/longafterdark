// The host status record (INTERACTION.md §3.4): 64 bytes at offset 0 of a
// pagefile-backed section the front-end creates per host and passes as an
// inherited handle in ADSTATUSHANDLE. adhostwin writes it after lane init and
// after every completed step; the .scr reads it to decide whether an input
// line woke the saver or belongs to the module (§4.3).
//
// This header is shared by both sides. The record layout, the flag bits, the
// seqlock writer primitive and the reader are inline, so the .scr (which does
// not link adw_core) includes this file for read_status() alone.
// StatusPublisher is adhostwin's side and lives in adw_core (status.cc).
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace adw {

struct Env;
struct LaneStatus;  // lane.h

inline constexpr uint32_t kStatusMagic = 0x53574441;  // "ADWS" little-endian
inline constexpr uint16_t kStatusVersion = 1;
inline constexpr uint32_t kStatusSectionBytes = 4096;  // what a front-end creates (at least)

// flags
inline constexpr uint32_t ADWS_INTERACTIVE = 0x01;  // the module takes keys, clicks and moves as its own
inline constexpr uint32_t ADWS_CURSOR = 0x02;       // show a cursor (AD4 +0x08 bit 1; AD3 0x11 until 0x12)
inline constexpr uint32_t ADWS_ROTATE_OK = 0x04;    // may be rotated away while interactive (AD4 +0x08 bit 2)
inline constexpr uint32_t ADWS_KEY_FILTER = 0x08;   // the guest may consume input without being interactive
inline constexpr uint32_t ADWS_WAKE = 0x10;         // the guest asked the saver window to close
inline constexpr uint32_t ADWS_READY = 0x20;        // lane init done

// source
inline constexpr uint32_t kStatusSourceNone = 0, kStatusSourceAd4 = 1, kStatusSourceAd3 = 2;
// lane
inline constexpr uint32_t kStatusLanePe32 = 1, kStatusLaneNe16 = 2, kStatusLaneTest = 3;

struct AdwHostStatusV1 {       // 64 bytes, little-endian
  uint32_t magic;              // kStatusMagic
  uint16_t version;            // 1
  uint16_t size;               // 64
  volatile uint32_t gen;       // seqlock: odd while the host writes
  uint32_t flags;              // ADWS_*
  uint64_t frames;             // steps completed
  uint64_t input_applied;      // seq of the last input line applied before the last completed step
  uint64_t input_eaten;        // highest input seq the guest consumed
  uint32_t source;             // why interactive: 0 none, 1 AD4 WantEvents, 2 AD3 0x0E
  uint32_t lane;               // 1 pe32, 2 ne16, 3 test pattern
  uint32_t reserved[4];        // 0
};
static_assert(sizeof(AdwHostStatusV1) == 64, "the status record is 64 bytes");
static_assert(offsetof(AdwHostStatusV1, gen) == 8);
static_assert(offsetof(AdwHostStatusV1, frames) == 16);
static_assert(offsetof(AdwHostStatusV1, input_eaten) == 32);
static_assert(offsetof(AdwHostStatusV1, lane) == 44);

// Seqlock writer (one writer per record): gen goes odd, the fields are stored,
// a full barrier, gen goes even. `view` is the mapped section.
inline void write_status(void* view, const AdwHostStatusV1& value) {
  auto* rec = static_cast<AdwHostStatusV1*>(view);
  uint32_t g = rec->gen;
  if (g & 1) g++;  // never start from a torn (odd) value left by a dead writer
  rec->gen = g + 1;
  MemoryBarrier();
  rec->magic = kStatusMagic;
  rec->version = kStatusVersion;
  rec->size = uint16_t(sizeof(AdwHostStatusV1));
  rec->flags = value.flags;
  rec->frames = value.frames;
  rec->input_applied = value.input_applied;
  rec->input_eaten = value.input_eaten;
  rec->source = value.source;
  rec->lane = value.lane;
  for (uint32_t& r : rec->reserved) r = 0;
  MemoryBarrier();
  rec->gen = g + 2;
}

// Seqlock reader: read gen; odd = a write is in progress, retry; copy; re-read
// gen; equal = consistent. Gives up after 4 tries (the caller keeps its
// previous copy). False too for a record nobody has written yet (no magic).
inline bool read_status(const void* view, AdwHostStatusV1* out) {
  if (!view || !out) return false;
  const auto* rec = static_cast<const AdwHostStatusV1*>(view);
  for (int attempt = 0; attempt < 4; attempt++) {
    uint32_t g1 = rec->gen;
    if (g1 & 1) {
      YieldProcessor();
      continue;
    }
    MemoryBarrier();
    AdwHostStatusV1 copy;
    memcpy(&copy, const_cast<const AdwHostStatusV1*>(rec), sizeof(copy));
    MemoryBarrier();
    uint32_t g2 = rec->gen;
    if (g1 != g2) continue;
    if (copy.magic != kStatusMagic || copy.version != kStatusVersion) return false;
    copy.gen = g1;
    *out = copy;
    return true;
  }
  return false;
}

// The flag word for a LaneStatus plus the host's own READY bit (status.cc).
uint32_t status_flags(const LaneStatus& s, bool ready);

// adhostwin's writer: maps ADSTATUSHANDLE and mirrors changes on stderr when
// ADSTATUSLOG=1. Every call is a no-op when neither is configured.
class StatusPublisher {
 public:
  StatusPublisher() = default;
  ~StatusPublisher();
  StatusPublisher(const StatusPublisher&) = delete;
  StatusPublisher& operator=(const StatusPublisher&) = delete;

  // Reads ADSTATUSHANDLE / ADSTATUSLOG from the env. A handle value that does
  // not map is logged once and ignored. True when a record is mapped.
  bool open(const Env& env);
  // Map an explicit handle (tests). The publisher does not close it.
  bool open_handle(HANDLE section);
  void set_log(bool on) { log_ = on; }

  void publish(const LaneStatus& s, uint64_t frames, uint64_t input_applied, uint32_t lane,
               bool ready = true);

  bool mapped() const { return view_ != nullptr; }
  bool active() const { return view_ != nullptr || log_; }
  const AdwHostStatusV1& last() const { return last_; }

 private:
  void* view_ = nullptr;
  bool log_ = false;
  bool logged_once_ = false;
  AdwHostStatusV1 last_{};
};

// "STATUS <frame> flags=0x<hex> applied=<n> eaten=<n> src=<s>" (no newline).
std::string format_status_line(const AdwHostStatusV1& s);

}  // namespace adw
