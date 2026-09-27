# host/win16 — the Win16 guest runtime and API shims

`adw_win16` is one emulated Win16 "task" for Long After Dark's Classic lane
(`host/ne16`). That lane runs the 16-bit modules of all five releases: the
After Dark 2.x/3.x modules in After Dark 4.0 Deluxe's `FILES\CLASSIC`, After
Dark 3.2, Totally Twisted, The Simpsons Screen Saver, and the 16-bit modules
of After Dark 10th Anniversary. The module, its package's engine when it uses
one (`ADXPL300.DLL`, `ADXPL40.DLL` or `ADXPL310.DLL`), `AD_SND.DLL`, the helper
DLLs and, where the package ships it (Deluxe, 10th Anniversary), the real
`OLDMOD16.DLL` run as 16-bit protected-mode code on `adw::cpu`, over a
host-owned LDT, with KERNEL/USER/GDI/MMSYSTEM/… supplied from here. The
packages without `OLDMOD16.DLL` go through the lane's native AD3 bridge
instead (`ne16/bridge.hh`, PACKAGES.md §7.4). Design and contract:
`docs/DESIGN.md`, `docs/ABI.md` §3/§4/§8 (verified on the
Deluxe binaries), `docs/API_SURFACE.md` §2, `docs/PACKAGES.md`
§7.3/§7.4.

