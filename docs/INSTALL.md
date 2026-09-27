# Installing Long After Dark

**Long After Dark** is a screen saver for Windows that runs the original
Windows After Dark modules, unchanged, under x86 emulation. It knows five
releases:

| id | Release | Internet Archive download |
|---|---|---|
| `deluxe` | After Dark 4.0 Deluxe (1996) | CD image, 381.7 MB |
| `ad10` | After Dark 10th Anniversary (1999) | CD image, 143.3 MB |
| `ad32` | After Dark 3.2 (1995) | CD image, 58.8 MB |
| `tt` | Totally Twisted After Dark (1995) | CD image, 37.9 MB |
| `simpsons` | The Simpsons Screen Saver (1994) | install files (ZIP), 2.6 MB |

Requirements: 64-bit Windows on an x64 PC. It was developed on Windows 11.

## The programs

Download `LongAfterDark-<version>-x64.zip` from the
[latest release](https://github.com/starrlord/longafterdark/releases/latest)
and unzip it anywhere. The programs aren't code-signed yet, so Windows may
warn that they come from an unknown publisher: click **More info**, then
**Run anyway**. Or build it from source as [BUILDING.md](BUILDING.md)
describes: `bash tools/package.sh` stages the same files in
`build/dist/LongAfterDark/`. The zip's folder holds:

- **LongAfterDark.scr**: the screen saver and its settings window.
- **adhostwin.exe**: the emulator. The screen saver starts one for each
  monitor.
- **adimport.exe**: copies the After Dark modules from your discs.
- **README.txt**: a short version of this page.
- **LICENSE.txt** and the **licenses** folder: this project's licence and
  those of the code built into the programs.

Keep the three programs in one folder: the screen saver looks for the other
two next to itself. No After Dark files are included. You import them from
your own copy (and are responsible for sourcing them legally).

## 1. Import your After Dark releases

Double-click `adimport.exe`, or open the screen saver's settings and click
**Import…**. Then pick a source:

- **A disc or floppy image:** `.iso`, `.bin`, `.img`, `.ima`, `.vfd` or
  `.flp`, or a `.zip` of the install files. If you have the two Simpsons
  floppies as separate images, select both.
- **A drive or folder:** the CD itself, or a folder copied from it.
- **A download from the Internet Archive:** a list of the five releases
  with their sizes, plus one entry that fetches every release not imported
  yet. An interrupted download resumes, and each file is checked against its
  published MD5 before it is used.

The importer works out which release it was given, checks every file against
that release's known MD5s, and installs it beside the releases already
imported, which it leaves untouched. Import as many as you like.

From a command prompt, with the ids from the table above:

```
adimport --image "C:\Images\After Dark 3.2.iso"
adimport --image disk1.img --image disk2.img
adimport --from E:\
adimport --download ad10
adimport --download all
adimport --list-packages
adimport --remove tt
```

`--list-packages` shows which releases are imported, and `--remove <id>`
deletes one. `adimport --help` lists every option. The exit code is 0 on
success, 1 on an error such as a file that cannot be written, 2 when the
source is not a known release, 3 when verification fails, 4 on a network
error and 5 when you cancel.

## 2. Covers

With two or more releases imported, the settings window shows their box
covers above the module list. Click covers to list only the modules of
those releases (with none selected, it lists them all); right-click a cover
for **Show only …** and **Show all releases**. An import fetches the
release's cover picture from the Internet (checked against its published
MD5) or uses the art on the disc; a cover it cannot get is drawn as a plain
box with the release's title, and the import still succeeds. While a
release still shows such a plain cover, **Get the covers** (in the settings
window or the importer) fetches the pictures; from a command prompt,
`adimport --refresh-covers` does the same.

To use a picture of your own, right-click a cover → **Change cover…** (or
**Change cover…** next to the release in the importer), or run
`adimport --set-cover <id> <picture>`. Any picture Windows can read will do
(PNG, JPEG, GIF, BMP, TIFF); it stays on this computer.
`adimport --clear-cover <id>` goes back to the original cover.

## 3. Install the screen saver

- **For yourself:** right-click `LongAfterDark.scr` → **Install**. Windows
  makes it the current screen saver where it is and opens Screen Saver
  Settings, so leave the folder where it is.
- **For every user:** copy `LongAfterDark.scr`, `adhostwin.exe` and
  `adimport.exe` to `C:\Windows\System32`, then choose **Long After Dark** in
  Screen Saver Settings (Settings → Personalization → Lock screen → Screen
  saver).

Right-click → **Test** runs it full screen at once. Double-clicking the `.scr`
does the same: that is what Windows does with screen savers.

## 4. Choose what it shows

**Settings…** in Screen Saver Settings (or right-click `LongAfterDark.scr` →
**Configure**) opens the settings window. Pick one module or **Random** and
the modules it rotates through, how often it changes, the resolution, the
monitors to use and the sound. A live preview shows the selected module with
its options. With two or more releases imported, click their covers to list
only those releases (§2).

Some modules have options of their own, shown as buttons among the module's
settings: Fish World's **Select Fish…**, the messages of the message
modules, Art Critic's **Pictures** and others. A button opens the module's
own window, as the original control panels did. What you set there is saved
by the module at once, so the settings window's **Cancel** does not undo it.

**Sound** is on by default. Only the primary monitor's screen saver plays
it, at After Dark's own volume (50): the modules' wave effects, their MIDI
music (through Windows' MIDI synthesizer, normally the Microsoft GS
Wavetable Synth) and the Simpsons' speech. In the settings window,
**Sound** (Primary monitor / Off) and **Volume** (0–100) change that.
**Preview** plays sound with the values you have not saved yet; the small
live preview never does.

## Ending it, and playing

Any key except Shift, Ctrl, Caps Lock and Num Lock, a click, the mouse
wheel, moving the mouse or switching away (the Windows key, Alt+Tab,
Ctrl+Alt+Del) ends the screen saver. Caps Lock never does: in some modules
it changes something (it scares the fish, changes the colours) or starts a
game, as in Rodger Dodger, You Bet Your Head, Simpsons Trivia, Mime Hunt,
Frankenscreen and Marbles.

While a game is playing, keys, clicks and the mouse belong to it, and the
pointer stays on the primary monitor. Press Caps Lock again to stop playing
(the next key or move then ends the screen saver), or press Alt to end it at
once. Locking the computer (Win+L) ends the screen saver whether a game is
playing or not. Only the primary monitor plays; the others keep running on
their own.

## Where your files are

Everything is in `%LOCALAPPDATA%\LongAfterDark` (paste that into Explorer's
address bar):

| Folder or file | What it holds |
|---|---|
| `assets\win\` | the imported modules, one folder per release, the module list `catalog-win.json`, and the releases' box covers (`covers\`) |
| `downloads\` | Internet Archive downloads, reused if you import the same release again |
| `settings.ini` | the screen saver's settings |
| `state\` | what the modules save themselves (message texts, chosen pictures, high scores), per release |
| `thumbs\` | the settings window's module pictures |
| `logs\saver-last.log` | how the last screen saver run went and why it ended |

## Updating

Close the settings window and make sure the screen saver is not running
(Windows locks running programs), then replace the three programs with the
new versions. Imported releases, downloads and settings are kept.
`adimport --version` says which version you have, and so does each
program's Properties → Details in Explorer.

## Not done yet

- **Speed.** Each module's pace follows a model of a mid-1990s PC (Swirling
  Magic's too-fast pace is fixed); not every module has been compared with
  the original yet.
- No installer or code signing yet.
