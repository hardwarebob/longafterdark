# Long After Dark: status

**Status as of 2026-09-27 01:45 CDT.** Branch `windows-x86`, HEAD `027ef00`. Nothing is committed after it. The repository is now **Windows-only and flat**: everything that was under `windows\` is at the root, and the macOS side is gone.
Every planned stage is **done and verified**: audio, the final QA pass, module interaction, the flatten with the Mac removal, Windows CI and the casual-user README. What is left needs you (below). `git status`: 107 renames, 161 renames with edits, 58 staged deletions (the Mac side, `docs\RESEARCH.md`, the old workflow and the six old `scr\src\ui_*` files), 5 modified and 54 untracked entries.

`SCRATCH` = the earlier working session's scratch folder (outside the repository), where the evidence cited below was kept.

## Needs you

1. **The commit.** Nothing is committed after `027ef00`, and nothing new is staged beyond the `git mv` and `git rm` of the flatten.
   - **Do not commit** `10thanniv.jpg` (your file in the root) or `.vscode\`. Both are untracked but not ignored, so a plain `git add -A` would take them.
   - **Must be added**, or the build and CI fail: the untracked sources `git status` lists. They are `.gitattributes`, `.github\workflows\build.yml` (the index still holds the staged deletion of the old macOS one), `tools\versions`, `common\`, `cmake\adhostwin.rc`, `cmake\adhostwin.manifest`, `cmake\adw_version.h.in`, `importer\gui\`, the audio and cover sources under `host\`, `importer\` and `scr\`, `scr\tests\fixtures\catalog-releases.json`, `docs\AUDIO.md`, `docs\COVERS.md`, `docs\BUILDING.md`, `docs\images\` and `host\cpu\README.md`.
   - One way: `git add -A -- . ':(exclude).vscode' ':(exclude)10thanniv.jpg'`, then read `git status` before committing. `research\`, `source_iso\`, `third_party\` and `build\` stay ignored. Git's "LF will be replaced by CRLF" messages are `core.autocrlf=true` and harmless.
2. **Replace your copy in `D:\Temp\LongAfterDark`.** Checked read-only at 01:40: its `LongAfterDark.scr` and `adhostwin.exe` are older than the dist (no interaction fixes); only `adimport.exe` matches. Close its settings window and saver, then copy everything in `build\dist\LongAfterDark` over it.
3. **Run the by-hand release checklist once** (`scr\README.md`, "By hand, before a release"): right-click Install; Settings… and Preview inside Screen Saver Settings (`/p`); the timeout; a System32 install; unplugging and replugging a monitor, and a resolution change; display power-off; uninstall. No test may do these. **Also listen** to 30 minutes or more of Random: no automated test plays sound.
4. **Smaller decisions:**
   - **A release job.** CI builds and uploads the dist, but a `v*` tag no longer makes a GitHub Release (the old job needed the Mac build). A Windows one (zip the dist, `gh release create`) needs your go-ahead, as do an installer and code signing.
5. **Old research scripts** (`research\win\bringup\census.py`, `research\win\census.py`, `research\win\ne16_census\*.py`, the `research\win\pkg\*` inventories, `api_surface.py`, `make_catalog.py`) hard-code an out-of-date data-folder path. Use `research\win\qa\census.py` (scratch data only), or give them `AD_ASSETS_DIR` and `AD_LOCALAPPDATA`.

## Cleanup (done 2026-09-27 07:12)

- `LICENSE` now reads "the Long After Dark contributors"; the three programs' VERSIONINFO copyright follows it (CMake re-runs the configure when `LICENSE` changes), and the dist's README.txt no longer mentions the old name.
- `.vscode\settings.json`: `cmake.sourceDirectory` points at the repository root.
- `app\` (SourceKit-LSP index files) is deleted.
- `build\` went from 12.7 GB to 0.5 GB: the stale scratch build folders and the `build\dist-src` worktree (its `third_party` junction removed first) are gone. Left: `build\win-release` (current build), `build\dist` (the dist) and `build\win-pkg-setup\assets` (read by the asset tests through `AD_ASSETS_DIR` / `AD_PE32_PKG_ROOT`). Build folders named in the docs' example commands are recreated by `tools\build.sh` when used.
- `.kilo\worktrees\prism-dog` is Kilo's worktree (`ea9106e`); leave it to Kilo.

## Try it now

`build\dist\LongAfterDark\` (staged 07:13) holds `LongAfterDark.scr`, `adhostwin.exe`, `adimport.exe`, `README.txt`, `LICENSE.txt` and `licenses\`. Keep the three programs in one folder. Right-click the `.scr` and choose **Configure** (the settings window), **Test** (full screen; any key or mouse move ends it, Caps Lock starts a game where a module has one) or **Install**. `README.txt` there is the short user guide; `docs\INSTALL.md` is the long form and `README.md` the front page.

## Flatten, Mac removal, CI and README (done, 2026-09-27 00:50 – 01:45)

What changed:
- **Removed** with `git rm`: `engine\`, `app\`, `tools\adblit`, `tools\adfetch`, `tools\get_extractors.sh`, `tools\ppc-dasm`, `docs\RESEARCH.md` and the macOS workflow.
- **Moved to the root** with `git mv` (untracked files with `mv`): `CMakeLists.txt`, `cmake\`, `common\`, `host\`, `importer\`, `scr\`, `tools\` and `docs\*.md`. `windows\README.md` became `docs\BUILDING.md` ("Building Long After Dark").
- **Paths:** CMake's `AD_REPO_ROOT` is the source dir; `cmake\llvm-mingw.cmake` and `tools\*.sh` find the root one level up; `.gitattributes` pins `tools/versions` to LF; every `windows\…` path in code comments, CMake, `.rc`/`.in` files and docs is updated. Comments that compared with the Mac hosts now stand alone; `test_e2e.cc`'s internal test is `reference_frame_reader`. `.gitignore` is down to `.DS_Store`, `build/`, `assets/`, `source_iso/`, `research/`, `tools/scratchpad/` and `third_party/`.
- **Docs:** `docs\INSTALL.md` is the Windows-only user guide (games, module buttons, where your files are, updating). The 15 developer docs and component READMEs have root paths and no Mac-implementation text. `INTERACTION.md` has its Status line, the session-lock row and the pe32 handle base; `DESIGN.md` §8 is in the present tense. Facts about the media stay: the hybrid Mac/PC discs, and the Mac halves the importer skips.
- **CI** (`.github\workflows\build.yml`): one `windows-2025` job in Git Bash. It caches `third_party` keyed on `tools/versions`, runs `bootstrap.sh`, then `build.sh` into `build/win-release` with `-LE gui -j4`, then `package.sh`, and uploads `LongAfterDark-x64` (and `LastTest.log` on failure). No test needs the network or After Dark files: those suites skip themselves.
- **`THIRD_PARTY_LICENSES.md`:** the Mac-only extractors and sections are gone. What each program contains was checked against the linker maps (zlib only in `adimport.exe`, phosg in `adhostwin.exe` and the `.scr`, no winpthreads). A table lists what `licenses\` holds.
- **`README.md`** is rewritten for casual users: what it is, the five releases, what you need, four getting-started steps, tips, status, building, docs and licence. Four screenshots are in `docs\images\`: the settings window twice, the importer and a Flying Toasters frame. None shows Simpsons art or a personal path. They were made off screen from scratch data (`SCRATCH\readme-shots`).
- **Verify-stage fixes:** `tools\package.sh`'s README.txt no longer calls the games and option dialogs unfinished, and says how to stop playing (Caps Lock again, Alt, Win+L). `DESIGN.md`'s dist listing now names what `licenses\` really holds.

How it was verified (evidence in `SCRATCH\verify\`):
- **Structure:** every file of the pre-flatten tree (`SCRATCH\preflatten-tree.tgz`, 401 files) exists at its new path, except the 51 Mac files removed on purpose. `windows\` is gone. `git status --ignored` ignores only `.kilo`, `build`, `research`, `source_iso` and `third_party`: no source became ignored.
- **Audits** over the 348 tracked and untracked text files: no repository `windows\` paths, and no `engine/`, `app/AfterDark`, `adhost68k`, `adfetch`, `adblit`, `ppc-dasm`, macOS, Swift, `.saver` or `RESEARCH.md`. The remaining hits are allowed:
  - Media facts: `[Mac-PC].iso` names, Mac/PC hybrid CDs, the Mac halves skipped, Mac resource forks in `iso9660.cc`.
  - 68K/PPC in the vendored resource_dasm comments (`host\cpu\src`).
  - "engine" meaning After Dark's own engine DLLs.
- **Markdown:** the link check has 0 broken links; an anchor check has 0 broken anchors. The README's images exist, are PNG (59–148 KB, no text chunks) and were each looked at. Facts spot-checked against the code: CMake 3.24, the programs `package.sh` stages, the importer's image types and download sizes, 202 = 15+44+13+84+46 modules, the data folder.
- **Build:** incremental `build\win-release`: nothing to rebuild, 0 warnings (the flatten stage's full rebuild at the root was 0 warnings too).
- **`ctest -LE gui -j8`** with the release data (`AD_ASSETS_DIR`/`AD_PE32_PKG_ROOT` = `build\win-pkg-setup\assets`, scratch `AD_LOCALAPPDATA`, `ADAUDIOLIVE=0`), about 108 s a run:
  - First run: 67/68. `import.cli` failed once: renaming a scratch data folder got error 5 (Access denied) under load. It passed alone.
  - Second run of the whole set: **68/68 passed**. The 6 opt-in suites skipped as designed: `core.audio_assets`, `ne16.pkg`, `import.e2e`, `import.pkg_real`, `import.download_real` and `import.covers_real`. `import.gui_shots`, `ui.*` and `ne16.interaction` are in this set.
- **Dist:** `bash tools/package.sh` exit 0 (3 s). `build\dist\LongAfterDark` has the three programs, `README.txt` (CRLF), `LICENSE.txt`, and `licenses\` (NOTICE.txt plus five licence texts). `adimport.exe --help` exits 0.
- **CI file** parses as YAML. Its commands and paths match `tools\*.sh`: the build directory is the one `package.sh` uses, and ninja and CMake come from `bootstrap.sh` and the runner.
- **Line endings:** all 35 CRLF files are still CRLF (`SCRATCH\flatten\eolcheck.py`, 0 mismatches). `*.sh` and `tools\versions` are LF.
- Your real `%LOCALAPPDATA%\LongAfterDark` was not written: its newest entry is from 00:32.

Not run in this stage:
- **No `gui`-labelled test.** `scr_smoke_input-play` runs full screen on the primary monitor. `seed` stages its monitors at 0,0 on the real screen. `config-buttons` shows the settings dialog. `e2e-dosshell` is a 60 s `/S` with live input, off screen. `import.gui_flow` opens real windows. Since the interaction stage passed them, the saver changed only in comments.
- CI on a real runner (not pushed). The window-based non-GUI tests (`ui.capture`, `import.gui_shots`) rely on the runner's interactive desktop; if one fails there, add `-E "ui.capture|import.gui_shots"` to `AD_CTEST_ARGS`.
- A cold `bootstrap.sh` from an empty `third_party\`, the network suites, soaks and the full census.

## Module interaction (done, 2026-09-27 00:12 – 00:45)

Plan and final behaviour: `docs\INTERACTION.md` (Status: implemented and verified). The real saver (`LongAfterDark-test.scr /s` via `CreateProcess`) ran with the real `adhostwin` and all five releases, on a staged monitor, with input from `AD_SCR_TEST_INPUT` and frames from `AD_SCR_TEST_CAPTURE`. Sound was captured with `ADAUDIOLIVE=0`. Your keyboard, mouse and screens were never used. Scripts are in `SCRATCH\integ\`; contact sheets are in `research\win\ix\r2_*`.
- **Caps Lock games: PASS.** Rodger Dodger (`r2_rodger_sheet.png`), You Bet Your Head (`r2_trivia_ybyh_sheet.png`), Simpsons Trivia (`r2_trivia_simptriv_sheet.png`), Magic Turtle (`r2_turtle_sheet.png`) and Lunatic Fringe (`r2_lunatic_sheet.png`). Play starts on Caps Lock, keys reach the game, the saver never exits during play, and Alt, or Caps Lock then a key, ends it with the reason logged.
- **Module buttons and message input: PASS** (`r2_cfg_fish_sheet.png`, `r2_messages_sheet.png`, `r2_cfg_*msg_sheet.png`). Fish World's Select Fish… saved `FishTypes=1668` and the saver then showed only those fish. Messages 4.0 (ad40, ad10), Message Mayhem, Classic Messages and ad32 Messages show the typed text.
- **DOS Shell: PASS.** 310 s, about 18,000 frames, ended only on the scripted Alt (`r2_dosshell_sheet.png`).
- **Desktop seed: PASS** (Puzzle, Spotlight, Down the Drain; `r2_seed_sheet.png`). **`/p` previews** of CYBER and CRITIC: PASS (`r2_preview_sheet.png`).
- **Tests:** `ctest -LE gui -j8` 68/68. GUI 6/6 at `-j1`: `scr_smoke_input-play`, `input-alt`, `input-wake`, `seed`, `config-buttons` and `e2e-rodger`. `scr_smoke_e2e-dosshell` passed off screen.
- **Fixed:** `RealUi::wrap_dc` (`host\win32\realui.cc`) leaked its memory DC when the GDI table was full. `scr_smoke_e2e-dosshell` now stages its monitor off every real one and starts with `CreateProcess` and `/S`.
- **Still open from its review list** (none blocks a release):
  - The emulated MessageBox is titled "After Dark" (`host\win32\realui.cc:1508`).
  - `OFN_ENABLETEMPLATE` and `OFN_ENABLEHOOK` are stripped.
  - The settings window waits on a configure host with no timeout (by design, §6.1).
  - PerMonitorV2 and Common Controls 6 for adhostwin's dialogs are undecided.
  - HOW2DRAW and MIMEHUNT were not driven.
  - A synchronous `sndPlaySound` outside a DRAWFRAME in a streamed run loses its live audio (AUDIO.md §12).
  - Configure mode's `DirectSoundCreate` answers `DSERR_ALLOCATED`, as documented in INTERACTION.md §6.2.

## Final QA pass and verification (done, 2026-09-26 21:00 – 2026-09-27 00:25)

Evidence: `SCRATCH\final\` and `SCRATCH\final2\`.
- **Full ctest: 107/107** at `-j1` with the GUI tests and every opt-in data suite (`final\ctest_console.log`). A clean full build had 0 warnings.
- **Fixed:**
  - Artist's GDI leak: a peak of 17 objects over 36,000 frames; it was 10,000.
  - Swirling Magic's pace: pe32 blits and fills now cost their pixels.
  - The CPU: from 74 to about 120 MIPS, with all 202 FBHASH streams unchanged.
  - Presentation: `/s` draws through Direct2D, 1.7 ms/frame at 4K.
  - Safety: test hooks are compiled out of the shipped `.scr`; guest-sized allocations are capped; importer staging and download limits.
  - Several smaller UI and importer fixes.
- **10-module smoke census** on the release host: all exit 0, deterministic, 0 unimplemented APIs (`final2\census_out\`).
- Not re-run then: the 30-minute saver soaks (QA's passed, `SCRATCH\qa-saver\soaks`), and the full 202-module census (the host fixer's gave 202/202).

## Done (with evidence)

| Area | State | Evidence |
|---|---|---|
| Releases and modules | Five releases (Deluxe, 3.2, Totally Twisted, 10th Anniversary, Simpsons), **202 catalog modules** (129 distinct), both lanes (pe32 32-bit, ne16 16-bit) | QA census (`research\win\qa\census.json`): 202/202 exit 0 over 900 frames, deterministic, 0 unimplemented APIs |
| Importer | Disc, image, folder, ZIP or Internet Archive (md5-checked, resumable); atomic per-package install; covers (download, disc art, your own picture); GUI in the shared Windows 11 look; releases oldest first | `import.*` suites; `import.pkg_real` |
| Screen saver | `/s`, `/p` and `/c`; one host per monitor; Random with a release filter; relayout; retry/skip; desktop seed; the box-cover settings window | 40+ `scr_smoke_*` GUI tests; `scr_unit_*` |
| Interaction | Caps Lock games, module buttons and dialogs, message input, per-user module state, session lock ends the saver | Section above; `docs\INTERACTION.md` |
| Rename | "Long After Dark", `LongAfterDark.scr`, data folder `%LOCALAPPDATA%\LongAfterDark` | `scr_smoke_*`, `scr_unit_*` |
| Audio | Wave, DirectSound, ACM, MCI/MIDI, MMSYSTEM, AD_SND/SIMP_SND/TT_SND; only the primary monitor's host plays; Sound and Volume | `docs\AUDIO.md` §10.5; `core.audio_assets`, `scr_smoke_e2e-sound` |
| Build and packaging | sha256-pinned, self-healing `tools\bootstrap.sh` (pins in `tools\versions`); one version source (`adw_version.h`); reproducible links; `tools\package.sh` swaps the dist in only when complete; licence texts in `licenses\` | This stage's build and dist |
| Repository | Windows-only, flat layout; Windows CI; casual-user README with screenshots | This stage's verification |
| Docs | `README.md`; `docs\INSTALL.md`; `docs\BUILDING.md` (developer guide: components, tests, every opt-in variable, headless runs); DESIGN, ABI, API_SURFACE, AUDIO, COVERS, INTERACTION, PACKAGES; component READMEs | Link and anchor checks: 0 broken |

## Deferred (each with its reason)

- **FROST at 1280x720 and larger stays below 60 fps** (42 fps at 1280x720). Its DRAWFRAME runs millions of instructions with no API call to pause at; fixing it needs a pre-decoded instruction cache.
- **pe32 "stop early when a DRAWFRAME does no work".** Nothing reliably tells an idle clock-paced call from a module that counts its calls, so it would change their pace.
- **Comparing modules with the originals** (HULA's black ground, GUERNSEY, SHADOW's colour cast, pace in general). This needs captures from real Win95 hardware or a VM.
- **Sound on a real device.** No automated test opens one (WASAPI device loss, the `waveOut` fallback, live-MIDI note release, SysEx on a slow port). It waits for you (Needs you 5).
- **The importer's test hooks** (`AD_IMPORT_TEST_SCREENSHOT`, `AD_IMPORT_TEST_PICK`) are still read by the shipped `adimport.exe`. They act only when set, and none can make a program ignore input. Fix: a twin `adimport-test.exe`, as the saver has.
- **The real file-open dialog is not tested automatically.** A real-dialog test needs UI Automation on a free desktop.
- **The FILES swap is two renames.** Windows cannot replace a non-empty folder in one step; rollback and recovery exist.
- **Rare Win32 failures are left unchecked** (CreateEvent in the live audio sink, the CreateProcess fallback, GlobalLock, SetWindowRgn, GetFileSizeEx on an image). Fix them when those files are next touched.
- **The line endings of `scr\tests\fixtures\*`** stay `text=auto`. Pinning them is the saver owner's call.
- **Future releases:** AD 3.2's built-in Starry Night, `ECOLOGIC.DLL`, the other AD 3.x collections, More After Dark and AD 2.x. Each needs its own survey first (PACKAGES.md §12).
- **No installer, code signing or release job** (Needs you 6).
- **Research only:** `research\win\audio\census_result.py` names captures by file stem, so Deluxe's two `RAIN.AD` share one capture.

## Known issues and notes

- `package.sh` refuses to replace `build\dist\LongAfterDark` while a program in it runs: it exits 1 and changes nothing. Close the saver and the settings window first.
- `import.cli` failed once under `ctest -j8` in this stage: a scratch folder's rename got Access denied (probably a scanner holding a file), and it passed on its own and in the next full run. `ne16.interaction` did the same once earlier. Run the GUI suites with `-j1`.
- `package.sh` reconfigures `build\win-release` with only the shipped components; the next `build.sh` there reconfigures it back to the whole tree. Nothing is rebuilt either way.
- To run the saver with test hooks, use `build\<dir>\scr\LongAfterDark-test.scr` with `AD_HOST_EXE`. The packaged `.scr` ignores every `AD_SCR_TEST_*` variable.
- On this machine, a new unsigned test process can take about 15 s for its first `std::filesystem::remove` of a PNG it just wrote (probably Defender-style scanning). Some GUI tests take 15 s longer as a result, still within their timeouts.
- FBHASH is deterministic only headless; streamed runs follow the wall clock. "0 unimplemented APIs" does not prove the pixels are right.
- `research\win\qa\census.py` reads the unimplemented-API count only from pe32 lines; for ne16 modules read the `[census16]` line in the run's log.

## How to build and run

```bash
# Git Bash, from the repository root
bash tools/bootstrap.sh     # once, and after a pin in tools/versions changes; a rerun is a no-op
AD_BUILD_DIR=build/win-release AD_CTEST_ARGS="-LE gui -j8" bash tools/build.sh   # configure the root, build, non-GUI ctest
bash tools/package.sh       # the three programs, Release -> build/win-release, staged in build/dist/LongAfterDark (AD_DIST_DIR overrides)
#   Plain `bash tools/build.sh` uses build/win (not made yet: a full build). Knobs: AD_BUILD_DIR, AD_COMPONENTS, AD_NO_TESTS=1, AD_CTEST_ARGS.