| File | What it owns |
|---|---|
| `layout16.hh` | The linear address space: BIOS data area (selector 0x40), system segment (environment, PSP, host strings), thunk segment, task stack, the global arena. |
| `ldt.hh/.cc` | `Ldt` (the CPU's `DescriptorProvider`): LDT selectors with RPL 3, handles = selector with bit 0 clear, huge blocks tiled `__AHINCR` (8) apart, the GDT selector 0x40. |
| `global_heap.hh/.cc` | `GlobalHeap16`: GlobalAlloc/ReAlloc/Free/Lock/Unlock/Size/Handle/Flags over `win32::GuestHeap` (one arena, so DIB sections can alias any block), module segments (`alloc_block`), DGROUPs reserved at 64 KiB. |
| `local_heap.hh/.cc` | `LocalHeaps16`: per-segment local heaps (LocalInit/Alloc/…), fixed pointers 4-aligned, moveable handles ≡ 2 mod 4 pointing at real handle-table entries, `DS:[6]` = pLocalHeap, growth to the segment end. |
| `shims16.hh/.cc` | Far thunks (`int 0xFE; dw id`), `Shim16Registry` keyed `MODULE.ordinal`, `Call16` (Pascal/cdecl argument readers, AX / DX:AX results), the unimplemented census. |
| `signatures16.cc` | **Generated** (`research/win/gen_sig16.py`): name, convention, return width and argument bytes of every entry of the emulated system DLLs, from the Win16 interface facts in `research/win/spec`. |
| `runtime16.hh/.cc` | `Runtime16`: the CPU in segmented mode, `call_far` (nested host→guest calls to a sentinel), thunk dispatch, faults (→ `GuestError16`), VGA ports (0x3DA retrace from the clock, DAC 0x3C7–0x3C9 on the display palette), virtual time, debug knobs. |
| `modules16.hh/.cc` | `ModuleTable16`: NE loading via `adw::loader::ne` (place → selectors → dependencies → `load_segments` → prolog patching → LibEntry → DLLENTRYPOINT), LoadLibrary/FreeLibrary/GetModuleHandle/GetProcAddress (names case-insensitive, constant exports), resources incl. Win 3.0 `NAMETABLE`s, pseudo modules for the system DLLs. |
| `dos16.hh/.cc` | INT 21h (DOS 7.00: files as a handle table over `win32::Vfs::open`/`VfsFile`, directories, rename, FindFirst/FindNext on the merged listing), INT 1Ah/2Fh/25h/26h/10h/16h/31h, the guest disk's seeds (below: `C:\WINDOWS` and its `TEMP` as in-memory overlays until the lane mounts its own, `MODULES.INI`/`AD_PREFS.INI`/`AFTERDRK.INI` as empty virtual files with their settings as profile seeds, `seed_program_manager`'s `PROGMAN.INI` and `.GRP` files as virtual files), and `profiles16()`, the runtime's `win32::IniStore`. |
| `input16.hh`, `keyboard16.cc` | Saver-window input (below): the WH_KEYBOARD chain, input messages tagged with their input line, the per-step report (consumed, queue reads, wake); the fixed US keyboard (scan codes, `TranslateMessage`'s characters, `key_lparam`). |
| `dialogs16.hh/.cc` | Configure mode (below): the Win16 → Win32 dialog template converter, the message translation table, and the shims that make a module's dialogs, message boxes and file dialogs real. |
| `gdi16.hh`, `gdi16_objects.cc`, `gdi16.cc` | `Gdi16`: Win16 GDI objects on real GDI with the Win32 lane's key-table model (`win32/display.hh`); screen DCs are DIB sections over the display's bits; DIBs are translated to hardware indices through the DC's palette. Also USER's SelectPalette/RealizePalette. |
| `kernel16.cc user16.cc system16.cc` | The API families (`register_<family>16(Runtime16&)`); `system16.cc` also has MMSYSTEM's clock, WIN87EM, COMMDLG, KEYBOARD, SHELL, TOOLHELP and `register_all16()`. `user16.cc` also holds the synthetic desktop and the icons (below), `SHELL.ExtractIcon`, and the host-posted messages MMSYSTEM's callbacks use (`user16_post_host`/`user16_dispatch_host`). |
| `sound16.hh/.cc` | MMSYSTEM's sound half (below, AUDIO.md §8) over the host audio engine (`adw/core/audio.h`): `sndPlaySound`, `waveOut*`, `midiOut*`/`aux*` volumes, the mixer (none), `mciSendString`'s sequencer, the MCISEQ.DRV stub, and the delivery of `MM_WOM_*`/`MM_MCINOTIFY`. Without an enabled engine: the silent device, byte for byte. |

## The synthetic desktop and icons (PACKAGES.md §7.3)

The desktop-icon gatherers of ADXPL40 (Totally Twisted: `CHAM`) and ADXPL310
(The Simpsons: `HOMEREAT`, `INS`) collect "desktop icons" from `EnumWindows`
(visible top-level windows with a class icon and a title) and Program
Manager's groups (`PROGMAN.INI [Groups]` → each `.GRP` file's name at the
`pName` offset 0x16). With none they `GlobalAlloc(…, 0)`, `GlobalLock`
fails and the module stops with "Out of memory". The first `EnumWindows`
brings a fixed desktop into being: a Windows 3.1 Program Manager window
(class `Progman`, title "Program Manager", HWND 0x0380 — below the window
counter, so no other window is renumbered — with a class icon drawn by the
host) under the saver window, and `C:\WINDOWS\PROGMAN.INI` naming five groups
(Main, Accessories, Games, StartUp, After Dark). A module that never
enumerates windows sees exactly the machine it always did: `BADDOG3` (Deluxe)
looks for `PROGMAN` with `FindWindow` and reads `PROGMAN.INI`, and its stream
must not change. `GetWindow`, `GetClassWord`, `GetWindowText`,
`GetClassName`, `IsWindowVisible` and `GetWindowPlacement` answer
consistently for the desktop's windows; `GetDCOrg` reports a DC's window
corner (0, 0 for the saver).

System bitmaps: `LoadBitmap(NULL, OBM_*)` — the system-menu box, the
minimize/maximize/restore buttons (and their pressed forms) and the four
scroll arrows — are drawn by the host in the Windows 3.1 look with the static
colours (`user16.cc` `system_bitmap`), SM_CXSIZE × SM_CYSIZE (18) or the
scroll-bar size (16). INS draws a window's chrome with them and stopped with
"Out of memory" when they were missing; OBJETS and Bad Dog load them too.
Other ids stay 0.

