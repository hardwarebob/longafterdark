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
//                    IsMusicAvail), two aux devices (0 CD, 1 = the MIDI bus);
//                    volumes only. No mixer.
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
// no MIDI/aux/mixer devices, MCI refused. ADSOUNDDEV=0 (Runtime16Options::
// sound_device) removes the wave device in both modes.
//
// Callbacks (§8.6): the engine's events (chunk_done → MM_WOM_DONE, song_end →
// MM_MCINOTIFY) and the ones MMSYSTEM raises itself (MM_WOM_OPEN/CLOSE,
// SUPERSEDED/ABORTED) are delivered in (virtual time, issue) order at the
// first API call at or after their time (Runtime16's audio hook) and at the
// lane's pump before each DRAWFRAME. Window callbacks are posted to the
// guest's queue (user16_post_host) and dispatched by the pump unless the
// guest takes them itself; CALLBACK_FUNCTION procedures are called then, as a
// nested call_far with their module's DS, never while another delivery runs.
// Everything is a function of the guest's calls and virtual time.
#pragma once

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
// wave streams and MCI devices the guest left open. Nothing is delivered.
void audio16_close(Runtime16& rt);

}  // namespace adw::win16