# Tests against the imported releases (read-only) plus opt-in suites; docs/BUILDING.md "Test" lists every variable
export AD_ASSETS_DIR="$PWD/build/win-pkg-setup/assets" AD_LOCALAPPDATA='C:\Temp\lad' ADAUDIOLIVE=0
AD_AUDIO_ASSETS=1 AD_E2E_PKG=1 AD_PE32_PKG_ROOT="$AD_ASSETS_DIR" ctest -LE gui -j8 --output-on-failure   # in the build dir
#   AD_E2E=1 adds the Internet Archive downloads (network) and the real-saver e2e runs. Never set AD_AUDIO_LIVE_TEST.

# Headless module (always with scratch data): 300 frames, one FBHASH per frame, PPMs in ADOUT
ADFRAMES=300 ADGOWAITMS=0 ADFBHASH=1 ADOUT=C:/Temp/adframes build/win-release/host/core/adhostwin.exe FILES/AD40/TOASTERS.AD
#   capture sound without playing it: ADAUDIOOUT=C:/Temp/t.wav   pacing trace: ADTRACE=pace   DRAWFRAME count: ADTRACE=lane   other knobs: each lane.hh
research/win/venv/Scripts/python.exe research/win/qa/census.py --exe build/win-release/host/core/adhostwin.exe --scratch C:/Temp/census --runs AB

# The saver with test hooks: never the dist's .scr. MSYS_NO_PATHCONV=1 keeps "/s" intact in Git Bash.
MSYS_NO_PATHCONV=1 AD_SETTINGS=C:/Temp/settings.ini AD_HOST_EXE=build/win-release/host/core/adhostwin.exe \
  AD_SCR_TEST_MONITORS="-16000,0,640,480,p" AD_SCR_TESTEXIT_AFTER_FRAMES=90 AD_SCR_LOG=C:/Temp/scr.log \
  build/win-release/scr/LongAfterDark-test.scr /s
```

For a test, never launch a `.scr` through Explorer, ShellExecute or `start`: Windows adds `/S` and runs the real full-screen saver. Stage its monitors off the real screens (as above), and never point a test at `%LOCALAPPDATA%\LongAfterDark`.