Icons (`LoadIcon`, `CopyIcon`, `CreateIcon`, `ExtractIcon`, `DrawIcon`,
`DestroyIcon`) are images in `UserState`, handles 0x8000–0xBFFC (multiples of
4: no selector, GDI object or window has one). Their pictures are the guest's
own `RT_GROUP_ICON`/`RT_ICON` resources (ADXPL40's and ADXPL310's group
icon 100), bits a module passes `CreateIcon`, or the Program Manager icon the
host draws — never the host's system icons, whose art differs between
Windows versions. `LoadIcon(NULL, IDI_*)` stays the image-less 0x0F04 it
always was. `DrawIcon` paints the opaque pixels through the DC's palette.

The display's starting palette (`Runtime16Options::desktop_palette`): the 20
statics with black between them, or with 236 distinct colours between them
(a 6×6×6 cube and 20 greys), as a Windows 256-colour desktop left the system
palette. ADXPL310 builds an "identity" palette from `GetSystemPaletteEntries`
at start-up and converts its full-screen canvas's colour table through it
with `GetNearestPaletteIndex`; with 236 identical black entries every
non-static index maps to 0 and `SIMPCLOK` draws its clocks in black. The
Classic lane turns it on for the AD 3 generation packages only (`ne16/lane.hh`).

## Sound (AUDIO.md §8)

`attach_audio16(rt, engine)` (the lane passes `LaneContext::audio`) decides
what MMSYSTEM is. Without an enabled engine (none, or `ADSOUND`/`ADAUDIOOUT`
unset) it is the silent device of before, answer for answer: one wave-out
device "Long After Dark (silent)" whose format queries all succeed and whose
`sndPlaySound` reports the sound played, no MIDI, aux or mixer devices, MCI
refused (`MCIERR_DEVICE_NOT_INSTALLED`); the calls it never answered
(`waveOutWrite` & co., the rest of `midiOut`) stay unimplemented. AD_SND.DLL
needs the wave device to initialize at all (ABI.md §3.6).

With the engine on (every audio call carries `Runtime16::peek_us()`):

* **`sndPlaySound`**: a `SND_MEMORY` image is copied at the call — its RIFF
  extent, bounded by the segment (AD_SND unlocks it right after) — then
  parsed and decoded (PCM, MS-ADPCM for the Totally Twisted banks, IMA-ADPCM)
  into one engine buffer on the wave bus. One voice per process: a new call
  replaces the sound, `SND_NOSTOP` while one plays is FALSE, NULL stops,
  `SND_LOOP` loops (and is always asynchronous). Without `SND_ASYNC`
  (NOCTURNE) the call lasts the sound's duration of virtual time
  (`wait_until_us`, below). A name is a file (as given, then `WINDOWS` and
  `SYSTEM`, `.WAV` assumed) or a `WIN.INI [sounds]` entry. Undecodable: FALSE.
* **`waveOut*`**: one device, "Long After Dark". `WAVE_FORMAT_QUERY` accepts
  PCM, and IMA-/MS-ADPCM on `WAVE_MAPPER` (decoded chunk by chunk when a
  stream is opened that way). Streams follow the Windows header model
  (`WHDR_PREPARED`/`INQUEUE`/`DONE`, `WAVERR_STILLPLAYING`/`UNPREPARED`),
  `GetPosition` in bytes/samples/ms, `Pause`/`Restart`/`Reset`, and
  `CALLBACK_NULL`/`WINDOW`/`TASK`/`FUNCTION` with `MM_WOM_OPEN`/`DONE`/
  `CLOSE`. Handles are 0xE000–0xFFFC (multiples of 4). `waveOutSetVolume` is
  the engine's wave bus.
* **`midiOut*`, `aux*`**: one MIDI device ("Long After Dark MIDI", mapper
  technology, `MIDICAPS_VOLUME|LRVOLUME`) — the engines' `IsMusicAvail` — and
  two aux devices (0 CD audio: stored; 1 = the MIDI bus). `midiOutSetVolume`
  and `auxSetVolume(1)` are one volume, the engine's MIDI bus. The rest of
  `midiOut` is `MMSYSERR_NOTSUPPORTED`. No mixer (`mixerGetNumDevs` 0), and
  with the engine on no mixer API by name either (`Shim16Entry::by_name`:
  `GetProcAddress` answers 0, ordinals resolve): AD_SND 3.2/TT picks its
  mixer path whenever `GetProcAddress` finds those names, and with no mixer
  device that path sets no volume at all; without them it sets the wave and
  MIDI volumes, so `ADVOLUME` reaches the AD 3.2 and Totally Twisted modules
  (AUDIO.md §2.10).
