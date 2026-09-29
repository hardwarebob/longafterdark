# Long After Dark on Linux

Long After Dark runs the original 1990s Berkeley Systems After Dark screen savers
(After Dark 4.0 Deluxe, After Dark 3.2, 10th Anniversary, Totally Twisted, and The Simpsons)
on Linux.

On Linux, Long After Dark combines:
1. **`longafterdark`**: A native Linux X11 runner and screen saver that renders P8 frames at smooth 60 fps, handles windowed/full-screen presentation, forwards input, and integrates seamlessly with `xscreensaver`.
2. **`adhostwin.exe`**: The x86 emulator engine running under 64-bit Wine, executing the original 16-bit and 32-bit module binaries.
3. **`adimport.exe`**: The asset importer running under Wine to download and extract releases from Internet Archive, CD ISOs, or install disks.

---

## Quick Start

### 1. Requirements

Install Wine and X11 libraries:

```bash
# Ubuntu / Debian
sudo apt update
sudo apt install wine64 libx11-6 libxext6

# Fedora
sudo dnf install wine libX11 libXext

# Arch Linux
sudo pacman -S wine libx11 libxext
```

### 2. Building from Source

The build uses a portable `llvm-mingw` toolchain and builds `zlib` and `phosg` automatically:

```bash
bash tools/bootstrap.sh    # fetches portable toolchains into third_party/
bash tools/package.sh      # builds the suite into build/dist/LongAfterDark/
```

The distribution will be in `build/dist/LongAfterDark/`:
- `longafterdark`: The native Linux player and screensaver
- `adhostwin.exe`: The emulated After Dark module host
- `adimport.exe`: The release importer
- `LongAfterDark.scr`: The Windows screensaver

### 3. Importing After Dark Modules

Import any of the 5 supported releases using `adimport` via the runner:

```bash
# Download and import The Simpsons Screen Saver (2.6 MB download)
./longafterdark --import --download simpsons

# Or import After Dark 4.0 Deluxe (Flying Toasters!)
./longafterdark --import --download deluxe

# Or import from an ISO or CD image
./longafterdark --import --image /path/to/afterdark.iso

# Or open the graphical importer
./longafterdark --import --gui
```

To view all imported modules:
```bash
./longafterdark --list
```

### 4. Running Screen Savers & Modules

```bash
# Run a random module full-screen (screensaver mode)
./longafterdark

# Run in a 640x480 window
./longafterdark -w

# Run in a 2x scaled window (1280x960)
./longafterdark -w -s 2

# Run a specific module by name or ID
./longafterdark toasters
./longafterdark chalkbrd
./longafterdark burns

# Cycle through random modules every 2 minutes
./longafterdark -r --cycle 120
```

---

## Controls & Interactive Games

- **Screen Saver Mode**: Moving the mouse or pressing Esc, 'q', or Space exits the screensaver.
- **Caps Lock starts games**: In modules with built-in games (Rodger Dodger, Lunatic Fringe, Simpsons Trivia, You Bet Your Head, etc.), press **Caps Lock** to take control and start playing.
- **Game Controls**: Arrow keys, Space, Return, letters, numbers, and mouse are routed into the emulated module.
- **Exiting Games**: Press **Caps Lock** again to resume the screensaver, or press **Alt** (or Esc) to exit immediately.

---

## XScreenSaver Integration

`longafterdark` fully supports XScreenSaver's `-window-id` protocol and draws directly into XScreenSaver windows.

1. Copy the `longafterdark` binary to your `$PATH` (e.g. `/usr/local/bin/longafterdark`).
2. Copy `longafterdark.xml` to `/usr/share/xscreensaver/config/longafterdark.xml` (or `~/.xscreensaver`).
3. Add `longafterdark -root \n\` to the `programs:` list in `~/.xscreensaver`.

Now you can select and preview Long After Dark directly in `xscreensaver-settings`!
