// MMSYSTEM's sound half for the Classic lane (docs/AUDIO.md §8), over
// the host audio engine (adw/core/audio.h):
//
//   sndPlaySound     SND_MEMORY images (PCM, MS-/IMA-ADPCM) copied at the
//                    call, or a file / WIN.INI [sounds] name; one voice per
//                    process; SND_LOOP loops; without SND_ASYNC the call lasts
//                    the sound's duration of virtual time (Runtime16::
//                    wait_until_us), never a real wait.
//   waveOut*         one device, "Long After Dark": format queries (PCM, and
//                    IMA-/MS-ADPCM on WAVE_MAPPER), real streams with
//                    CALLBACK_NULL/WINDOW/TASK/FUNCTION and MM_WOM_OPEN/DONE/
//                    CLOSE, WAVEHDR flags, positions, the wave bus volume.
//   midiOut*, aux*   one MIDI device ("Long After Dark MIDI"; the engines'
//                    IsMusicAvail), two aux devices (0 CD, 1 = the MIDI bus),
//                    their volumes. No mixer. The MIDI device's raw port, for a
//                    guest that sequences itself (MEMMIDI, under SWSE): one
//                    client at a time (MMSYSERR_ALLOCATED), midiOutShortMsg/
//                    LongMsg/Reset into the engine's raw MIDI (its .mid log,
//                    the live synth), MIDIHDR flags, MM_MOM_OPEN/DONE/CLOSE,
//                    patch caching not supported (no MIDICAPS_CACHE).
//   mciSendString    the sequencer's command strings (§8.4) over the engine's
//                    songs; MM_MCINOTIFY to the callback window at a song's
//                    end, SUPERSEDED/ABORTED when a later command replaces or
//                    interrupts a pending notify.
//   MCISEQ.DRV       a stub module, so the engines' LoadLibrary gate (§2.9)
//                    passes.
//
// Without an enabled engine (none attached, or ADSOUND/ADAUDIOOUT unset)
// every answer is the silent device's the lane always had, byte for byte:
// one wave device named "Long After Dark (silent)" whose sounds play nowhere,
// no MIDI/aux/mixer devices, MCI refused; midiOutOpen fails honestly
// (MMSYSERR_NODRIVER for the mapper) with the handle zeroed. ADSOUNDDEV=0
// (Runtime16Options::sound_device) removes the wave device in both modes.
//
// Callbacks (§8.6): the engine's events (chunk_done → MM_WOM_DONE, song_end →
// MM_MCINOTIFY), the ones MMSYSTEM raises itself (MM_WOM_*/MM_MOM_OPEN,
// DONE, CLOSE, SUPERSEDED/ABORTED) and the periods of multimedia timer events
// (timeSetEvent, system16.cc; engine or no engine) are delivered in (virtual
// time, issue) order at the first API call at or after their time
// (Runtime16's audio hook) and at the lane's pump before each DRAWFRAME.
// Window callbacks are posted to the guest's queue (user16_post_host) and
// dispatched by the pump unless the guest takes them itself; CALLBACK_FUNCTION
// and timer procedures are called then, as a nested call_far with their
// module's DS, one per timer period, never while another delivery runs. A
// procedure ran at interrupt time on the original, at its event's time: the
// MMSYSTEM calls it makes are dated from there (its due time plus the virtual
// time it has run), so music a timer sequences keeps its tempo and timing
// however late the delivery point came — neither a delivery nor the lane's
// step end renders the engine past a due point still to be delivered (the
// step end stops at Runtime16::audio_due). MCI commands are dated at the
// delivery point even there (no interrupt-time API). What a procedure causes
// — its device opened or closed, a long message sent, a WAVEHDR written that
// is done at once — waits for a later delivery point, so a procedure that
// answers every notification with another request takes one step per
// delivery point. A periodic event more than 250 ms behind drops its oldest
// periods (and an event a procedure sets starts no more than 250 ms back).
// Everything is a function of the guest's calls and virtual time.
#pragma once

#include <cstddef>
#include <cstdint>

namespace adw::audio {
class Engine;
}

namespace adw::win16 {

class Runtime16;

// The engine the lane runs with (LaneContext::audio): once, before the module
// loads. nullptr or a disabled engine keeps the silent device. With an
// enabled one the MCISEQ stub module is registered and the runtime's audio
// hook installed.
void attach_audio16(Runtime16& rt, audio::Engine* engine);
// An enabled engine is attached.
bool audio16_enabled(Runtime16& rt);
// The lane's pump, before each DRAWFRAME: delivers what is due by now, then
// dispatches the host-posted messages the guest has not taken.
void audio16_pump(Runtime16& rt);
// The run is over (after UNLOAD): stops the sndPlaySound voice, closes the
// wave streams, MIDI handles and MCI devices the guest left open, kills its
// timer events. Nothing is delivered.
void audio16_close(Runtime16& rt);

// Multimedia timer events (MMSYSTEM timeSetEvent/timeKillEvent, system16.cc),
// delivered with the callbacks above. A new event calls proc(id, 0, user, 0,
// 0) — FAR PASCAL, with DS = ds — every delay_ms (periodic) or once, from
// delay_ms after now (after a procedure's due time when a procedure sets it).
// 0 when delay_ms or proc is 0 or 16 events exist; ids are never 0.
uint16_t timer16_set(Runtime16& rt, uint16_t delay_ms, uint32_t proc, uint32_t user, bool periodic, uint16_t ds);
// False when no event has that id (a one-shot event is gone once called).
bool timer16_kill(Runtime16& rt, uint16_t id);
struct Timer16Stats {
  size_t active = 0;
  uint64_t calls = 0;    // procedure calls made
  uint64_t dropped = 0;  // periods dropped behind the lag bound
  uint64_t insns = 0;    // guest instructions the procedures ran (their API calls' own work excluded)
};
Timer16Stats timer16_stats(Runtime16& rt);

}  // namespace adw::win16