* **`mciSendString`**: the sequencer's commands (AUDIO.md §8.4): `open
  sequencer`, `open sequencer!<path> [alias a]`, `open <path> type sequencer`
  (or a `.mid`/`.rmi` path alone), `close <a>|all`, `play [from n] [to n]
  [notify]` (`play … wait` is `MCIERR_UNSUPPORTED_FUNCTION`), `stop`,
  `seek to start|end|n`, `status mode|length|position|ready|…`, `set time
  format ms`, `set port mapper`; `wait` is accepted and harmless elsewhere.
  Paths go through the VFS (relative to the current directory). `open`
  returns the device id (lowest free from 1); errors are the `MCIERR_*`
  Windows gives; the return buffer is always NUL-terminated. A play's
  `notify` sends `MM_MCINOTIFY(SUCCESSFUL, id)` to the callback window at the
  song's end (or its `to`); a later `notify` supersedes it (`SUPERSEDED`),
  `stop`/`seek`/`close` abort it (`ABORTED`), either before the new
  command's own notify.
* **The engines' music gates** (ADXPL300/310/40, AUDIO.md §2.9): a MIDI
  device; `LoadLibrary("TOOLHELP.DLL")` (a system module) and
  `LoadLibrary("MCISEQ.DRV")` (a stub system module registered only with the
  engine on) >= 32; `GetProcAddress(TOOLHELP, "GLOBALFIRST"/"GLOBALNEXT")`
  non-NULL. The engines use GlobalFirst/GlobalNext only on Windows 3.10
  exactly (to page-lock MCISEQ's segments, ADXPL310 4:f46f); on the 3.95 we
  report they never call them. They reload MCISEQ every 100 songs, and
  around every play set `system.ini [mciseq.drv] disablewarning=true` and
  write the old value back — the key is seeded `true` (a profile seed, sound
  on only), so no SYSTEM.INI is written to a persistent state directory.
  Their hidden `adwMidiCall` window's `MIDIWNDPROC` takes `MM_MCINOTIFY`:
  SUCCESSFUL replays (loop) or ends the song, SUPERSEDED and FAILURE end it,
  ABORTED is ignored.

**Callbacks** (AUDIO.md §8.6): the engine's events (`chunk_done` →
`MM_WOM_DONE` and the header's `WHDR_DONE`, `song_end` → `MM_MCINOTIFY`) and
the ones MMSYSTEM raises itself (`MM_WOM_OPEN`/`CLOSE`, SUPERSEDED/ABORTED)
queue with their virtual time and issue order. They are delivered at the
first API call at or after that time (the runtime's audio hook, below) and
at the lane's pump before every DRAWFRAME (`audio16_pump`): window messages
are posted to the guest's queue (`user16_post_host`), where a guest that
pumps takes them, and the pump sends those still waiting to their window
procedures, as the 1996 host's message loop did between DRAWFRAMEs;
`CALLBACK_FUNCTION` procedures are called as a nested `call_far` with their
module's DS. Never while a delivery runs (a callback's own API calls deliver
nothing). Everything is a function of the guest's calls and virtual time.

## The guest's disk (INTERACTION.md §7)

Files and profiles go through `win32::Vfs` and `win32::IniStore`, shared
with the pe32 lane. The Classic lane mounts (`ne16/lane.cc` `mount_disk`):

| Guest | Lower (read-only) | Upper |
|---|---|---|
| `C:\WINDOWS` | virtual seed files (`MODULES.INI`, `AD_PREFS.INI`, `AFTERDRK.INI` empty; `PROGMAN.INI` and the `.GRP` files once the synthetic desktop exists; `LunData.dat`, the module dir's `LUNDATA.DAT`, where the installers copied it) | `<ADSTATE>\<package>\WINDOWS`, or memory |
| `C:\WINDOWS\TEMP` | — | memory, always |
| `C:\WINDOWS\SYSTEM` | the engine dir | none (read-only) |
| `C:\AFTERDRK`, `C:\AFTERD~1` | the module dir | `<ADSTATE>\<package>\<MODDIR>` (one directory for both names; in memory mode each name has its own) |
| `H:\<L>\…` | the host's drives, 8.3 names | none |

Opening a lower file for writing (`_lcreat`, `OpenFile(OF_CREATE|OF_WRITE…)`,
INT 21h 3Ch/3Dh/5Bh/6Ch) copies it up first; new files and directories go
to the upper layer; deleting or renaming a lower file fails (access denied).
Profiles read the seeds under the file (the file wins per key) and write the
upper file only. Without `ADSTATE` every upper layer is memory: a headless
run reads and writes nothing of the user's, and its frames are what they
always were. Where modules keep their state (verified in configure runs):
MESSAGE3 `MESG_AD3.DAT`, NONSENSE `NONSENSE.TXT`, SLIDE `BITMAPS.ADC`,
WMORPH `morph*.dat` (module dir); FISHPRO `[Fish]`, BUGS `[Bugs]`, ARTIST
`[The Artist] Image`, LOGO `[Logo Section] LogoFile`, Message Mayhem
`[Message Mayhem] CustomA` (`MODULES.INI`); GLOBE `AD_PREFS.INI`; LUNATIC
`LunData.dat` (in `GetWindowsDirectory()`, so `<package>\WINDOWS`; read from the seed until
Keys… or a high score writes it).

## Saver-window input (INTERACTION.md §5.2)

Modules mostly poll (`GetAsyncKeyState` & co. read the host's `InputState`;
`VK_RBUTTON`/`VK_MBUTTON` from the `MOUSE` bitmask). For those that take
messages, the lane (`ne16/lane.cc`, "Input and status") hands every `KEY`
line to the WH_KEYBOARD chain (`SetWindowsHook`/`SetWindowsHookEx(2)`, most
recent first; `DefHookProc(…, &token)` and `CallNextHookEx(token)` call the
hook installed before; a non-zero result consumes the key) and otherwise
posts it to the saver window as `WM_KEYDOWN`/`WM_KEYUP` (Win16 `lParam`:
repeat 1, US scan code, extended bit, bit 30 previous state, bit 31
transition), `MOUSE` lines as `WM_MOUSEMOVE` and button messages, each
tagged with its input line. These wait in an input queue read after the
posted one, as Win16's system queue was; `PeekMessage`/`GetMessage` honour
their hwnd and range filters. A tagged message the guest removes and does
not `DispatchMessage` back to the saver window is consumed
(`user16_end_step`); one nobody took is dropped after its step, or kept up to
`keep_steps` more steps for a suspended DRAWFRAME that may still take it (the
lane passes 1, or 600 while that call reads the saver window's queue itself:
LUNATIC's game), and reported as `pending` meanwhile. Reading the
saver window's queue with removal for a range that includes keys counts as a
queue read (the lane's key-filter). `FindWindow("Sleep", NULL)` — the AD 2/3
blanker class LUNATIC looks for (LUNATIC `26:0089` pushes DS:0DE8 =
"Sleep") — answers the saver window. `PostMessage(saver, WM_CLOSE)` or
`SC_CLOSE` raises wake. `TranslateMessage` makes `WM_CHAR` from the US
layout; `KEYBOARD.MapVirtualKey`/`VkKeyScan` use the same tables (never the
host's layout: runs stay deterministic).

Measured (`ne16.interaction`): YBYH, SIMPTRIV, tt FRANKEN, tt MIMEHUNT and
HOW2DRAW install their WH_KEYBOARD hook when they enter the game (0x0E) and
remove it when they leave; the ADXPL40/ADXPL310 engines hook nothing at load.
Without input only LUNATIC raises key-filter (every frame, before the first
key). MIMEHUNT also asks for a WH_MOUSE hook (refused: other hook kinds
return 0 as before).

## Configure mode: real dialogs (INTERACTION.md §6.2 Win16)

`adhostwin --configure` runs a module's button through the bridge
(`BUTTONPUSHED16`); the lane calls `enable_real_dialogs16`, which replaces
or wraps the USER/COMMDLG/KERNEL shims below. Nothing of it is installed in
the saver.

* `DialogBox(Param)`, `DialogBoxIndirect(Param)`, `CreateDialog(Param)`,
  `CreateDialogIndirect`: the RT_DIALOG template goes through our converter
  (`convert_dialog_template16`: DWORD-aligned items, UTF-16 from code page
  1252, the menu and a custom dialog class dropped, `SS_ICON` ordinals
  emptied, the guest's extra item bytes dropped) into the real
  `DialogBoxIndirectParamW`, owned by `--owner`. The host dialog procedure
  forwards to the 16-bit `DLGPROC` through `call_far` (Pascal), from the
  dialog's first message on (`WM_MEASUREITEM` and `WM_SETFONT` come before
  `WM_INITDIALOG`, whose `lParam` is the guest's init param).
* Real windows are HWND16s from `0xC000`–`0xDFFC` (multiples of 4: no
  selector, GDI object, icon or emulated window has one), mapped both ways.
  The wrapped shims (`GetDlgItem`, `SendDlgItemMessage`, `Set/GetDlgItemText`,
  `…Int`, `CheckDlgButton`, `IsDlgButtonChecked`, `CheckRadioButton`,
  `DlgDirList`/`DlgDirSelect` (on the guest's disk), `EndDialog`,
  `SendMessage`, `PostMessage`, `DefWindowProc`, `CallWindowProc`, window
  queries and moves, focus, capture, timers with guest `TIMERPROC`s, props,
  window words/longs incl. subclassing, `EnumChildWindows`, scroll bars,
  `CreateWindow(Ex)` on a real parent) act on the real window.
* Messages into the guest (Win32 → Win16): `WM_COMMAND` (`wParam` = id,
  `lParam` = MAKELONG(hwnd16, code)), `WM_CTLCOLOR*` → `WM_CTLCOLOR`
  (`wParam` = a DC wrapper, `lParam` = MAKELONG(hwnd16, CTLCOLOR_xxx); the
  colours it sets and the brush it returns go to the real DC), scroll
  messages (`lParam` = MAKELONG(pos, hwnd16)), the `*ITEM` structures as
  16-bit copies (`WM_DRAWITEM` with a DC wrapper), focus/activation (handles
  mapped), keys, mouse, `WM_TIMER`, `WM_PAINT`, and the dialog's and guest's
  own messages (DM_*, WM_USER + n). Guest classes named in a template or
  created on a real parent get a real class whose procedure forwards the
  same way (`WM_CREATE` with a 16-bit `CREATESTRUCT`); `SendMessage` to such
  a window calls the guest's procedure directly with the Win16 values.
* Messages from the guest to a real control (Win16 → Win32): numbers by the
  target's class (`msg16_to_32`: EM 0x400+n ↔ 0xB0+n, BM 0x400+n ↔ 0xF0+n,
  LB 0x401+n ↔ 0x180+n, CB 0x400+n ↔ 0x140+n; the local-handle and
  word-break messages and `STM_*` have none), far pointers marshalled
  (strings, `LB_GETTEXT`/`CB_GETLBTEXT`, `EM_GETLINE`, tab stops,
  `LB_GETSELITEMS`, rectangles; owner-draw lists without `HASSTRINGS` pass
  item data), `EM_SETSEL`/`EM_LINESCROLL` repacked, `LB_DIR`/`CB_DIR` listed
  from the guest's disk.
* DC wrappers: a real HDC reaches the guest as a gdi16 DC for one message
  (or a `GetDC`/`BeginPaint` … `ReleaseDC`/`EndPaint`): an 8-bit key
  surface filled from the real pixels (nearest hardware colour), with the
  control's real font as a guest font object; the guest draws with the
  ordinary GDI shims (palettes and all), and the surface goes back to the
  real DC through the hardware palette — what a 256-colour display showed.
* `MessageBox` → a real `MessageBoxW`; `COMMDLG.GetOpenFileName`/
  `GetSaveFileName` → the real dialogs (hooks and templates ignored,
  logged), the chosen host path back as an 8.3 `H:\` path
  (`Vfs::host_to_guest`); `WinHelp` → logged, 1; `WinExec("notepad
  <file>")` → the file copied into the upper layer and the real Notepad on
  that copy (not started when hidden or in memory); `EnumFonts` → the
  Windows 95 faces this host has.
* `ADCONFIGSCRIPT`/`ADCONFIGHIDDEN`/`ADCONFIGDUMP`/`ADCONFIGTIMEOUTMS`
  (`win32/config_script.hh`) attach once the guest has filled the dialog
  and it has been shown (a DialogBox shows when its queue first goes idle;
  MESSAGE3 builds its edit box on `WM_SHOWWINDOW`).
* A guest failure inside a real callback is kept, every real dialog ends,
  and the failure is rethrown when the real call returns.

## Extension recipe: adding or finishing a shim

1. **Find the family file** for the module (`gdi16.cc` for `GDI.nnn`).
2. **Signature**: every entry of KERNEL/USER/GDI/MMSYSTEM/… has a row in
   `signatures16.cc`. `r.impl("GDI", "PatBlt", fn)` looks it up by name
   (case-insensitive). A row that is a `stub` (no argument list in the spec)
   is declared with `r.add(module, ordinal, name, Conv16::pascal_, ret16,
   arg_bytes, fn)`. Never hand-edit `signatures16.cc`: re-run the generator.
3. **Handler**: `[](Call16& c) { … c.ret(v); }`
   * arguments in **declaration order**: `c.w()`, `c.sw()`, `c.l()`,
     `c.sl()`, `c.ptr()` (a far pointer, `sel:off` packed);
   * guest memory through far pointers: `c.rt.rd16/wr16/rd32/wr32`,
     `read_str`, `write_str`, `read_bytes`, `write_bytes`, `read16<T>` /
     `write16<T>` (packed 16-bit structs in `gdi16.hh`); a bad pointer throws
     `GuestError16(fault)` — Win16 would have GP-faulted;
   * results: `c.ret(v)` (AX, or DX:AX for a function the table marks
     32-bit), `c.ret32(v)`;
   * the caller's DS (Local*): `caller_ds(c)`;
   * callbacks into the guest: `c.rt.call_far(proc, {w16(a), l16(b)})` —
     PASCAL order, nests freely, restores every register (also when a guest
     fault or hang throws out of it), so call `c.ret` **after** any nested
     call. A callback may `Throw` to a `Catch` made outside the shim: the
     host frames in between unwind (`GuestUnwind16`, not a `std::exception`)
     and the guest resumes at the `Catch` — let it pass through your handler;
   * a handler that sets CS:IP/SS:SP itself (Throw, SwitchStackTo) calls
     `c.take_over()`.
4. **State**: `struct MyState : RuntimeState16 {…}` + `c.rt.state<MyState>()`.
5. **GDI**: `c.rt.state<Gdi16>()` — `get`/`dc`/`host_dc`, `key(hdc, colorref)`
   (the only form a colour may reach real GDI in), `sync(hdc)` before real
   GDI draws, `dc_palette`, `dc_surface`, `display()`.
6. **Time**: only `c.rt.tick_count()` (GetTickCount: 55 ms steps),
   `c.rt.time_ms()` (timeGetTime), `c.rt.local_filetime()`, `c.rt.clock_us()`.
   Every read advances virtual time (see below), so busy-waits end.
7. **Test**: `tests/test_win16.cc` builds guest code as bytes (see the
   `Machine` fixture); `adw_win16_tests --assets` loads OLDMOD16/AD_SND.
   `tests/test_sound16.cc` (`win16.sound`) tests MMSYSTEM's sound half
   against a scripted engine (exact times) and the real one.

## Virtual time

Headless runs are deterministic. Every clock read nudges virtual time by
`read_step_us`, plus one µs per `insns_per_us` instructions executed since
the previous read and `api_cost_insns` per API call (a notional 100-MIPS 1996
CPU on which a USER/GDI call costs 5 µs). Modules that draw until the tick
changes (SATORI) or calibrate on it (AD_RSRC, EINSTEIN) thus do the amount of
work they did on real hardware. That work runs *inside* the frame period: a
read sees `max(time already reached, frame × step)` plus its nudge, so a
frame that does less than a period's work costs no extra time (the lane
fills the period with DRAWFRAMEs, `ne16/lane.hh` "Pacing"), and only work
beyond the period pushes the clock past the frame grid. With `insns_per_us`
0 the nudges accumulate on the core clock instead (the lane's original
model, `ADMIPS=0`). `settle_time()` (the lane calls it as each frame ends)
charges the instructions run since the last read to that frame, so a module
that reads no clock while drawing does not carry a frame's work onto the next
frame's grid line. Streamed runs follow the wall clock (the instruction term
is off) from `start_frames()` on — the lane's first frame, when the realtime
core clock starts; before it (the module's load, where AD_RSRC, EINSTEIN,
GLOBE and Om Appliances calibrate on the tick count) reads are modeled as
headless, and the wall clock then continues from the time that took, so time
never goes back. `work_insns()` — instructions, API costs and
`pixel_cost_insns` per pixel a GDI blit or fill writes (`charge_pixels`) — is
what the lane's DRAWFRAME budget counts; pixels never move the clock.

