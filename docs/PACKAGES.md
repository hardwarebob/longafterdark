# Long After Dark — packages

How the host imports and runs modules from five After Dark releases, not
just the After Dark 4.0 Deluxe CD. DESIGN.md §6, §6a and §7 give the
contract in brief; this file is the full specification and the plan that
implements it.

**Status: implemented.** The importer reads all five releases, and all 202
catalog modules run headless, deterministically; each release also runs on
its own. This file keeps the specification and the plan as they were
written, so tables that say "today" or "current" describe the host before
this work. Where the implementation went further, the component READMEs are
authoritative:
`importer/README.md` (every release can also be downloaded from the
Internet Archive, and a ZIP of install files is a valid source), and
`host/win32/README.md` and `host/win16/README.md`.

Everything below about the four new discs comes from four surveys (Sept
2026). Their artifacts are in `research/win/pkg/<id>/`, which is gitignored:
extraction scripts, inventories, trial runs, contact sheets and disassembly.
**No After Dark bytes, extracted files or disassembly are ever committed.**
Manifests hold only path, size and md5. The ZIP password is derived at import
time, never stored (§8.4).

## 0. Decisions at a glance

| Question | Decision |
|---|---|
| What is a package | One release in a built-in registry: id, titles, known images, fingerprints, recipe, manifest (§2) |
| Where it goes | Deluxe keeps `<win>\FILES\…` unchanged. Every other package goes in `<win>\packages\<id>\` and owns only that directory (§4) |
| Import semantics | Stage and swap only that package's directory. Importing or re-importing one package never touches another. One `import.json` per package, inside its directory (Deluxe keeps `<win>\import.json`). There is no second index: the merged view is the catalog (§5) |
| Catalog ids | Deluxe keeps `ad40.*` / `classic.*`. Others use `<package>.<lower-case file stem>` (§6) |
| Duplicates | Keep all, one entry per package. Later duplicate names get ` (<short title>)`. `sameAs` links byte-identical modules (§6) |
| Lane → package | By path only: package root = parent of the module's folder. Engine dir = `<root>\ENGINE`. Packaged when the root's parent is named `packages`. No env var, no descriptor file (§7.1) |
| Cross-package files | Never, for packaged modules: search the module dir, then the engine dir. Deluxe and lone modules keep today's order (§7.1) |
| 16-bit host for AD 3.x | Real OLDMOD16 where the package ships it (`deluxe`, `ad10`). Otherwise a host-native AD3 bridge that implements OLDMOD16's verified behaviour. It uses the package's own AD_SND 3.x and ADTASK palettes. Deluxe is never needed (§7.4) |
| Install-time fix-ups | A baked table per package: file copies under the names the modules open (§4.3). No INF or IS-script interpreter |
| Formats to implement | FAT12/16 image reader; PKZIP with traditional PKWARE (ZipCrypto) decryption and raw inflate (zlib). The existing ISO-9660/Joliet reader already handles all three hybrid CDs (§8) |
| Work split | A: importer (`importer/**`). B: pe32 lane + core. C: ne16 lane + win16 + cpu. File ownership is disjoint (§10) |

## 1. Scope and starting point

Only the Windows half of each disc is in scope. What the Mac halves of the
hybrid discs hold is noted in §12; the importer skips them.

| Package | Modules | Packaged host at survey time | Blockers at survey time |
|---|---|---|---|
| `deluxe` | 84 (23 pe32 + 61 ne16) | all run | — |
| `ad10` | 46 (17 pe32 + 29 ne16): 45 in `AD10TH` + `ENGINE\STARRYNI.AD` | 44 exit 0 | the importer rejects the disc; `CHAM` (ne16 EnumWindows); `HALLOFFA` (pe32 missing stdcall signatures) |
| `ad32` | 44 ne16 | 42 exit 0, **only with Deluxe's ENGINE** | no ZIP support; no OLDMOD16 in the package; `LOGO`, `RAY` (INI seeds); `GUTS` (unimplemented APIs) |
| `tt` | 13 ne16 | 12 exit 0, **only with Deluxe's ENGINE** | no ZIP support; no OLDMOD16; `CHAM` |
| `simpsons` | 15 ne16 (the survey prose says 14; its own table, the disc and AD10's `PREVIOUS.INF` list 15) | 12 exit 0, **only with Deluxe's ENGINE**; `SIMPCLOK` renders black | no FAT or ZIP support; no OLDMOD16; `HOMEREAT`, `INS` (desktop icons) |

In total, 202 catalog entries over the five packages.

## 2. The package registry

A table compiled into `adw_import` (`packages.h/.cc`). Registry order is
the catalog order and the precedence order for name disambiguation:

| id | title | shortTitle | recipe | root | module dirs (catalog scan order) |
|---|---|---|---|---|---|
| `deluxe` | After Dark 4.0 Deluxe | Deluxe | `tree` | `FILES` | `AD40`, `ENGINE` (STARRYNI only, as today), `CLASSIC` |
| `ad10` | After Dark 10th Anniversary | 10th Anniversary | `tree` | `packages/ad10` | `AD10TH`, `ENGINE` |
| `ad32` | After Dark 3.2 | After Dark 3.2 | `ad3zip` | `packages/ad32` | `AD32` |
| `tt` | Totally Twisted After Dark | Totally Twisted | `ad3zip` | `packages/tt` | `TWISTED` |
| `simpsons` | The Simpsons Screen Saver | Simpsons | `ad3zip` | `packages/simpsons` | `SIMPSONS` |

Ids match `[a-z0-9]+`. `ad40` and `classic` are reserved: they are Deluxe's
legacy id prefixes.

Known images (identification + `verified: image`):

| id | image md5 | size (B) | medium | volume id |
|---|---|---|---|---|
| `deluxe` | `d875a60338b73f44b7befa06bdd33aeb` | 400,234,496 | ISO-9660, hybrid | `AD_DELUXE` |
| `ad10` | `a8d088415d391ce0d3199a831e1b2ad6` | 150,228,992 | ISO-9660 + Joliet, Apple partition map hybrid | `AD10TH` |
| `ad32` | `8b8be6977375fbf4d54146b9d505aa1c` | 61,693,952 | ISO-9660 level 1, no Joliet, APM hybrid | `ADW320_C` |
| `tt` | `541b9cfd744c377263a4ab1a144bdc12` | 39,784,448 | ISO-9660 level 1, no Joliet, APM hybrid | `TTW320CD` |
| `simpsons` | `7674adbda6fc5402d4e7a78af14880cc` | 2,949,120 | FAT12, 2.88 MB, both install floppies merged (WinImage) | — |

Other registry fields:

* `required` (relative to the package root; import fails with 2 without them):
  * `ad10`: `AD10TH/ADXPL510.DLL`, `AD10TH/ADXPL300.DLL`, `AD10TH/ADXPL40.DLL`, `ENGINE/OLDMOD16.DLL`, `ENGINE/AD_SND.DLL`, `ENGINE/AFTERDAR.SCR`
  * `ad32`: `AD32/ADXPL300.DLL`, `ENGINE/AD_SND.DLL`, `ENGINE/ADTASK.DLL`
  * `tt`: `TWISTED/ADXPL40.DLL`, `TWISTED/TT_SND.DLL`, `ENGINE/AD_SND.DLL`, `ENGINE/ADTASK.DLL`
  * `simpsons`: `SIMPSONS/ADXPL310.DLL`, `SIMPSONS/SIMP_SND.DLL`, `ENGINE/AD_SND.DLL`, `ENGINE/ADTASK.DLL`, plus all 15 module ZIPs (so a split-floppy source is complete)
* `ad3zip` parameters: `moduleDir` (`AD32` / `TWISTED` / `SIMPSONS`); `engineDll`, the member of `MODMISC.ZIP` that identifies the package (`ADXPL300.DLL` / `ADXPL40.DLL` / `ADXPL310.DLL`); `folderAfi`, the `AFI.ZIP` member that becomes `FOLDER.AFI` (`AD3.AFI` / `PHLEM.AFI` / `SAX.AFI`).
* `fixups` (§4.3), `nameOverrides` (§6), `manifest` (`known_files_<id>.inc`, generated like today's `known_files.inc`), `downloadUrl` (Deluxe only, as planned; the importer now lists Internet Archive copies for every package, `importer/README.md` "Downloads").

## 3. Identification

A source is a file (an image), a folder, or several images (split floppies).
Every source is read through one `SourceFs` view: an ISO-9660/Joliet image,
a FAT12/16 image, or a host folder. Names are matched case-insensitively and
reported as 8.3 upper case.

1. **Image md5.** The md5 is computed for every image source anyway, for
   `verified`. A match names the package, and that package's fingerprint must
   then also match. If it does not, the source is invalid (2).
2. **Fingerprints**, tried for every package in registry order (for folders,
   and for images with an unknown md5). Exactly one package must match: none
   → 2 ("not a known After Dark disc; known: …"), several → 2 (ambiguous).
   * `deluxe`: a `FILES` dir (`ADE\FILES`, `FILES`, or the root) holding `AD40\` and `ENGINE\`. This is today's rule, unchanged.
   * `ad10`: a `FILES` dir (same three places) holding `AD10TH\` (with `ADXPL40.DLL`) and `ENGINE\`, and no `AD40\`.
   * `ad3zip` family: an install dir (`INSTALL\`, or the root for floppies and floppy copies) holding `INSTALL.INS`, `SETUP.PKG`, `ENGINE.ZIP` and `MODMISC.ZIP`. The package is the one whose `engineDll` is a member of `MODMISC.ZIP`. Central-directory names are not encrypted, so no password is needed to identify.
3. `--package <id>` restricts step 2 to one package. It is an error if the
   source is not that package.

The volume id is recorded in `import.json` and logged. It never decides
identification alone.

## 4. On-disk layout and the importer's guarantees

This is the contract between the importer (A) and the lanes (B, C).

### 4.1 Layout

```
<win>\FILES\{AD40,CLASSIC,ENGINE,AFI}\…      deluxe (unchanged)
<win>\import.json                             deluxe record (v1, unchanged)
<win>\packages\ad10\AD10TH\…                  45 modules (pe32 + ne16), engines ADXPL510 5.2 / ADXPL300 / ADXPL40,
                                                helpers, data, BITMAPS\ MUSIC\ PICTURES\ TRACES\, fix-ups (§4.3)
<win>\packages\ad10\ENGINE\…                  OLDMOD16, OLDMOD32, AD_SND 4.0, AFTERDAR.SCR/EXE/ANI, ADPAGE.*, STARRYNI.AD, …
<win>\packages\ad10\AFI\…                     7 folder-info DLLs (as Deluxe's AFI)
<win>\packages\ad32\AD32\…                    44 modules, ADXPL300 + helpers, AD_RSRC, data, BITMAPS\ TRACES\ SOUNDS\ MUSIC\
<win>\packages\ad32\ENGINE\…                  AD_SND 3.2, ADTASK 3.0.12, ADW30.EXE, ADW30.INI, ECOLOGIC.DLL
<win>\packages\tt\TWISTED\…                   13 modules, ADXPL40, TT_SND, AD_RSRC, MUSIC\
<win>\packages\tt\ENGINE\…                    AD_SND 3.2, ADTASK 3.0.12, ADW30.EXE, ADW30.INI, ECOLOGIC.DLL
<win>\packages\simpsons\SIMPSONS\…            15 modules, ADXPL310, SIMP_SND, AD_RSRC, MUSIC\
<win>\packages\simpsons\ENGINE\…              AD_SND 3.0.3, ADTASK 3.0.8, ADW30.EXE, ADW30.INI, ECOLOGIC.DLL
<win>\packages\<id>\import.json               record (v2, §5.3)
<win>\catalog-win.json                        merged catalog (§6)
```

Names are 8.3 upper case, as the sources list them. The only exceptions
are the long-name aliases of §4.3.

### 4.2 Invariants

The importer checks these after staging. A violation fails the import with
2, and the tests check them over real imports. They apply to every package
except Deluxe, whose layout stays as it is. A "module folder" here is one of
the package's module dirs other than `ENGINE`. `ENGINE` may hold a
self-contained module that imports nothing, such as `ad10`'s
`STARRYNI.AD`.

* **I1.** No module folder holds `AD_SND.DLL`, `OLDMOD16.DLL`,
  `OLDMOD32.DLL`, `ADTASK.DLL` or `ADW30.EXE`. Verified in all three AD 3.x
  surveys: an older AD_SND beside the modules is found first and OLDMOD16
  refuses it ("AD_SND.DLL is too old").
* **I2.** For every module, each non-system DLL it imports (its catalog
  `needs`) except `AD_SND` is in its own module folder.
* **I3.** `ENGINE\AD_SND.DLL` exists. So does either
  `ENGINE\OLDMOD16.DLL` + `ENGINE\AFTERDAR.SCR` (AD 4 generation) or
  `ENGINE\ADTASK.DLL` (AD 3 generation).
* **I4.** Sound databases a module names (`TT_SND.DLL`, `SIMP_SND.DLL`) sit
  in the module folder, because modules open them as
  `<AD Data Files>\X.DLL` = `C:\AFTERDRK`. MIDI goes in `MUSIC\` beneath it,
  because modules open `MUSIC\X.MID` relative to `C:\AFTERDRK`.
* **I5.** The importer reads only the files its recipe names. On the Simpsons
  floppy, `CEREAL.TXT` and `SERIAL.TXT` (the original owner's notes;
  `SERIAL.TXT` holds a serial number) are never opened, copied, hashed or
  listed.

### 4.3 Recipes

**`tree`** (`deluxe`, `ad10`): copy `<FILES>\<dir>\**` for the package's
copy dirs, byte for byte, to `<root>\<dir>\**`, then apply the fix-ups.

* `deluxe`: dirs `AD40`, `CLASSIC`, `ENGINE`, `AFI` → `FILES\`. Exactly
  today's behaviour, including the atomic swap of `FILES` and the 175-file
  manifest.
* `ad10`: dirs `AD10TH`, `ENGINE`, `AFI` from `ADE\FILES` → `packages\ad10\`,
  143 files (123 + 13 + 7), 147 with the fix-ups below. `GAMES\` (9 files:
  standalone game EXEs, not savers) and `WALLPAPR\` (7)
  are skipped, as are the root folders that are not After Dark (`DirectX6`,
  `AT&T`, `Ereg`, `Web Sites`). Fix-ups: copies that reproduce what the
  original installers did, because the modules open these names. The facts
  come from `SETUP.INF` and `ENGINE\PREVIOUS.INF`, as decoded in
  `research/win/pkg/ad10/extracted/install_map.json`:

  | Create (under `packages\ad10\`) | Copy of | Why |
  |---|---|---|
  | `AD10TH\TT_SND.DLL` | `AD10TH\MUSIC\TT_SND.DLL` | ADXPL40 loads it from the AD Data Files root only. Totally Twisted's installer put it there (`PREVIOUS.INF`). Verified: the shared WAVs 30000/30008 load |
  | `AD10TH\MUSIC\Toasters2k.mid` | `AD10TH\TOASTER1.MID` (Joliet `Toasters2k.mid`) | `TOASTER2.AD` opens `Music\Toasters2k.mid` |
  | `AD10TH\MUSIC\Flying Toasters.mid` | `AD10TH\TOASTERS.MID` | `TOAST2K.AD` opens it |
  | `AD10TH\MUSIC\Baby Toasters.mid` | `AD10TH\BABY.MID` | both Toasters 2k builds open it |

  The other `SETUP.INF` Music entries differ from the 8.3 files already in
  `MUSIC\` only in case, so they need nothing. The importer copies a fix-up
  only when its source matched the manifest. It records the copy as
  `"from": "alias:<source path>"`.

**`ad3zip`** (`ad32`, `tt`, `simpsons`): the Windows install is an
InstallShield 2/3 script driving encrypted PKZIP archives in the install dir
(`INSTALL\` on the CDs, the root on the floppy). The recipe reproduces the
placement the three `INSTALL.INS` scripts perform, as decoded by the surveys.
It flattens that placement into one module folder, because the lane mounts
that folder as `C:\AFTERDRK`, the AD Data Files directory, and data files
hard-code `C:\AFTERDRK\…`. The recipe does not interpret the IS script.

| Archive | Classified by | Goes to (`M` = `<root>\<moduleDir>`, `E` = `<root>\ENGINE`) |
|---|---|---|
| any ZIP with at least one `*.AD` member ("module ZIP") | content | every member → `M\` (e.g. `WMORPH.ZIP`: `WMORPH.AD` + 9 `.DAT`) |
| `MODMISC.ZIP` | name | every member except `EDITFILE.TXT` → `M\` (engine `ADXPL300/40/310.DLL`, AD 3.2 helpers `AD30RSDB`, `ADTOOL`, `READ*`, `DJPG`, `DTARGA`, `STOIKDTH`, `BITMAPS.ADC`, `MESG_AD3.DAT`, `NONSENSE.TXT`) |
| `WIN.ZIP` | name | `AD_RSRC.DLL` → `M\` (the installer put it in `C:\WINDOWS`); the rest skipped (`UNLINK.EXE`, `SPALETTE.DLL`) |
| `BITMAPS.ZIP`, `TRACES.ZIP`, `SOUNDS.ZIP` | name | → `M\BITMAPS\`, `M\TRACES\`, `M\SOUNDS\` |
| `MUSICG.ZIP` if present, else `MUSIC.ZIP` | name | `*.MID` → `M\MUSIC\`; any `*.DLL` (`TT_SND`, `SIMP_SND`) → `M\` (I4). MUSICG is the General MIDI authoring (channels 1–8). MUSIC is the Microsoft dual-format one, which a modern GS synth would double |
| `AFI.ZIP` | name | the registry's `folderAfi` → `M\FOLDER.AFI`; the rest skipped |
| `ENGINE.ZIP` | name | `AD_SND.DLL`, `ADTASK.DLL`, `ADW30.EXE`, `ADW30.INI`, `ECOLOGIC.DLL` (those present) → `E\`; the rest skipped (hooks, VxD, password drivers, setup, System IQ, CTL3D, font, Win95 shell extras) |
| `HELP`, `MULTIS`, `WINSYS`, `WAVEMIX`, `MUSIC` when MUSICG was used | name | skipped |
| any other ZIP without an `.AD` | — | skipped and logged |

Resulting file sets, checked against the survey extractions:
* `ad32`: `AD32\` has 44 `.AD`, 9 morph `.DAT`, `FOLDER.AFI`, 11 DLLs from
  MODMISC, `BITMAPS.ADC`, `MESG_AD3.DAT`, `NONSENSE.TXT`, `AD_RSRC.DLL`,
  `BITMAPS\` (3), `TRACES\` (`DIAMOND`, `ROTPYRA`), `SOUNDS\` (2), `MUSIC\`
  (8 MIDI). `ENGINE\` has 5 files.
* `tt`: `TWISTED\` has 13 `.AD`, `ADXPL40.DLL`, `TT_SND.DLL`, `AD_RSRC.DLL`,
  `FOLDER.AFI`, `MUSIC\` (4 MIDI). `ENGINE\` has 5 files.
* `simpsons`: `SIMPSONS\` has 15 `.AD`, `ADXPL310.DLL`, `SIMP_SND.DLL`,
  `AD_RSRC.DLL`, `FOLDER.AFI`, `MUSIC\` (6 MIDI, including `I&SSHOW.MID`).
  `ENGINE\` has 5 files.

### 4.4 Interim test roots (for B and C before A lands)

The lanes are built in parallel with the importer. Until A's `adimport` can
produce these layouts, B and C build them in their own build dirs by copying
from the survey extractions. The copies must match §4.1 exactly.
`AD_ASSETS_DIR=<build dir>\pkgroots\<scenario>`, with the files under
`…\win\packages\<id>\`:

* `ad10`: `research/win/pkg/ad10/extracted/FILES/{AD10TH,ENGINE,AFI}` plus the four §4.3 fix-ups.
* `ad32`: from `research/win/pkg/ad32/extracted/`:
  * `AFTERDRK/AD30/*` → `AD32\`
  * `AFTERDRK/{ADXPL300,AD30RSDB,ADTOOL,READFILE,READBMP,READGIF,READMMP,READPCX,DJPG,DTARGA,STOIKDTH}.DLL`, `BITMAPS.ADC`, `MESG_AD3.DAT`, `NONSENSE.TXT` → `AD32\`
  * `WINDOWS/AD_RSRC.DLL` → `AD32\`
  * `AFTERDRK/{BITMAPS,TRACES,SOUNDS}/` → `AD32\…`
  * `_alt/MUSICG/*` → `AD32\MUSIC\`
  * `AFTERDRK/{AD_SND.DLL,ADTASK.DLL,ADW30.EXE,ADW30.INI,ECOLOGIC.DLL}` → `ENGINE\`
* `tt`: from `research/win/pkg/tt/extracted/`:
  * `AFTERDRK/TWISTED/*.AD`, `AFTERDRK/ADXPL40.DLL`, `WINDOWS/AD_RSRC.DLL` → `TWISTED\`
  * `ALT/MUSICG/TT_SND.DLL` → `TWISTED\`
  * `ALT/MUSICG/*.MID` → `TWISTED\MUSIC\`
  * `AFTERDRK/PHLEM.AFI` → `TWISTED\FOLDER.AFI`
  * `AFTERDRK/{AD_SND.DLL,ADTASK.DLL,ADW30.EXE,ADW30.INI,ECOLOGIC.DLL}` → `ENGINE\`
* `simpsons`: from `research/win/pkg/simpsons/extracted/`:
  * `AFTERDRK/SIMPSONS/*` (15 `.AD` + `FOLDER.AFI`), `AFTERDRK/ADXPL310.DLL`, `AFTERDRK/SIMP_SND.DLL`, `WINDOWS/AD_RSRC.DLL` → `SIMPSONS\`
  * `AFTERDRK/MUSIC/*` → `SIMPSONS\MUSIC\`
  * `AFTERDRK/{AD_SND.DLL,ADTASK.DLL,ADW30.EXE,ECOLOGIC.DLL}` → `ENGINE\`
  * `WINDOWS/AFTERDRK.INI` → `ENGINE\ADW30.INI`. The survey's extraction may
    have applied the installer's INI edits, so this one file can differ from
    the ZIP member. No lane reads it.

Scenarios: one root per package with **no `FILES` at all** (standalone:
proves a package needs nothing else), and one "all" root that also holds a
copy of the installed Deluxe `FILES` tree.

## 5. Import semantics

### 5.1 One package, one directory

* `<win>\import.lock` (delete-on-close, as today) serialises imports,
  `--catalog-only` and `--remove` for the whole `win` dir.
* Deluxe: exactly today's flow. It stages `FILES.importing-<pid>`, swaps
  `FILES` with two renames and writes `import.json`. Only the catalog scan
  changes: it now also lists the installed packages.
* Other packages:
  1. Stage `<win>\packages\<id>.importing-<pid>\` from the recipe.
  2. Re-read and re-hash, check the manifest, `required` and the invariants.
  3. Write `import.json` into the stage.
  4. Render the merged catalog over the stage plus every other installed
     package to `catalog-win.json.tmp-<pid>`.
  5. Swap: `packages\<id>` → `packages\<id>.old-<pid>` (if present), stage →
     `packages\<id>`, catalog tmp → `catalog-win.json`, delete the old tree.
  Cancel is honoured up to the first rename, as today.
* An import of package P writes only `packages\<P>*`, `catalog-win.json*`
  and `import.lock`. It reads other packages only to build the catalog, and
  never modifies `FILES`, `import.json` or another package. The tests check
  this byte for byte.
* **Recovery**, at the start of every operation under the lock: for each
  `packages\<id>.old-<pid>`, put it back if `packages\<id>` is missing (the
  run died between the renames), else delete it. Delete every
  `*.importing-*` and `*.removing-*`. If anything was recovered, or a
  `catalog-win.json.tmp-*` was left behind, regenerate the catalog. Deluxe's
  existing recovery is unchanged.
* **Installed** means: Deluxe when `<win>\FILES` is a directory; a package
  when `<win>\packages\<id>\import.json` exists. Directories in `packages\`
  that are not registry ids are ignored, and logged once.
* `--remove <id>` (should): rename the root to `<root>.removing-<pid>`,
  regenerate the catalog, delete. For Deluxe, it also deletes
  `<win>\import.json`.
* `--catalog-only` rescans every installed package. It no longer requires
  `FILES`, only at least one installed package.

### 5.2 Sources, CLI and GUI

```
adimport --iso <image> | --image <image> [--image <image2> …] | --from <folder> | --download
         [--package <id>] [--dest <root>] [--gui] [--no-verify] [--quiet] [--download-dir <dir>] [--url <url> [--md5 <hex>]]
adimport --catalog-only [--dest <root>]
adimport --list-packages [--dest <root>]        registry + installed state, one line each
adimport --remove <id> [--dest <root>]          (should)
```

* `--iso` stays; it is an alias for `--image`. The image type is sniffed
  from content, never the extension: ISO-9660 (cooked or raw sectors, as
  today), else FAT12/16 (§8.2), else 2.
* Several `--image`s (split floppies) are unioned into one `SourceFs`. The
  same name with different bytes → 2. This is a *should*: the user's
  Simpsons image already holds both disks.
* `--download` was planned to stay Deluxe-only, since no other package had
  a known URL when this was written. The Internet Archive search that
  followed found copies of every package, so `--download <id>|all` now
  fetches any of them (`importer/README.md`, "Downloads").
* Exit codes are unchanged (0 ok · 1 error · 2 source invalid · 3 verify
  failed · 4 network · 5 cancelled). The settings dialog launches
  `adimport --gui` and reads only 0 and 5.
* GUI: the chooser offers "disc image (ISO or floppy image)", "drive or
  folder" and "download After Dark 4.0 Deluxe" (as planned; the download
  choice now lists every release). The file dialog filter
  includes `*.iso;*.img;*.ima;*.vfd;*.flp`. The progress window names the
  package it identified. The final message says "Imported <title>: N
  modules" and lists the installed packages. The restyle queued in WIP.md
  comes later and is out of scope here.
* `win_assets_dir(root)`, in both the importer and the host: `<root>\win`
  if it holds `FILES`, `packages` or `catalog-win.json`; else `<root>` if
  it holds one of those; else `<root>\win`.

### 5.3 `import.json` version 2 (packages other than Deluxe)

```json
{ "version": 2, "tool": "adimport 1.1", "importedUtc": "2026-09-26T18:00:00Z",
  "package": { "id": "ad32", "title": "After Dark 3.2", "recipe": "ad3zip", "root": "packages/ad32" },
  "source": { "kind": "iso", "format": "iso9660", "path": "D:\\…\\afterdark3.2.ISO",
              "imageSize": 61693952, "imageMd5": "8b8be6977375fbf4d54146b9d505aa1c", "imageMd5Known": true,
              "volumeId": "ADW320_C", "parts": [] },
  "verified": "image", "fileCount": 0, "totalBytes": 0, "missingKnown": [],
  "files": [ { "path": "packages/ad32/AD32/GUTS.AD", "size": 0, "md5": "…", "known": "match",
               "from": "INSTALL/GUTS.ZIP!GUTS.AD" } ] }
```

* `kind` is `iso`, `floppy`, `folder` or `download`.
* `format` is `iso9660`, `iso9660+joliet`, `fat12`, `fat16` or `folder`.
* `parts` lists every image of a multi-image source (path, size, md5).
* `from` is the source path; `zip!member` for an archive member; or
  `alias:<package path>` for a fix-up copy.
* `verified` means what it means today: `image`, `files`, `partial` or
  `none`. The manifest covers the installed files. Every ZIP member's CRC-32
  is always checked.
* Deluxe's `<win>\import.json` stays version 1, unchanged.

## 6. The merged catalog

`catalog-win.json` keeps `"version": 1`, with only additive changes. The
generator becomes `adimport 1.1` (`adimport 1.2` since the covers work,
COVERS.md §2.7).

* **Order.** Installed packages in registry order. Within a package, its
  module dirs in registry order. Within a dir, `*.AD` sorted as today.
  Deluxe's order is exactly today's: `AD40`, `ENGINE\STARRYNI.AD`,
  `CLASSIC`. For every other package, `ENGINE\*.AD` is scanned too, which
  gives `ad10.starryni`.
* **Lane** comes from the header (PE32 → `pe32`, NE → `ne16`), never the
  folder. `AD10TH` mixes 16 pe32 and 29 ne16 modules.
* **id**: Deluxe keeps `ad40.<base>` (pe32) / `classic.<base>` (ne16). For
  every other package it is `<package>.<base>`, where `<base>` is the
  lower-case file stem. A second file with an id already taken is skipped
  and logged, as today. This does not happen in the corpus.
* **moduleName**: the existing display-name rule (importer README: PE
  `VERSIONINFO` FileDescription, NE resource 2000/20, then STRINGLIST 128,
  then the file name), trimmed at both ends. Simpsons has
  `"Grampa's Wisdom "` and `"Snowball I "`. Then `nameOverrides` apply. Only one exists:
  `ad10` `TOAST2K.AD` → `Toasters 2k (early build)`. It is an earlier build
  that `SETUP.INF` never installs, and without the override it would share
  the name of the installed `TOASTER2.AD`.
* **displayName**: unique within a lane, case-insensitively. Walk the
  catalog in order. The first module with a given `(lane, moduleName)`
  keeps `moduleName`. Every later one gets `moduleName + " (" + shortTitle +
  ")"`, and, if that is still taken, `moduleName + " (" + shortTitle + ", "
  + FILE + ")"`. Examples:
  * `classic.toilets` "Flying Toilets"
  * `ad10.toilet` "Flying Toilets (10th Anniversary)"
  * `ad32.toilet` "Flying Toilets (After Dark 3.2)"
  * `tt.toilet` "Flying Toilets (Totally Twisted)"

  Deluxe's 84 names are already trimmed and unique within their lanes
  (checked), so none of them changes. Names depend on which packages are
  installed. Ids never do.
* **New per-module fields**, on every entry, Deluxe's too, appended after
  the existing ones:
  * `package`, `packageTitle`, `moduleName`
  * `md5` (of the module file)
  * `sameAs` (only when set): the id of the first entry in catalog order
    with the same md5. For example, 30 of `ad10`'s and 36 of `ad32`'s
    modules point at Deluxe entries. A front-end may use it to collapse
    duplicates or to skip them in Random. Today's front-end ignores it.
* **Top-level `packages`** (between `generator` and `modules`): `[{ "id",
  "title", "shortTitle", "root", "verified", "importedUtc", "modules",
  "released" }]` for the installed packages, oldest release first
  (`released` is the registry's `YYYY-MM[-DD]`; a package without one
  comes last, in registry order). The front-ends show releases in this
  order.
* The existing fields of every Deluxe entry keep their values, so
  `import.catalog_real` still compares them to the prototype. The test must
  now ignore the new fields.
* The front-end of the time needed no change. It read
  `id`/`displayName`/`lane`/`path`/`controls`/`about`/`credits`, resolved
  `path` under the win dir (any depth), grouped by lane, and passed the
  absolute path to `adhostwin`. A later front-end can group by `package`
  and show `moduleName` under a package header.

## 7. How the lanes find a module's package

### 7.1 The rule (both lanes)

```
module dir   = folder of the module file (the absolute path adhostwin was given, after its usual resolution)
package root = parent of the module dir
engine dir   = <package root>\ENGINE
packaged     = the package root's parent directory is named "packages" (case-insensitive)
package id   = name of the package root (logs only; lanes do not branch on it)
```

There is no environment variable and no descriptor file. The layout is the
contract. For a **packaged** module, the lane resolves nothing outside its
package root. Everything else is *legacy*: Deluxe's `FILES\…`, a module
under some other folder, or a research root laid out as `FILES\<X>\`.
Legacy modules keep exactly today's behaviour, so the 84 Deluxe
`FBHASH` streams cannot change. With `ADTRACE=lane`, each lane logs one
line at init with the package id (or `legacy`), module dir, engine dir,
and, for ne16, the bridge, the AD_SND path and the palette source.

### 7.2 pe32

| | DLL search order |
|---|---|
| packaged | module dir → engine dir |
| legacy | module dir → `<win>\FILES\AD40` (today) |

`ad10`'s `ADXPL510.DLL` (5.2.0.1, a later build than Deluxe's 5.1.0.1, with
the same 1171 exports) sits beside its modules. The survey found that the
13 modules shared with Deluxe give byte-identical 300-frame streams under
either build. The `C:\PICTURES` mount (the module dir's `PICTURES\`) works
unchanged. Long MIDI names come from the importer's fix-ups (§4.3), not
from a VFS alias table.

### 7.3 ne16

| | DLL search order | `C:\WINDOWS\SYSTEM` mount | bridge (§7.4) | palettes |
|---|---|---|---|---|
| packaged, engine dir holds `OLDMOD16.DLL` (`ad10`) | module dir → engine dir | engine dir | real `OLDMOD16` | `ENGINE\AFTERDAR.SCR` `AD_PALETTE` 101..104 |
| packaged, no `OLDMOD16.DLL` (`ad32`, `tt`, `simpsons`) | module dir → engine dir | engine dir | native AD3 bridge with `ENGINE\AD_SND.DLL` | `ENGINE\ADTASK.DLL` 5000/1..4, else `ENGINE\AFTERDAR.SCR` |
| legacy | today: module dir → `FILES\CLASSIC` → `FILES\ENGINE` (or beside the module) | today | real `OLDMOD16` | today |

* `ADNE16BRIDGE=auto|oldmod16|native` overrides the choice. `auto` is the
  default. `native` on a Deluxe Classic module is the oracle that tests the
  bridge (§9).
* **AD_SND guard.** When the real OLDMOD16 runs and the module dir holds an
  `AD_SND.DLL` (a layout the importer never produces), the lane loads the
  engine dir's copy first. The Win16 `LoadLibrary("ad_snd.dll")` then
  resolves by module name to the already-loaded instance, and the lane logs
  it. Normal layouts are unaffected.
* Guest disk: the module dir stays `C:\AFTERDRK`, the AD Data Files
  directory (also mounted as `C:\AFTERD~1`). So `TT_SND.DLL`,
  `SIMP_SND.DLL`, `BITMAPS.ADC`, `MUSIC\` and `TRACES\` resolve.
* `MODULES.INI` seeds become per module dir:
  * `[Ray] RaySceneFile` = the first of `TRACES\ROTCUBE.TRC`,
    `DIAMOND.TRC`, `ROTPYRA.TRC` that exists. Deluxe keeps `ROTCUBE`; AD 3.2
    ships the other two.
  * New: `[Logo Section] LogoFile=C:\AFTERDRK\BITMAPS\ADLOGO.BMP`. AD 3.2's
    `LOGO.AD` refuses to start without it.
* **Synthetic desktop**, for ADXPL310's and ADXPL40's desktop-icon
  gatherers. `CHAM`, `HOMEREAT` and `INS` fail with "Out of memory" when it
  finds 0 icons, because `GlobalAlloc(0x62, 0)` followed by `GlobalLock`
  returns NULL.
  * `EnumWindows` enumerates a fixed set of visible top-level windows: the
    saver window, plus a "Program Manager" (class `Progman`) with a class
    icon and a title. `IsWindowVisible`, `GetClassName`, `GetClassWord(GCW_HICON)`,
    `GetWindowWord(GWW_HINSTANCE)`, `GetModuleFileName`, `GetWindowText`,
    `GetWindowPlacement`, `GetWindowRect`, `GetWindow`, `CopyIcon`,
    `DestroyIcon`, `SHELL.ExtractIcon` answer consistently for those windows.
  * A seeded `C:\WINDOWS\PROGMAN.INI` `[Groups]` points at Win 3.1-style
    `.GRP` files (`PMCC` header, group-name offset at 0x16). The Simpsons
    survey verified this fix in a private build.
  * Icon images are generated or compiled into the host, never system icons
    (`IDI_*` art differs between Windows versions and would break `FBHASH`
    determinism).
  * The set must not change any Deluxe stream. The `ad10` survey verified
    that for its `EnumWindows` patch.

### 7.4 The native AD3 bridge (ne16, packages without OLDMOD16)

**Why.** The AD 3.x releases shipped their own 16-bit host (`ADW30.EXE` +
the MFC `ADTASK.DLL`). OLDMOD16 was the bridge AD 4 used to run AD 3.x
modules, so running them under it is faithful. But it exists only on the AD
4 generation discs. Depending on another disc would break "own one disc, run
it". ADTASK's exports (`LOADUPMODULE`, `RUNMODULE`, `DOMESSAGE`, …) are not
verified and would need a larger USER surface (MFC windows, messages). The
host side of the module protocol, on the other hand, is fully verified from
OLDMOD16 (ABI.md §3.3/§3.4), and every AD 3.x module tried in the surveys
ran under it. So the bridge implements OLDMOD16's five entry points in C++.
It is a replacement for OLDMOD16, of the same kind as our replacement of
`OLDMOD32` and `AFTERDAR.SCR`. It does not replace any engine or module
code.

**Interface.** The lane talks to either bridge through one interface that
mirrors ABI.md §3.2: `load(hwnd, hdc, ctrl4, volume, mute, path, err,
errId)`, `message(msg, err)`, `set_controls(volume, mute, ctrl4)`,
`set_palette(hpal, idx)`, `unload()`. The real-OLDMOD16 implementation is
today's far calls. The native one does the following, per ABI.md §3.3. For
anything that section leaves open, follow OLDMOD16's code
(`research/win/dis/OLDMOD16.DLL.asm`):

1. **Blocks.** `GlobalAlloc(GMEM_MOVEABLE|GMEM_SHARE)` `AD_SYSTEM` (0x3C)
   and `AD_MODULE` (0x30) in the guest heap, locked, as `DLLENTRYPOINT`
   does. Fill them field for field as the ABI.md §3.3 tables say:
   * `AD_SYSTEM`: +0 = 2, CPU, FPU, `HORZRES`/`VERTRES`, bits,
     aspect, logpixels, **+0x14 = 300**, +0x20 = `hADModule`, +0x22 = the
     error buffer's far pointer, +0x26 = 0, +0x2A = `RC_PALETTE`,
     **+0x2C = "BUTTHEAD"** one char per WORD.
   * `AD_MODULE`: `hDrawRgn` = `CreateRectRgn(client)`, `ptRgnSize`,
     `iControlValue[4]`, `iControlID` = 1..4, `hModule`, zeros.

   Four guest `LOGPALETTE`s (version 0x300, room for 256 entries) back
   `lpLogPalette`.
2. **Load**:
   1. `LoadLibrary(<engine dir>\AD_SND.DLL)` by full path. Resolve
      `adwSoundInit`, `adwSoundCleanup`, `adwGetSystemVolumes`,
      `adwSetSystemVolumes`, `adwSetVolume`, `adwSetSoundMute` and
      `adwStopSound` case-insensitively. All seven exist in AD_SND 3.0.3
      and 3.2 (checked). Load failure is error id 1 and a missing entry is
      id 3. There is **no `VerStr` gate**.
   2. Call `adwSoundInit(0, buf)` and `adwGetSystemVolumes(&saved)`.
   3. `LoadLibrary(module)`, then `GetProcAddress("MODULE")`.
   4. `SaveDC`, then fill the blocks.
   5. `MODULE(5)`. On 0: re-copy the controls; if `bWantSnd`, call
      `adwSetSoundMute` and `adwSetVolume`. Then `MODULE(12)` (result
      ignored), then `MODULE(0)` (0x0E counts as OK), then, if OK,
      `MODULE(1)`.
   6. On failure, unload. On success, copy back an error text the module
      substituted at +0x22.
3. **Message**:
   1. Point +0x22 at `err`, then `r = MODULE(msg, hdc, hADSystem)`.
   2. Palette requests 10/11/12/13 select `hpal[1]/[3]/[0]/[2]`, set
      `hPalette`/`lpLogPalette`, realize, and return 0. They return 7 when
      that palette is missing.
   3. `r == 3` (RESTART) → `MODULE(0)` + `MODULE(1)`.
   4. Copy back a replaced error string. `r == 1` clears `err`.
   5. 0x0E, 0x11 and 0x12 pass through to the lane as today.
4. **Controls**: the sound calls when `bWantSnd` and the values changed;
   then copy the values into `AD_MODULE+6`.
5. **Palettes.** `set_palette(hpal, idx)` copies the entries into
   `LOGPALETTE[idx]`. The lane feeds it four palettes: `hpal[i]` =
   `AD_PALETTE 101+i` when they come from `AFTERDAR.SCR`. When they come
   from `ADTASK.DLL`, `hpal[0..3]` = 5000/3, 5000/1, 5000/4, 5000/2, so
   palette request `10+k` selects 5000/(k+1). The three ADTASK builds have
   byte-identical 5000/1..4: `ad32`'s and `tt`'s 3.0.12 and `simpsons`'
   3.0.8, checked by md5 while writing this. The `ad32` survey verified
   them equal to `AFTERDAR.SCR` `AD_PALETTE` 102/104/101/103. None of these
   palettes is ever embedded in the host: they are After Dark data.
6. **Unload**:
   1. `MODULE(3)` if initialized.
   2. `RestoreDC`, `FreeLibrary(module)`, `DeleteObject(hDrawRgn)`.
   3. `adwStopSound`, `adwSetSystemVolumes(saved)`, `adwSoundCleanup`,
      `FreeLibrary(AD_SND)`.
   4. Free both blocks.
7. **Machinery.** Every guest call goes through `Runtime16::call_far`, so
   faults, `Catch`/`Throw`, virtual time and the census behave as for
   OLDMOD16.

The only side-effect differences from OLDMOD16 are OLDMOD16's own
instructions, API calls and memory. These nudge virtual time through the
instruction-cost model (`ADMIPS`/`ADAPICOST`) and shift selector numbers. So
the oracle compares both bridges with `ADMIPS=0` (§9).

## 8. Extraction formats (importer)

### 8.1 ISO-9660 / Joliet: already implemented

`iso9660.cc` reads all three hybrid CDs. The PVD is at 2048-byte sector 16
whatever the Apple driver descriptor and partition map put at the start of
the image. It reads `ad32` and `tt` as
level 1 without Joliet (both were walked by today's `adimport` before it
rejected them). It reads `ad10` with Joliet, pairing each Joliet entry with
its 8.3 twin by extent. The Apple HFS partitions are ignored, as they are
for Deluxe.

### 8.2 FAT12/FAT16 floppy images (new: `fat.h/.cc`)

* Boot sector BPB:
  * bytes/sector must be 512, 1024, 2048 or 4096;
  * sectors/cluster must be a power of two;
  * reserved sectors ≥ 1;
  * 1–2 FATs;
  * root entries, total sectors (16- or 32-bit field), sectors/FAT and
    media byte are read.

  Reject: the `55 AA` signature missing where the BPB demands it, an image
  shorter than total sectors × sector size, or a zero field.
* FAT12 when clusters < 4085, FAT16 when < 65525, else reject (FAT32 is
  not needed). Decode 12-bit entries with odd/even nibble packing. Chains
  end at ≥ 0xFF8 (0xFFF8). A chain loop, a free or out-of-range cluster
  inside a chain, or a chain shorter than the file size rejects the image.
* Directories: the fixed root region, then subdirectories through their
  chains. Skip deleted entries (0xE5), volume labels, LFN entries (attr
  0x0F), `.` and `..`. A lead byte of 0x05 means 0xE5. Names are 8.3,
  upper-cased, and go through `check_component` like ISO names. DOS
  date/time becomes the copy's mtime.
* The Simpsons image: 2,949,120 B = 5760 × 512, 2 sectors/cluster, 2 FATs
  × 9 sectors, 240 root entries, media 0xF0, no volume label, one flat root
  of 31 files, 2863 clusters, no fragmentation or hidden data.

### 8.3 PKZIP with traditional encryption (new: `zip.h/.cc`)

* **Structure.**
  * Find the end-of-central-directory record (`PK\5\6`) in the last
    65,557 bytes.
  * Walk the central directory (`PK\1\2`). Its sizes, CRC and flags are
    authoritative.
  * Locate the data through each local header (`PK\3\4`: 30 bytes + its own
    name/extra lengths).
  * Reject: multi-disk, ZIP64 (0xFFFF/0xFFFFFFFF markers), strong
    encryption (flag bit 6), methods other than 0 (stored) and 8 (deflate),
    names containing `/`, `\` or `:`, DOS device names (reuse
    `check_component`), and duplicate names within one archive.
  * Every entry in the corpus is a bare 8.3 name, made by 2.0 (FAT), with
    flags 0x0001 or 0x0003 (bit 0 encrypted, bit 1 maximum deflate) and bit
    3 clear.
* **Traditional PKWARE decryption ("ZipCrypto").**

  ```
  keys = 0x12345678, 0x23456789, 0x34567890
  update(c): k0 = crc32_step(k0, c); k1 = (k1 + (k0 & 0xFF)) * 134775813 + 1; k2 = crc32_step(k2, k1 >> 24)
  crc32_step(crc, b) = table[(crc ^ b) & 0xFF] ^ (crc >> 8)     (reflected 0xEDB88320, no pre/post inversion)
  stream byte: t = (k2 | 2) & 0xFFFF; ks = ((t * (t ^ 1)) >> 8) & 0xFF; plain = cipher ^ ks; update(plain)
  ```

  1. Initialise the keys with `update(b)` over each password byte.
  2. Decrypt the 12-byte header. Its last byte must equal `crc >> 24`, or
     `mod_time >> 8` when bit 3 is set, which is not needed here but costs
     nothing.
  3. The payload is `compressed size − 12` bytes.
* **Decompression:** raw deflate with zlib `inflateInit2(-15)`, streamed.
  zlib is already in `third_party/win/local`, because phosg depends on it.
  Link it into `adw_import`. Stored entries are copied. Check CRC-32 and
  the uncompressed size on every entry. A mismatch is a corrupt source
  (2), never a verify failure.
* **Streaming.** A member is read through the importer's existing
  `Planned::read(sink)`, so a ZIP-derived file is staged, hashed and
  verified like an ISO file.

### 8.4 The archive password (derived, not stored)

All three AD 3.x discs use one password. It sits in clear text in
`INSTALL.INS`, right after the string "Cannot initialize for unzip!", from
where the IS script passes it to `DUNZIP.DLL`. Do not put it in the
repository. Derive it:

1. Candidates are every run of 4–32 printable ASCII bytes in `INSTALL.INS`.
   The run right after "Cannot initialize for unzip!" comes first, then the
   rest in file order, deduplicated.
2. The check entry is the smallest encrypted member across the package's
   ZIPs. It is found in the central directory, with no password needed.
3. A candidate is accepted when the check entry's 12-byte header check
   passes **and** the whole entry decrypts, inflates and matches its CRC-32
   and size. A second entry from a different ZIP must also pass.
4. If no candidate passes, the source is invalid (2, "cannot find the
   archive password"). The password is never logged or written to
   `import.json`.
5. (could) `--zip-password <pw>` for an unknown AD 3.x collection.

### 8.5 Not needed

* SZDD/KWAJ (`_` files), CAB (MSZIP/Quantum/LZX) and InstallShield 5 CABs.
* The InstallShield 3 `.Z` archive `INS0762.LIB` (PKWARE DCL implode): it
  holds only the installer's own `DUNZIP.DLL`/`RESOURCE.DLL`.
* An `INSTALL.INS` bytecode interpreter: the placement is baked into
  `ad3zip`.
* A `SETUP.INF` parser: the `ad10` fix-ups are baked.
* HFS and StuffIt (Mac halves, §12).

## 9. Test strategy

* **Importer, synthetic (always on).** Fixture builders in
  `importer/tests/`, next to `iso_builder.h` and
  `module_builder.h`:
  * a ZIP writer (ZipCrypto with a test-only password, stored and deflate);
  * a FAT12/16 image writer;
  * an `INSTALL.INS`-like blob holding the password among decoy strings.

  With these, test:
  * ZIP: round trip, wrong password, bad CRC, truncation, zip-slip and
    device names, ZIP64 and multi-disk rejected, header check byte,
    duplicate names.
  * FAT: 1.44 MB and 2.88 MB BPBs, a fragmented chain, a subdirectory,
    skipped LFN/deleted/label entries, chain loop and short chain rejected.
  * Password derivation: preferred position, decoys, none found.
  * Identification: each package's shape as a synthetic tree, ISO and FAT
    image. Unknown and ambiguous sources, and `--package` mismatch.
  * Recipes: exact file sets per package. Invariant violations fail. Fix-ups
    apply only to matching sources.
  * Per-package atomicity: importing an `ad32`-shaped source into a root
    holding a Deluxe-shaped tree leaves `FILES` and `import.json` byte-for-byte
    unchanged. A re-import replaces only that package. A corrupt member
    leaves the previous package and catalog. Recovery from each interrupted
    swap state and from leftover staging dirs. `--catalog-only` without
    `FILES`. `--remove`.
  * Catalog merge: ids, order, the displayName rule with collisions across
    and within packages, `sameAs`, `packages`, and Deluxe entries unchanged
    apart from the new fields.
* **Importer, real images (opt-in).** `import.pkg_real` runs with
  `AD_E2E_PKG=1`; the images are in `AD_SOURCE_ISO_DIR`, default
  `<repo>\source_iso` (gitignored). It identifies images by md5,
  not file name, and skips (77) when they are absent.
  * Each image imports into a fresh scratch root: exit 0, `verified: image`,
    `missingKnown` empty, the exact §4 layout and counts, invariants hold,
    and the catalog has 46 / 44 / 13 / 15 modules with the right lanes.
  * Then all four go into one root together with a Deluxe tree (a copy of
    the installed assets, or `--from` them): 202 modules. Deluxe's existing
    fields are unchanged versus today's installed catalog.
  * A second import of each package changes nothing else in the root.
* **Lanes.**
  1. Before changing anything, capture **baselines** with the packaged
     host of the time (`build\dist\AfterDark\adhostwin.exe`; the dist is
     `build\dist\LongAfterDark` since the rename): Deluxe 23 AD4 and 61
     Classic, 120 frames `FBHASH`. After the change the streams must be
     identical.
  2. On the §4.4 interim roots: every module of the lane's packages runs
     120 frames twice → exit 0, identical streams, and the `[census]` of
     unimplemented calls is reported.
  3. Longer runs where the surveys showed late content (`VOYEUR` ≥ 900,
     `CHAM` ≥ 900, `HOMEREAT`/`INS` ≥ 1800, `HALLOFFA` ≥ 1200).
  4. Contact sheets of new modules, checked by eye.
  5. The ne16 bridge oracle: every Deluxe Classic module with
     `ADNE16BRIDGE=native` versus `oldmod16`, both with `ADMIPS=0`, 120
     frames. Streams must be identical; any exception is explained.
* **Integration** (§11): the new `adimport` output replaces the interim
  roots, and the full 202-module census runs with the combined
  `adhostwin`.

Headless runs stay deterministic throughout. Nothing may write to the
user's real data folder (`%LOCALAPPDATA%\LongAfterDark`) except the
integration step's final import.

## 10. Work packages

Three parallel packages with disjoint file ownership. Rules for all three:
* Never touch `scr` (another workflow is redesigning it) or another
  package's paths.
* Never commit, push, stash, reset, clean or check out.
* After Dark files, extractions and disassembly live only under the
  gitignored `research/win/pkg/…`, `build/…` or a scratch dir.
* No system-wide installs. Venvs under `research/win/venv` are fine.
* Build only your components, in your own `build/win-<key>` dir, with
  `tools/build.sh`.
* Report doc changes you would make outside your paths instead of making
  them.

The briefs below are the ones handed to the three agents. They are quoted
as written, so the screen saver's name is that of the time,
`AfterDark.scr`, since renamed `LongAfterDark.scr`.

### A: importer (`importer/**`)

=== A ===
**Work package A: multi-package importer.** Owns `importer/**`
only (sources, tests, `README.md`, manifests, `gen_known_files.py`).
Read first: `docs/PACKAGES.md` (all of it; §2–§6 and §8–§9 are your
spec), `DESIGN.md` §6/§6a/§7, `importer/README.md`, and the survey
artifacts in `research/win/pkg/{ad10,ad32,tt,simpsons}/` (extraction
scripts and manifests, as reference for expected results; not code to copy
into the repo).
Build: `AD_BUILD_DIR=build/win-pkg-import AD_COMPONENTS="host/loader;importer" bash tools/build.sh`.

Goals:
1. A package registry (`packages.h/.cc`) with the five packages of §2: ids,
   titles, shortTitles, known images, fingerprints, recipes, module dirs,
   `required`, fix-ups, name overrides, manifests. Deluxe's constants move
   into it, and its behaviour stays identical.
2. A `SourceFs` view over the existing ISO reader, a new FAT12/16 image
   reader (`fat.h/.cc`, §8.2) and host folders. Images are sniffed by
   content. Repeated `--image` unions split floppies (should).
3. Identification by image md5 plus fingerprints, and `--package` (§3).
   Unknown or ambiguous sources give exit 2 with a message naming the known
   products.
4. PKZIP + ZipCrypto + raw inflate via zlib (`zip.h/.cc`, §8.3), and the
   password derived from `INSTALL.INS` (§8.4). The password never appears in
   the repo, the logs or `import.json`.
5. The recipes `tree` (Deluxe unchanged; `ad10`) and `ad3zip` (`ad32`,
   `tt`, `simpsons`), producing exactly the §4.1 layout, the §4.3 file sets
   and fix-ups, and checking the §4.2 invariants. Never read `CEREAL.TXT` or
   `SERIAL.TXT`.
6. Per-package atomic import with the lock, staging, swap and recovery of
   §5.1. Deluxe's flow stays unchanged. `--remove <id>` (should).
   `--catalog-only` works without `FILES`. `win_assets_dir` accepts
   `packages` as a marker (§5.2).
7. `import.json` version 2 inside each package root (§5.3). Deluxe's stays
   version 1.
8. The merged catalog (§6): order, ids, `moduleName`, the `displayName`
   disambiguation, the `TOAST2K` override, `package`/`packageTitle`/`md5`/
   `sameAs`, and top-level `packages`.
9. Manifests `known_files_<id>.inc` for `ad10`/`ad32`/`tt`/`simpsons`,
   generated from verified imports of the real images in
   the repository's `source_iso\` (path, size, md5 only), with
   the image md5s from §2. Generalise `gen_known_files.py`.
10. CLI and GUI (§5.2): `--image` (with `--iso` as an alias),
    `--list-packages`, a file filter that includes floppy images, the
    identified package named in the progress and result, and exit codes
    unchanged.
11. Update `importer/README.md`.

Interfaces: you produce the §4 layout and invariants, which lanes B and C
rely on without talking to you, and the §6 catalog, which the current
`AfterDark.scr` must read unchanged. You never run modules.

Acceptance:
- Every existing importer test passes. Only documented expectation updates
  are allowed: `catalog_real` ignores the new fields; `--catalog-only`
  without `FILES`.
- The new synthetic tests of §9 pass: zip, fat, password, identification,
  recipes and invariants, per-package atomicity and recovery, catalog merge.
- `AD_E2E_PKG=1` `import.pkg_real` passes:
  * the four images, each into a fresh scratch root: exit 0,
    `verified: image`, the exact layouts, 46/44/13/15 modules;
  * all four plus a Deluxe tree in one root: 202 modules, Deluxe's existing
    fields unchanged;
  * re-imports are isolated.
- A final run of the new `adimport` imports all four images into
  `build/win-pkg-import/assets-all` (plus a copy of the installed Deluxe
  `FILES` and `import.json`). Report the resulting catalog summary. The
  integration step uses that root.
- No test or run writes to `%LOCALAPPDATA%\LongAfterDark`. No After Dark
  bytes are in the repo.
=== end A ===

### B: pe32 lane + core

=== B ===
**Work package B: pe32 lane and core for packages.** Owns
`host/win32/**`, `host/pe32/**` and `host/core/**`
(core only for the `win_assets_dir` rule and its tests/README).
Read first: `docs/PACKAGES.md` §1, §4.1–§4.4, §7.1, §7.2 and §9,
`DESIGN.md` §6/§7, `host/win32/README.md`,
`host/core/README.md`, and the `ad10` survey artifacts in
`research/win/pkg/ad10/`. `hostpatch.diff` there is a verified private patch
adding the missing signatures; port it properly, don't paste blindly.
Build: `AD_BUILD_DIR=build/win-pkg-pe32 AD_COMPONENTS="host/core;host/cpu;host/loader;host/win32;host/pe32" bash tools/build.sh`.

Goals:
1. **Baseline first.** Record 120- and 300-frame `FBHASH` streams of the 23
   Deluxe AD4 modules with today's `build\dist\AfterDark\adhostwin.exe`.
2. **Package-aware search (§7.1/§7.2).** Packaged modules search the module
   dir, then `<package root>\ENGINE`, nothing else. Legacy modules keep
   today's order (module dir, then `<win>\FILES\AD40`). Add the one-line
   `ADTRACE=lane` init summary.
3. **Signatures** for the 49 imports that have none today:
   * KERNEL32: `CreateSemaphoreA` 16, `GetFileTime` 16, `SetFileTime` 16,
     `CopyFileA` 12, `RemoveDirectoryA` 4, `LocalFileTimeToFileTime` 8,
     `GetSystemInfo` 4, `FileTimeToSystemTime` 8, `GetVersionExA` 4,
     `CreateDirectoryA` 8, `MoveFileA` 8, `SystemTimeToFileTime` 8,
     `HeapSize` 12.
   * WINMM: `waveOutGetDevCapsA` 12, `waveOutReset` 4,
     `waveOutUnprepareHeader` 12, `waveOutSetVolume` 8, `waveOutRestart` 4,
     `waveOutPrepareHeader` 12, `waveOutWrite` 12, `waveOutOpen` 24,
     `waveOutGetVolume` 8, `waveOutGetNumDevs` 0, `waveOutPause` 4,
     `waveOutClose` 4.
   * USER32: `PostQuitMessage` 4, `PostMessageA` 16, `PeekMessageA` 20,
     `LoadCursorA` 8, `KillTimer` 8, `GetScrollInfo` 12, `GetClientRect` 8,
     `FindWindowA` 8, `EnableScrollBar` 12, `DispatchMessageA` 4,
     `BeginPaint` 8, `EndPaint` 8, `TranslateMessage` 4,
     `SystemParametersInfoA` 16, `ShowCursor` 4, `SetWindowPos` 28,
     `SetTimer` 16, `SetScrollInfo` 16, `SetCursor` 4, `SetClassLongA` 12,
     `RegisterClassExA` 4.
   * SHELL32: `ShellExecuteA` 24 (stays refused).
   * GDI32: `SetTextCharacterExtra` 8, `GetTextMetricsA` 8.

   Implement what `HALLOFFA` calls: `LoadCursorA` (a cursor handle),
   `GetFileTime` (from the VFS), and `waveOutGetNumDevs` (0, consistent with
   "no device"). Implement anything else it reaches in 1200 frames. A
   deterministic, discarded in-memory `WritePrivateProfileStringA` overlay
   for `HOF.INI` is optional.
4. **No silent stack corruption.** An import with no signature is logged at
   bind time and appears in the census. Calling it raises a clear
   `GuestError` naming the import instead of returning with an unbalanced
   stack. Add a unit test.
5. **Core:** `Env::win_assets_dir()` returns `<root>\win` if it holds
   `FILES`, `packages` or `catalog-win.json`; else `<root>` if it holds one
   of them; else `<root>\win`. Add a `core.unit` case and update the README.
6. **Test root.** Build `build/win-pkg-pe32/pkgroots/ad10/win/packages/ad10/`
   per §4.4 (with the fix-ups) and an "all" root that adds a copy of the
   installed Deluxe `FILES`. Re-run on A's real importer output when it
   exists.

Interfaces: the §4 layout is your only input from A. Core must not change
behaviour for the ne16 lane (C builds core in its own dir). Settle any
cross-lane question through `PACKAGES.md`, and report proposed changes in
your final report.

Acceptance:
- The 23 Deluxe AD4 modules give 120- and 300-frame `FBHASH` identical to
  the baseline.
- All 17 `ad10` pe32 modules run 300 frames: exit 0, deterministic over two
  runs, 0 unimplemented APIs in the census. These are `AD10TH`'s 16
  (`BADDOG`, `CYBER`, `FISH`, `HALLOFFA`, `HULA`, `MARBLES`, `MESSAGES`,
  `RAIN`, `RODGER`, `SHADOW`, `SUPERGUY`, `TIME`, `TOAST2K`, `TOASTER2`,
  `TOASTERS`, `TURTLE`) plus `ENGINE\STARRYNI.AD`. List any residual
  unimplemented calls with a reason.
- `HALLOFFA` runs ≥ 1200 frames, exit 0, deterministic.
- The 13 `ad10` AD4 modules that are md5-identical to Deluxe's give 300-frame
  `FBHASH` identical to the Deluxe runs.
- `TOASTER2` and `TOAST2K` open their long-name MIDIs successfully
  (`ADTRACE=file`).
- ctest of core, win32 and pe32 passes.
- Contact sheet of the four non-Deluxe pe32 modules (`HALLOFFA`,
  `TOASTER2`, `TOAST2K`, `STARRYNI`) checked by eye.
=== end B ===

### C: ne16 lane (+ win16, cpu)

=== C ===
**Work package C: ne16 lane for packages, with the native AD3 bridge.**
Owns `host/win16/**`, `host/ne16/**` and
`host/cpu/**` (CPU only for CPU bugs you find).
Read first: `docs/PACKAGES.md` §1, §4, §7.1, §7.3, §7.4 and §9,
`DESIGN.md` §7, `docs/ABI.md` §3 (the whole Classic ABI;
§3.3/§3.4 are the bridge spec), `host/win16/README.md`, and the
survey artifacts in `research/win/pkg/{ad10,ad32,tt,simpsons}/`
(`ad10/hostpatch.diff` for `user16.cc`; `simpsons/wincopy` for the
`PROGMAN.INI` seed; trial logs; disassembly). Port the verified private
patches properly.
Build: `AD_BUILD_DIR=build/win-pkg-ne16 AD_COMPONENTS="host/core;host/cpu;host/loader;host/win32;host/win16;host/ne16" bash tools/build.sh`.

Goals:
1. **Baseline first.** Record 120-frame `FBHASH` of the 61 Deluxe Classic
   modules with today's `build\dist\AfterDark\adhostwin.exe`.
2. **Package-aware resolution (§7.1/§7.3).**
   * Packaged modules: search the module dir, then `<package root>\ENGINE`;
     `C:\WINDOWS\SYSTEM` is mounted on the engine dir.
   * Legacy modules: exactly today's behaviour.
   * The AD_SND guard.
   * The `ADTRACE=lane` init summary.
3. **Bridge selection and the native AD3 bridge (§7.4).**
   * The real OLDMOD16 when the engine dir has `OLDMOD16.DLL`; otherwise
     the native bridge with the package's own `AD_SND.DLL`, no `VerStr` gate.
   * `ADNE16BRIDGE=auto|oldmod16|native`.
   * One bridge interface, so the lane's frame loop is shared.
   * Follow ABI.md §3.3 and OLDMOD16's code wherever §3.3 is silent.
4. **Palettes** for the native bridge: from `ENGINE\ADTASK.DLL` 5000/1..4
   with the §7.4 index mapping, else `ENGINE\AFTERDAR.SCR`. Log the source.
   Never embed palette data.
5. **Win16 shims for the desktop-icon gatherers of ADXPL40 and ADXPL310
   (§7.3):**
   * `USER.54 EnumWindows` over a fixed synthetic desktop;
   * `USER.129 GetClassWord`, `USER.368 CopyIcon`, `USER.457 DestroyIcon`,
     `USER.458 DestroyCursor`, `USER.407 CreateIcon`, `USER.262 GetWindow`;
   * `GetWindowPlacement`/`GetWindowRect`/`GetWindowText`/`GetClassName`/
     `GetWindowWord`/`IsWindowVisible`/`GetModuleFileName`, coherent for
     the synthetic windows;
   * `SHELL.34 ExtractIcon`, `GDI.79 GetDCOrg` (0,0), `GDI.50
     CreateBrushIndirect`;
   * a seeded `C:\WINDOWS\PROGMAN.INI` `[Groups]` + `.GRP` files.

   Icons are host-generated, never system icons. Keep it deterministic and
   leave Deluxe streams unchanged.
6. **`MODULES.INI` seeds:** `[Ray] RaySceneFile` = the first existing of
   `ROTCUBE`/`DIAMOND`/`ROTPYRA`.TRC, and
   `[Logo Section] LogoFile=C:\AFTERDRK\BITMAPS\ADLOGO.BMP`.
7. **`SIMPCLOK`** (Simpsons Clocks) runs but renders black. The survey's
   findings: zero-filled 0x92F8 save buffers, a bogus `Ellipse` rect, and
   the `hmemcpy` at ADXPL310 4:39D0. Investigate CPU (32-bit arithmetic in
   16-bit code), `GetDIBits`/`SetDIBits` with `DIB_PAL_COLORS`, and huge
   `hmemcpy`. Fix it if found. Otherwise document the findings.
8. **Test roots.** Build `build/win-pkg-ne16/pkgroots/<id>/win/packages/…`
   for all four packages per §4.4. These are standalone roots, with **no
   `FILES`**. Also build an "all" root with a copy of the installed Deluxe
   `FILES`. Re-run on A's real importer output when it exists.

Interfaces: the §4 layout and invariants from A are your only input. Core
belongs to B: do not edit `host/core`. If you need a core change,
report it.

Acceptance:
- The 61 Deluxe Classic modules give 120-frame `FBHASH` identical to the
  baseline at default settings.
- Bridge oracle: `ADNE16BRIDGE=native` versus `oldmod16`, both with
  `ADMIPS=0`, over the 61 Deluxe Classic modules for 120 frames: identical
  streams. Every exception is explained.
- On the standalone roots, **with no Deluxe files anywhere**, every ne16
  module of `ad10` (29), `ad32` (44), `tt` (13) and `simpsons` (15) runs
  120 frames: exit 0, deterministic over two runs. Report the census of
  unimplemented APIs per module. The census target is 0 in saver runs.
  Dialog-only imports that are never called do not count.
- `CHAM` (`ad10` and `tt`) runs ≥ 900 frames with chameleons animating.
- `HOMEREAT` and `INS` run ≥ 1800 frames.
- `LOGO` and `RAY` (`ad32`) run and draw.
- `GUTS` has no unimplemented calls.
- `SIMPCLOK` exits 0. Rendering its clocks is a stretch goal.
- `ad10`'s ne16 modules that are md5-identical to Deluxe Classic give
  `FBHASH` identical to the Deluxe runs, or the difference is explained.
- A lone module outside any package still runs as today.
- ctest of win16, ne16 and cpu passes.
- Contact sheets (frames 30/119 or later) of every new module, checked by
  eye.
=== end C ===

## 11. Integration (after A, B and C)

1. Build every component together (`host/*`, `importer`) in one build dir.
   B owns core and C builds against it unchanged, so the only merge is at
   link time.
2. Import the four images plus Deluxe into a scratch root with the new
   `adimport`, or use A's `build/win-pkg-import/assets-all`. Check that it
   matches the interim roots file for file.
3. Census every catalog entry (202 modules) with the combined `adhostwin`:
   120 frames, twice, exit 0, deterministic. Deluxe's 84 must match the
   baselines. Rerun on standalone roots (each package alone) for the AD 3.x
   packages.
4. Only then, and only if the user wants it, import the four packages into
   the real assets folder (`%LOCALAPPDATA%\LongAfterDark\assets`). That
   changes the list
   the settings dialog shows, and the saver needs no change for it.
5. Documentation to update afterwards (outside these packages):
   * WIP.md;
   * `tools/package.sh`'s dist README ("Deluxe only" wording; since rewritten for the five releases);
   * the scr status strings that count "After Dark 4 / Classic" (the scr
     workflow owns them). These are part of the user-requested wording pass
     in WIP.md.

## 12. Deferred and open

* **Mac halves (not read):**
  * `ad10`'s HFS volume "After Dark 10th Anniversary" holds a StuffIt
    InstallerMaker archive, "Main" (43.9 MB), and QuickTime 3.
  * `ad32` ("Untitled") and `tt` ("Totally Twisted CD") each hold a StuffIt
    InstallerMaker installer, "Double-Click Me To Install" (`APPL/STi0`),
    plus demos.
  * The Simpsons floppy has no Mac content.
* **Not cataloged this round:**
  * AD 3.2's built-in Starry Night lives inside `ADW30.EXE`, an NE
    *application* exporting `MODULE`. Supporting it would need library-style
    mapping of an EXE plus stubs for `ADHOOK`/`ADTASK`.
  * `ECOLOGIC.DLL` (the "EcoLogic" countdown blanker; it already runs as a
    `.DLL`) ships with all three AD 3.x packages.

  Both stay in `ENGINE\` for later.
* **Games:** `ad10`'s `GAMES\` (`ADGXPL10.DLL` + three game EXEs) are not
  screen savers.
* **Shared lane-wide TODOs** that these packages made more visible, and
  where they stand:
  * desktop seed: **done** (INTERACTION.md §8). The survey expected `MBORIS`,
    `PUZZLE`, `PUNCH`, `SPLIGHT`, `ZOOOMMM`, `GRASSKRT` and `OBJETS` to act
    on the desktop. The final census (900 frames of every module with and
    without a synthetic desktop seed) found that Shadow Agents (Clear Screen
    First off), Puzzle, Punch Out, Spotlight, Down the Drain, Bugs, Rebound,
    Can of Worms, Ray, Bad Dog!, Mr. Burns, Objets B'art and Homer Eats
    draw over it for the whole run; Mowin' Man, Hard Rain and String Theory
    for their first 270–375 frames; and Boris, Shapes and Spheres for their
    first few. Mowin' Boris, Grass Skirts, Zooommm! (both builds) and AD4's
    Slow Burn produce identical frames with and without it: Zooommm! blanks
    the screen itself, and Slow Burn's first PAINT reaches ADXPL510's
    full-screen `PatBlt(BLACKNESS)`, as it did under AFTERDAR.SCR;
  * audio: **done** (AUDIO.md): the Simpsons' speech in `SIMP_SND.DLL` and
    Totally Twisted's MCI sequencer music play;
  * pacing (WIP.md): still open. `TOILET` (`ad10`/`tt`) costs about 16 ms
    per frame;
  * a writable per-user overlay: **done** (INTERACTION.md §7): `HALLOFFA`'s
    `HOF.INI` and the other modules' files persist per user and per
    package.
* **Other AD 3.x collections** share this install family: Looney Tunes,
  Star Trek TNG, X-Men, Disney and Marvel, per the folder list in the IS
  scripts and the AFI files. `ad3zip` plus a registry entry (engine DLL,
  module dir, folder AFI, manifest) should cover them. The derived password
  makes that cheap. They, More After Dark and the AD 2.x releases for
  Windows are future packages: none is in the registry, and each needs its
  own survey (identification, layout, manifest, a known source) before it
  can be.
* **Deferred on purpose:** AD 3.2's built-in Starry Night (above: it needs
  an NE application mapped as a library; Deluxe and the 10th Anniversary
  ship Starry Night as a module), `ECOLOGIC.DLL` in the catalog (above),
  and module file-dialog templates (`OFN_ENABLETEMPLATE`, logged and
  ignored by `win32/comdlg32.cc`; no module in the corpus uses one).
* **Deduplication in the front-end** (`sameAs`) and grouping by package:
  decided by the settings dialog (COVERS.md §1.7, §1.8): grouped by
  release, byte-identical copies played once per Random pass.