`set_deadline(us, fn)` runs `fn` once, at the first API call (or retrace-port
read) at or after virtual time `us`, before the call does anything. The
Classic lane ends a frame there when a DRAWFRAME outlasts the period: `fn`
switches from the fiber the call runs on back to the host, which presents the
screen and resumes the call next frame (`ne16/lane.hh`, "Long calls").

`set_audio_hook(fn)` / `set_audio_due(us)`: `fn` runs at every API call at or
after `us` (after the scanout and deadline hooks, before the call); MMSYSTEM
keeps `us` at its next event (sound16.cc). `wait_until_us(us)` is a call that
blocked until virtual time `us` (a synchronous `sndPlaySound`): while the
yield hook (`set_yield_hook`, the Classic lane's long-call fiber) can end the
frame there, frames are presented and the call resumes on later ones until
guest time reaches `us` — the screen holds, as it did — otherwise the time is
charged at once (the modeled clock jumps; a realtime or read-step clock is
offset). Never a real wait.

The display is refreshed at a virtual 70 Hz, the timing the 0x3DA retrace
bit follows: `set_scanout_hook` runs at the first API call after each
refresh boundary (`peek_us()` includes instructions not yet charged), so a
lane can see what a monitor showed *during* a call — the Classic lane uses it
for content drawn and erased inside one DRAWFRAME (`ne16/lane.hh`, "Frames").

## Debugging

`ADTRACE` categories: `api16` (every call, raw argument words, result, caller),
`mod16` (loading, LibEntry, DllEntryPoint, GetProcAddress), `res16`
(FindResource), `file16` (files and profile strings), `dos` (INT 21h),
`throw16` (Throw with a BP-chain backtrace), `prof16` (host time per shim, at
exit), `pace` (the lane's DRAWFRAMEs, work and virtual time per frame, and frames that ended inside a call), `user16` (also host-posted messages), `sound` (every MMSYSTEM sound call
with its virtual time: voices, streams, MCI commands and their results,
notifies and callbacks as they are delivered), `lane`, `bt16` (every call with the BP chain of
its callers: which module code reached a shim), `dib16` (per `StretchDIBits`
of an 8-bit DIB: the source rectangle's index histogram, what it became, the
DIB's colour table and the DC's palette), `input16` (hooks installed and
called with their results, input posted/removed/dispatched/consumed,
`FindWindow("Sleep")`, wake), `dlg16` (configure mode: every message
forwarded to a guest dialog or window procedure, dialogs opened and ended,
classes made real, control messages without a Win32 form). Lane knobs `ADWATCH16=sel:off` (log every
change of that byte and the instruction that made it) and
`ADSTEP16=cs:lo-hi` (log every instruction executed in that range). Faults
report CS:IP as `MODULE seg:off` plus DS/ES/SS limits, and `log_state` prints
the registers, stack words and BP-chain frames where the failed call was
(captured before the unwinding restored the caller's registers);
`research/win/dis/<file>.asm` has the listings.

Build + test: `AD_BUILD_DIR=build/win-<key> AD_COMPONENTS="host/core;host/cpu;host/loader;host/win32;host/win16;host/ne16" bash tools/build.sh`
