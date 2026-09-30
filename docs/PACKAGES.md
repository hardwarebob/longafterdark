# Long After Dark — packages

How the host imports and runs modules from seven releases, not just the
After Dark 4.0 Deluxe CD: six of After Dark, the oldest being Star Trek: The
Screen Saver (1992, After Dark 2.0b), and LucasArts' Star Wars Screen
Entertainment (1994), whose modules run on Delrina's Intermission screen
saver engine, not on After Dark's. DESIGN.md §6, §6a and §7 give the
contract in brief; this file is the full specification and the plan that
implements it.

**Status: implemented.** The importer reads all seven releases (232 catalog
modules). The 218 modules of the six After Dark releases run headless,
deterministically, and each release also runs on its own; Star Wars Screen
Entertainment's 14 are driven by the Intermission protocol of §7.5. This
file keeps the specification and the plan as they were written, so tables
that say "today" or "current" describe the host before this work. Star Wars
Screen Entertainment was added in a second round, after the rest was built
(the `swse` rows and paragraphs, §7.5, §8.6 and §8.7); there, "before this
release" means the five-release host. Star Trek: The Screen Saver was added
in a third round (the `startrek` rows and paragraphs, the After Dark 2.0
rules of §7.3 and §7.4, and §8.8); there, "before this release" means the
six-release host. Where the implementation went further, the component
READMEs are authoritative: `importer/README.md` (every release can also be
downloaded from the Internet Archive, and a ZIP of install files, or of a
release's floppy images, is a valid source), and `host/win32/README.md` and
`host/win16/README.md`.

Everything below about the four new After Dark discs comes from four
surveys (Sept 2026), about Star Wars Screen Entertainment from seven more of
its disc (Sept 2026: the Intermission protocol, the modules, the API census,
the host, the importer, the front-end, and the online copies and covers),
and about Star Trek: The Screen Saver from four more of its floppies (Sept
2026: the KWAJ format and the install, the importer, the lane, and the
modules' content). Their artifacts are in `research/win/pkg/<id>/`, which is
gitignored: extraction scripts, inventories, trial runs, contact sheets and
disassembly. **No After Dark, LucasArts or Paramount bytes, extracted files
or disassembly are ever committed.** Manifests hold only path, size and
md5. The ZIP password is derived at import time, never stored (§8.4).

## 0. Decisions at a glance

| Question | Decision |
|---|---|
| What is a package | One release in a built-in registry: id, titles, known images, fingerprints, recipe, manifest (§2) |
| Where it goes | Deluxe keeps `<win>\FILES\…` unchanged. Every other package goes in `<win>\packages\<id>\` and owns only that directory (§4). A package may also have a `WINDOWS` folder: what its installer put in `C:\WINDOWS` (`swse`: `SWSE.INI`), the lower layer of the guest's `C:\WINDOWS` (§4.1, §7.5). `startrek` has none: its settings are the lane's profile seeds (§7.3) |
| Import semantics | Stage and swap only that package's directory. Importing or re-importing one package never touches another. One `import.json` per package, inside its directory (Deluxe keeps `<win>\import.json`). There is no second index: the merged view is the catalog (§5) |
| Catalog ids | Deluxe keeps `ad40.*` / `classic.*`. Others use `<package>.<lower-case file stem>` (§6) |
| Duplicates | Keep all, one entry per package. Later duplicate names get ` (<short title>)`. `sameAs` links byte-identical modules (§6) |
| Lane → package | By path only: package root = parent of the module's folder. Engine dir = `<root>\ENGINE`. Packaged when the root's parent is named `packages`. No env var, no descriptor file (§7.1) |
| Cross-package files | Never, for packaged modules: search the module dir, then the engine dir. Deluxe and lone modules keep today's order (§7.1) |
| 16-bit host for AD 3.x | Real OLDMOD16 where the package ships it (`deluxe`, `ad10`). Otherwise a host-native AD3 bridge that implements OLDMOD16's verified behaviour. It uses the package's own AD_SND 3.x and ADTASK palettes. Deluxe is never needed (§7.4) |
| After Dark 2.0 (`startrek`) | The same AD3 protocol and native bridge: After Dark 2.0's `AD.EXE` drove its modules with the same messages and blocks as OLDMOD16, and the host replaces it, an NE application, as it replaces `AFTERDAR.SCR` (ABI.md §3.9). The bridge takes AD_SND 1.0's own volume pair (§7.4). The lane's other differences are rules by file, keyed on `AD_MOD.DLL` in the module folder: `AD_PREFS.INI` profile seeds, result 5 as the module's wake, no AD palettes (§7.3). Every catalog entry has `"screen": "640x480"`: several of the modules compose a fixed 640×480 scene, and all 16 get that screen so the release looks as it did at 640×480 (§6) |
| Intermission modules (`swse`) | A second module protocol inside the ne16 lane, chosen by the module's exports. Intermission's own IMX reader, `IMIMXPLY.IMQ`, runs as real code, as OLDMOD16 does; a native C++ reader is the oracle and the fallback. The host replaces `INTERMIS.EXE`, an NE application, as it replaces `AFTERDAR.SCR` (§7.5; the protocol is ABI.md §3.8) |
| Known images | An image md5 names a release, or since the seventh release one install disk of a release on several floppies: every disk exactly once, from any known copy of each, is that release's known image (`verified: image`, §3) |
| Install-time fix-ups | A baked table per package: file copies under the names the modules open (§4.3). No INF, IS-script, Presage-script or MS-Test interpreter: `intermission` and `ad2kwaj` are baked too |
| Formats to implement | FAT12/16 image reader; PKZIP with traditional PKWARE (ZipCrypto) decryption and raw inflate (zlib). The existing ISO-9660/Joliet reader already handles all three hybrid CDs (§8). For `swse`: multi-volume ARJ 2.x (§8.6) and SZDD (§8.7). For `startrek`: KWAJ method 3 (§8.8), and a ZIP of floppy images read as those images (§5.2) |
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
| `swse` | 14 ne16, Intermission IMX modules (surveyed later, against the five-release host) | none: the lane took an IMX for an After Dark module, loaded Intermission's own `AD_SND.DLL` and exited 1 ("AD_SND.DLL lacks an entry point"); a research prototype of §7.5 showed the protocol right, every module then stopping at SWSE's "resources are too low" screen | the importer refused the disc (no ARJ or SZDD, no identification); no Intermission protocol; STRESS's handle probe (`GetTempFileName` created no file, ABI.md §3.8.7), `AccessResource`, the DIB driver `CreateDC("DIB")`, and a few more Win16 calls (§7.5) |
| `startrek` | 16 ne16, After Dark 2.0b modules (surveyed last, against the six-release host) | none: every module stopped at load, "AD_SND.DLL lacks an entry point" (its AD_SND 1.0 has no `adwGetSystemVolumes`/`adwSetSystemVolumes`); in a research prototype with that fixed, 15 stopped with "File not found." and Sounder with "Can't find any .WAV files to play!" until `AD_PREFS.INI`'s `[After Dark] Path` reached them | the importer refused the user's ZIP, the two images and a folder copy ("not a known release": no KWAJ, no ZIP of floppy images, no disk sets); AD_SND 1.0's volume pair; `AD_PREFS.INI`, which the lane's empty virtual file hid, and whose PC-speaker sound driver hangs the emulator (§7.3); Final Exam: no Num Lock toggle, and its wake (result 5) taken for an error |

In total, 232 catalog entries over the seven packages: 218 over the six After
Dark ones, and Star Wars Screen Entertainment's 14.

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
| `swse` | Star Wars Screen Entertainment | Star Wars | `intermission` | `packages/swse` | `SAVER` |
| `startrek` | Star Trek: The Screen Saver | Star Trek | `ad2kwaj` | `packages/startrek` | `AFTERDRK` |

Ids match `[a-z0-9]+`. `ad40` and `classic` are reserved: they are Deluxe's
legacy id prefixes. `swse` is the product's own short name (its installer's
`shortname`, its volume id); it comes last so that the importer window's
command ids of the first five stay put. `startrek` is Berkeley's own key for
the release (After Dark 4.0 Deluxe's `ENGINE\PREVIOUS.INF` lists it as
`[StarTrek]`, `Name=Star Trek`, the short title; a later The Next
Generation package would take its `[STTNG]`); it comes after `swse`, last,
for the same reason.

Known images (identification + `verified: image`):

| id | image md5 | size (B) | medium | volume id |
|---|---|---|---|---|
| `deluxe` | `d875a60338b73f44b7befa06bdd33aeb` | 400,234,496 | ISO-9660, hybrid | `AD_DELUXE` |
| `ad10` | `a8d088415d391ce0d3199a831e1b2ad6` | 150,228,992 | ISO-9660 + Joliet, Apple partition map hybrid | `AD10TH` |
| `ad32` | `8b8be6977375fbf4d54146b9d505aa1c` | 61,693,952 | ISO-9660 level 1, no Joliet, APM hybrid | `ADW320_C` |
| `tt` | `541b9cfd744c377263a4ab1a144bdc12` | 39,784,448 | ISO-9660 level 1, no Joliet, APM hybrid | `TTW320CD` |
| `simpsons` | `7674adbda6fc5402d4e7a78af14880cc` | 2,949,120 | FAT12, 2.88 MB, both install floppies merged (WinImage) | — |
| `swse` | `bfa63c1bce15dcbea965dfd7c2ed44e8` | 7,227,392 | ISO-9660 level 1, no Joliet, not hybrid: a CD copy of the five install floppies (mastered 1995-05-24) | `SWSE` |
| `swse` | `ce51614a3484b9269b5ed9e61510e971` | 9,005,808 | the same pressing as raw 2352-byte sectors (Redump 24301): cooked, its first 3529 sectors are the ISO above | `SWSE` |
| `startrek` | `28e33608b8d3bafa28585472c4a7a9ac` | 1,474,560 | FAT12, 1.44 MB: install disk 1 of 2 (Internet Archive item `afterdark-20b_startrek`, and the ZIP that item serves) | — |
| `startrek` | `c630da5f6839303b599947f56fdd7c25` | 1,474,560 | FAT12, 1.44 MB: install disk 2 of 2 (the same) | — |
| `startrek` | `6ee71b45e32b07001d46ab8c80af589d` | 1,474,560 | install disk 1 of 2 as a Windows 9x copy wrote to it (item `startrektosscreensaver1992win`: the boot sector's OEM name and the root directory's access dates differ; every file is the same, byte for byte) | — |
| `startrek` | `af9d29a7ddea2c03618899c1c5733c67` | 1,474,560 | install disk 2 of 2, the same way | — |

A known image is the whole release, or, since the seventh release, one
install disk of a release on several (`KnownImage::disk` n of N):
`startrek`'s four are its two disks in two copies, and only a set of every
disk is the release (§3).

Other registry fields:

* `required` (relative to the package root; import fails with 2 without them):
  * `ad10`: `AD10TH/ADXPL510.DLL`, `AD10TH/ADXPL300.DLL`, `AD10TH/ADXPL40.DLL`, `ENGINE/OLDMOD16.DLL`, `ENGINE/AD_SND.DLL`, `ENGINE/AFTERDAR.SCR`
  * `ad32`: `AD32/ADXPL300.DLL`, `ENGINE/AD_SND.DLL`, `ENGINE/ADTASK.DLL`
  * `tt`: `TWISTED/ADXPL40.DLL`, `TWISTED/TT_SND.DLL`, `ENGINE/AD_SND.DLL`, `ENGINE/ADTASK.DLL`
  * `simpsons`: `SIMPSONS/ADXPL310.DLL`, `SIMPSONS/SIMP_SND.DLL`, `ENGINE/AD_SND.DLL`, `ENGINE/ADTASK.DLL`, plus all 15 module ZIPs (so a split-floppy source is complete)
  * `swse`: `SAVER/INTRMLIB.DLL`, `SAVER/ANTSW.DLL`, `SAVER/SWSE.DLL`, `SAVER/READJPG.DLL`, `SAVER/STRESS.DLL`, `SAVER/SWSFX.DLL`, `SAVER/MEMMIDI.DLL` (what every module loads, statically or by name), `ENGINE/IMIMXPLY.IMQ` (the reader), `WINDOWS/SWSE.INI` (the modules' default settings), plus all five ARJ volumes (so every install disk is there)
  * `startrek`: `AFTERDRK/AD_MOD.DLL`, `AFTERDRK/AD_RSRC.DLL` (the modules' framework), `AFTERDRK/ST_RES/ST_RESDB.DLL`, `ST_MASKS.DLL`, `ST_VGA.DLL`, `ST_SVGA.DLL`, `ST_SND.DLL` (the art and sound databases `AD_MOD.DLL` opens), `ENGINE/AD_SND.DLL` (the sound library the native bridge loads), plus both install disks' tag files (`MISSION.AD_`, `ST_SND.DL_`: every install disk is there)
* `ad3zip` parameters: `moduleDir` (`AD32` / `TWISTED` / `SIMPSONS`); `engineDll`, the member of `MODMISC.ZIP` that identifies the package (`ADXPL300.DLL` / `ADXPL40.DLL` / `ADXPL310.DLL`); `folderAfi`, the `AFI.ZIP` member that becomes `FOLDER.AFI` (`AD3.AFI` / `PHLEM.AFI` / `SAX.AFI`).
* `intermission` parameters (`swse`): `moduleDir` (`SAVER`); `installName`, the `[data] shortname` of the installer's `INSTALL.DAT` that identifies the package (`SWSE`); the archives (`SWSE1.ARJ`, `SWSE2.ARJ`, `SWSE2.A01`, `SWSE2.A02`, `SWSE2.A03`: disk 1, then one archive over disks 2–5); `looseFiles`, what the recipe takes from outside the archives, with the name the installer gave it and whether it is SZDD (§4.3).
* `ad2kwaj` parameters (`startrek`): `moduleDir` (`AFTERDRK`); `setupTitle`, the `[Params] WndTitle` of Microsoft Setup's `SETUP.LST` that identifies the package (`Star Trek\xAE: The Screen Saver`, 0xAE being Windows-1252's ®); the install disks' tag files, from `ST_NSTLL.INF [Source Media Descriptions]` (`MISSION.AD_` on disk 1, which the fingerprint wants beside `SETUP.LST`, and `ST_SND.DL_` on disk 2); `looseFiles`, every file the recipe installs, each KWAJ-compressed (§4.3); `screen` (`640x480`) and the About rules (`ad20`), for the catalog (§6).
* `fixups` (§4.3), `nameOverrides` (§6), `manifest` (`known_files_<id>.inc`, generated like today's `known_files.inc`; `swse` 29 files, `startrek` 27), `downloadUrl` (Deluxe only, as planned; the importer now lists Internet Archive copies for every package, `importer/README.md` "Downloads"; `swse` has three: the ISO, the Redump BIN and a flat ZIP of the disc's 40 files, 6.9, 8.6 and 6.7 MB; `startrek` two, each the images of both disks, 2.8 MB: a copy is used only when both of its images are fetched and verify, §5.2).
* `released` (`YYYY-MM[-DD]`, §6): `swse` is `1994-08`. Every Windows build of it found dates from 1994-08-20 at the earliest (`INTERMIS.EXE`), LucasArts announced it for July 1994 on both platforms, and this CD is a later build (its files are from October 1994). It ties with the Simpsons (`1994-08`) and sorts after it by registry order. `startrek` is `1992-11`: its newest files are dated 1992-11-16 (15 of the 16 modules, all but Sounder, with `AD_MOD.DL_`, `ST_NSTLL.IN_`, `AD_NSTLL.DL_` and `AD_NSTLL.MS_`), so it is the oldest release and comes first in the catalog's `packages` list.

## 3. Identification

A source is a file (an image), a folder, or several images (split floppies).
Every source is read through one `SourceFs` view: an ISO-9660/Joliet image,
a FAT12/16 image, or a host folder. Names are matched case-insensitively and
reported as 8.3 upper case.

1. **Image md5.** The md5 is computed for every image source anyway, for
   `verified`. A match names the package, and that package's fingerprint must
   then also match. If it does not, the source is invalid (2).

   **Disk sets** (since the seventh release). For a release on several
   install disks the known images are its disks' (§2): the images of every
   disk exactly once, from either copy of each, in any order, loose or in a
   ZIP, and nothing else, are its known image (`verified: image`,
   `imageMd5Known` true, no `imageMd5`; an image given twice, or again in a
   ZIP, is read once). Fewer disks still name the package. Disk 1 alone is
   identified by its fingerprint and then refused by the recipe ("the source
   is missing ST_SND.DL_; importing Star Trek: The Screen Saver needs every
   install disk", §4.3). Other disks without it are refused as what they are:
   "this image is install disk 2 of 2 of Star Trek: The Screen Saver (by its
   md5); import every disk together (--image … --image …, or the ZIP they
   came in)", or for several images "these images hold install disk 2 of 2
   of … (by their md5s) but not the rest of it; …". The whole set with
   another image beside it (a stranger, or a second copy of a disk) is no
   known set: it is identified by its fingerprint and verified file by
   file.
2. **Fingerprints**, tried for every package in registry order (for folders,
   and for images with an unknown md5). Exactly one package must match: none
   → 2 ("not a known After Dark disc; known: …", since the sixth release
   "not a known release; known: …"), several → 2 (ambiguous).
   * `deluxe`: a `FILES` dir (`ADE\FILES`, `FILES`, or the root) holding `AD40\` and `ENGINE\`. This is today's rule, unchanged.
   * `ad10`: a `FILES` dir (same three places) holding `AD10TH\` (with `ADXPL40.DLL`) and `ENGINE\`, and no `AD40\`.
   * `ad3zip` family: an install dir (`INSTALL\`, or the root for floppies and floppy copies) holding `INSTALL.INS`, `SETUP.PKG`, `ENGINE.ZIP` and `MODMISC.ZIP`. The package is the one whose `engineDll` is a member of `MODMISC.ZIP`. Central-directory names are not encrypted, so no password is needed to identify.
   * `intermission` (`swse`): Presage's installer script `INSTALL.DAT` at the source's root (the CD, disk 1, a flat ZIP, or a folder copy of the disks), a plain file of at most 64 KiB (a larger one is simply no match), whose `[data] shortname` is the package's `installName` (`SWSE`; sections and keys compared without case, values trimmed), with the first archive, `SWSE1.ARJ`, beside it. The script is read for nothing else. Disk 1 alone is identified, and then refused because every install disk is needed (§4.3); disks 2–5 without it match nothing.
   * `ad2kwaj` (`startrek`): Microsoft Setup's file list `SETUP.LST` at the source's root (disk 1, the disks together, or a flat folder, ZIP or ISO of their files), a plain file of at most 64 KiB (the real one is 654 bytes), whose `[Params] WndTitle` is the package's `setupTitle` ("Star Trek®: The Screen Saver", compared in Windows-1252 as the file holds it, without ASCII case, values trimmed), with disk 1's tag file, `MISSION.AD_`, beside it. `CmdLine` is not used: it is the same for every Berkeley After Dark 2.0 setup. The file is read for nothing else. Disk 1 alone is identified, and then refused because every install disk is needed (§4.3); disk 2 alone matches no fingerprint (its md5 names it, step 1). An installed `C:\AFTERDRK` is no source: it has no `SETUP.LST`, and the installer wrote the owner's name, company and serial number into its `AD.EXE` (resource type 3000, ids 1–3), so that file never matches the manifest.
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
<win>\packages\swse\SAVER\…                   14 IMX modules, INTRMLIB, ANTSW, SWSE, READJPG, MEMMIDI, SWSFX, STRESS,
                                                SWTEXT.TXT, 4 MIDI (26 files; the guest's C:\SAVER, §7.5)
<win>\packages\swse\ENGINE\…                  INTERMIS.EXE (replaced by the host, kept for reference), IMIMXPLY.IMQ
<win>\packages\swse\WINDOWS\…                 SWSE.INI, which the installer put in C:\WINDOWS
<win>\packages\startrek\AFTERDRK\…            16 modules, AD_MOD, AD_RSRC, AD_MME.DRV, ST_RES\ (5 DLLs), SOUNDS\JIM.WAV
                                                (25 files; the guest's C:\AFTERDRK, §7.3)
<win>\packages\startrek\ENGINE\…              AD_SND 1.0, AD.EXE (replaced by the host, kept for reference)
<win>\packages\<id>\import.json               record (v2, §5.3)
<win>\catalog-win.json                        merged catalog (§6)
```

Names are 8.3 upper case, as the sources list them. The only exceptions
are the long-name aliases of §4.3.

**`WINDOWS\`** is an optional package-root folder, new with the sixth
release: the files the original installer put in `C:\WINDOWS`. It is never
a module folder (the catalog never scans it, and the invariants below do not
count it as one). The ne16 lane mounts it, when it exists, as the read-only
lower layer of the guest's `C:\WINDOWS` (§7.5): a rule by file, for every
package, never by package id. Only `swse` has one. `startrek`'s installer
put its settings file, `AD_PREFS.INI`, in `C:\WINDOWS` too, but the package
has no `WINDOWS` folder: those settings are the lane's profile seeds, which
a file there would override (§7.3).

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

**For an `intermission` package** (`swse`) they read:

* **I1** also: no `INTERMIS.EXE` and no `*.IMQ` in the module folder.
  Intermission and its reader live in `ENGINE`; `SAVER` holds only the
  modules and what they load. The disc's `AD_SND.DLL` is Intermission's
  ("Intermission AD Sound Support", for running After Dark modules through
  its `IMAD_PLY.IMQ` reader), not Berkeley's: nothing an IMX loads imports
  it, and beside the modules it would break the After Dark bridge, so the
  recipe never installs it.
* **I2** also: every non-system DLL that a DLL in the module folder imports
  is there too (`INTRMLIB.DLL` → `ANTSW.DLL`; `TOOLHELP`, `LZEXPAND`, `VER`
  and `WING` count as system here). What SWSE.DLL loads by name
  (`SWSFX.DLL`, `MEMMIDI.DLL`) is in `required`.
* **I3** instead: `ENGINE\IMIMXPLY.IMQ` and the installer's `WINDOWS` files
  (`SWSE.INI`) exist, and `ENGINE` holds none of `OLDMOD16.DLL`,
  `ADTASK.DLL` and `AD_SND.DLL`, so the ne16 lane can never take the package
  for an After Dark one. The After Dark I3 does not apply.
* **I4** instead: every `*.MID` directly in the module folder. The modules
  open their music by bare name, from their current directory `C:\SAVER`,
  where the installer put it (no `MUSIC\`).
* **I5**: only `INSTALL.DAT` (to identify), the five archives and the loose
  files are ever opened; the never-read list is in §4.3.

**For an `ad2kwaj` package** (`startrek`) they read:

* **I1** also: no `AD.EXE` in the module folder. After Dark 2.0's host is
  kept in `ENGINE`, for reference; nothing loads it.
* **I2** also: every non-system DLL that a DLL or sound driver (`*.DRV`) in
  the module folder imports is there too (`AD_MOD.DLL` → `AD_RSRC`;
  `AD_SND` is `ENGINE`'s, as for the modules; `TOOLHELP`, `LZEXPAND`, `VER`
  and `WING` count as system here). The installer put `AD_MOD.DLL` and
  `AD_RSRC.DLL` in `C:\WINDOWS`; the modules import them, so they sit
  beside the modules.
* **I3** instead: `ENGINE\AD_SND.DLL` (AD_SND 1.0) exists; `ENGINE` holds
  none of `OLDMOD16.DLL`, `ADTASK.DLL` and `AFTERDAR.SCR` (After Dark 3.x's
  and 4.x's); and there is no `WINDOWS` folder at all: the lane's profile
  seeds are the package's settings, and a file there (the disk's
  `AD_PREFS.INI`, whose PC-speaker driver hangs the emulator) would win
  over them (§7.3).
* **I4** instead: every sound database (a `*_SND.DLL` other than
  `AD_SND.DLL`: `ST_SND.DLL`) in the module folder's `ST_RES\`, where
  `AD_MOD.DLL` opens it (`<Path>ST_RES\`).
* **I5**: only `SETUP.LST` (to identify) and the recipe's 27 files are ever
  opened; the never-read list is in §4.3.

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

**`intermission`** (`swse`): the disc is a CD copy of the five install
floppies of Presage's installer: `INSTALL.EXE` + its script `INSTALL.DAT`,
two ARJ archives (`SWSE1.ARJ` on disk 1; `SWSE2.ARJ` + `.A01`–`.A03`, one
archive over disks 2–5, with three members split across volumes) and
COMPRESS'd (SZDD) loose files. The recipe reproduces the installer's
placement, flattened as `ad3zip`'s is: the module folder `M` = `SAVER` (the
installer's `C:\SAVER`, which the lane mounts there), `E` = `ENGINE` (the
guest's `C:\WINDOWS\SYSTEM`) and `WINDOWS`. It is baked in from registry
parameters (§2); `INSTALL.DAT` is read only to identify, never interpreted,
and an opt-in real test checks the baked table against the disc's script
(§9).

1. **Every install disk.** Each registry archive must be in the install dir
   (the source's root), else 2: "the source is missing SWSE2.A01, …;
   importing Star Wars Screen Entertainment needs every install disk".
2. **The archives.** Each chain of volumes that starts at one of the
   registry's `.ARJ` names, followed for as long as a volume's main header
   says another follows (`X.ARJ`, `X.A01`, …), and only through the
   registry's archives: a volume that says it goes on to one the registry
   does not list (`SWSE2.A03` naming an `SWSE2.A04`) is damaged or foreign
   (2), and the file it names is never opened. Any other `.ARJ` on the
   source is skipped (logged); a volume no chain reaches is never read.
3. **The members**, by name:

   | Members | Go to |
   |---|---|
   | `INTERMIS.EXE`, `IMIMXPLY.IMQ` (Intermission, which the host replaces, and its IMX reader) | `E\` |
   | every other `*.IMQ` (the readers of other products' formats, among them the After Dark reader `IMAD_PLY.IMQ`), `AD_SND.DLL` (Intermission's sound support for them), `IWLIB.DLL` (another product's extension library), `*.HLP` | skipped: listed, never decoded (one log line) |
   | everything else: the 14 `*.IMX`, the DLLs they load, `SWTEXT.TXT` | `M\` |

4. **The loose files**, under the names the installer gave them
   (`INSTALL.DAT` lines 4, 10–13 and 37), each only when present (the
   manifest reports a missing one):

   | From | To | How |
   |---|---|---|
   | `STRESS.DL_` | `M\STRESS.DLL` | SZDD, expanded |
   | `GM_BATTL.MI_`, `GM_CNTNA.MI_`, `GM_EMPIR.MI_`, `GM_TITLE.MI_` | `M\BATTLE.MID`, `CANTINA.MID`, `EMPIRE.MID`, `SWTHEME.MID` | SZDD, expanded: the General MIDI set (the installer's `CHECK=24`, as `ad3zip` takes `MUSICG`), directly beside the modules, which open these names |
   | `SWSE.INI` | `WINDOWS\SWSE.INI` | as is: the modules' default settings |

Result: `SAVER\` 26 files (14 modules, `INTRMLIB`, `ANTSW`, `SWSE`,
`READJPG`, `MEMMIDI`, `SWSFX`, `STRESS`, `SWTEXT.TXT`, 4 MIDI), `ENGINE\` 2,
`WINDOWS\` 1: 29 files, the manifest's. A member's `from` names its volume
(`SWSE1.ARJ!ANTSW.DLL`), or its volumes when it is split
(`SWSE2.ARJ+SWSE2.A01!JAWAS.IMX`). Every planned size (an ARJ member's, an SZDD
header's) counts towards the staging budget and is checked against the
manifest before a byte is written. There are no fix-ups: the renames are
part of the plan.

**Never read** (I5), 28 of the disc's 40 files: `README.TXT`,
`INSTDETL.DAT` (which describes the archives of the earlier builds, not this
CD's), `INSTALL.EXE`, `SVGA.EXE`, `SYSINI.DAT`, the VxDs `ANTHOOK.386`,
`DVA.386` and `DVA.38_`, `IMCPL.CPL`, `SWSESET.EXE` (the GDI/WinG chooser,
whose job the lane's seeds do), `DIB.DR_` (the host has its own DIB driver),
WinG (`WING.DL_`, `WING32.DL_`, `WINGDE.DL_`, `WINGDIB.DR_`,
`WINGPAL.WN_`) and the 12 MIDI files of the other three sets (`SB6*`,
`SBP*`, `FM_*`). Opened: `INSTALL.DAT`, the five archives and the six loose
files.

**`ad2kwaj`** (`startrek`): the release came on two 1.44 MB floppies
installed by Microsoft Setup 2.0. `SETUP.EXE` expanded what `SETUP.LST`
lists and ran the MS-Test script `AD_NSTLL.MST` with `_MSTEST.EXE`.
`ST_NSTLL.INF` lists every file of both disks but itself, 61 lines in
sections, and the script copied the sections it installs, each where it
says. 59 of the disks' 62 files are KWAJ-compressed (§8.8); the three plain
ones are `SETUP.EXE`, `SETUP.LST` and `AD_NSTLL.INI`. The recipe reproduces
the script's placement of what the modules use, flattened into the module
folder `M` = `AFTERDRK` (the installer's `C:\AFTERDRK`, which the lane
mounts there) and `E` = `ENGINE`. It is baked in from registry parameters
(§2): no INF or MS-Test interpreter; `SETUP.LST` is read only to identify,
and an opt-in real test checks the baked table against the disks' own
`ST_NSTLL.INF` (§9). Nothing is edited.

1. **Every install disk.** Each disk's tag file (`MISSION.AD_`,
   `ST_SND.DL_`) must be at the source's root, else 2: "the source is
   missing ST_SND.DL_; importing Star Trek: The Screen Saver needs every
   install disk".
2. **The loose files**, KWAJ-expanded under their installed names (the
   INF's), each only when present (the manifest reports a missing one):

   | From | To | Why |
   |---|---|---|
   | the 16 modules, `BRAINCEL.AD_` … `TRIBBLE.AD_` | `M\*.AD` | the INF's `[ADModules]` |
   | `AD_MOD.DL_`, `AD_RSRC.DL_` | `M\AD_MOD.DLL`, `M\AD_RSRC.DLL` | the modules' framework: the installer put them in `C:\WINDOWS`, but the modules import them (I2) |
   | `AD_MME.DR_` | `M\AD_MME.DRV` | the multimedia sound driver AD_SND 1.0 loads from the After Dark directory (§7.3; AUDIO.md §2.12) |
   | `ST_RESDB.DL_`, `ST_MASKS.DL_`, `ST_VGA.DL_`, `ST_SVGA.DL_`, `ST_SND.DL_` | `M\ST_RES\*.DLL` | the art and sound databases `AD_MOD.DLL` opens from `<Path>ST_RES\` (I4) |
   | `JIM.WA_` | `M\SOUNDS\JIM.WAV` | Sounder's default folder: without a `.WAV` there Sounder refuses to start |
   | `AD_SND.DL_` | `E\AD_SND.DLL` | AD_SND 1.0, which the native bridge loads (I3) |
   | `AD.EX_` | `E\AD.EXE` | After Dark 2.0's host, which the host replaces: the disks' pristine copy (an installed one carries its owner's name, §3), kept for reference as `INTERMIS.EXE` is; nothing loads it |

Result: `AFTERDRK\` 25 files (the 16 modules, `AD_MOD.DLL`, `AD_RSRC.DLL`,
`AD_MME.DRV`, the five `ST_RES\` DLLs and `SOUNDS\JIM.WAV`) and `ENGINE\` 2:
27 files, 5,187,460 bytes, the manifest's. KWAJ records no size: each file
is expanded once while planning to learn it, bounded by what is left of the
staging budget (`KwajTooLarge`), and that size counts towards the budget
and is checked against the manifest before a byte is written; the copy
expands no further. The copies keep the disks' FAT timestamps. A file's
`from` is its compressed name (`PLANETS.AD_`). There are no fix-ups.

**Never opened** (I5): `ST_NSTLL.INF` itself (`ST_NSTLL.IN_`) and 33 of its
61 lines, every one but the table's 27 and `SETUP.LST` (read only to
identify):

* the disk's `AD_PREFS.INI`, whose `[Sound] SoundDriver=AD_MPT.DRV` would
  win over the lane's profile seed and hang the emulator (§7.3);
* the PC-speaker path, `AD_MPT.DRV`, `SPALETTE.DLL` and `AD_LIB.DLL`, and
  `AD_SB.DRV` (the Sound Blaster under Windows 3.0: it refuses Windows 3.1
  and later);
* the network support (`AD_AILAN.DLL`, `AD_NVLNW.DLL`, `AD_NET.EXE`,
  `NWCORE.DLL`, `NWCONN.DLL`, `NWMISC.DLL`), the VxD `AD.386`,
  `AD_WRAP.COM`, `AFTERDRK.NSS` (for Norton Desktop), `ADINIT.EXE` (whose
  `WinMain` returns 0) and `AD.HLP`;
* `AD_MESG.ADS` (the data of After Dark 2.0's generic Messages module,
  which this release does not ship) and `AD_NSTLL.INI`;
* Microsoft Setup's own files: `SETUP.EXE`, `_MSTEST.EXE`, `AD_NSTLL.MST`,
  `AD_NSTLL.DLL`, `MSDETECT.INC`, `SETUPAPI.INC`, `MSUILSTF.DLL`,
  `MSSHLSTF.DLL`, `MSCUISTF.DLL`, `MSDETSTF.DLL`, `MSINSSTF.DLL`,
  `MSCOMSTF.DLL`, `VER.DLL`, `SPLASH1.BMP` and `BMPRSRC.DLL`.

Opened: `SETUP.LST` and the table's 27 files, 28 of the disks' 62.

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
  Simpsons image already holds both disks. Star Wars Screen
  Entertainment's five 1.44 MB floppies are read the same way, in any
  order (the floppy sets found online are builds other than this CD's and
  fail verification, §12), and so are Star Trek: The Screen Saver's two, in
  either order: a known disk set (§3). A flat folder, ZIP or ISO of both
  disks' files is a source too (verified `files`); a folder that keeps the
  disks apart, in `DISK1\` and `DISK2\`, is not (§12).
* **A ZIP of floppy images** (since the seventh release; `source.h`
  `floppy_images_in_zip`) is those images. The Internet Archive serves an
  item's disk images together as a ZIP it makes on the fly
  (`https://archive.org/compress/afterdark-20b_startrek/formats=ISO%20IMAGE&file=/afterdark-20b_startrek.zip`,
  the user's copy of Star Trek: The Screen Saver among them): its members'
  times are the download's, so the ZIP's own md5 changes with every
  download and can never be a known image or a download's md5; its images'
  md5s identify it. A ZIP given as an image (at most 256 MB, held in
  memory) whose members of a DOS floppy's size (a multiple of 512 bytes,
  160 KB to 2.88 MB) are FAT12/16 volumes is read as those images: each is
  inflated into memory with its size and CRC-32 checked and read as if it
  had been given with `--image`, a part of its own named `<zip path>!<member>`,
  and the images are unioned as above (an image given again, loose or in
  the ZIP, is read once). The other members (label scans, the item's
  metadata) are named in the log and never inflated; a floppy-sized member
  whose first sector is no boot sector (`55 AA` and a sector size the FAT
  reader takes) is not inflated past its first 64 KiB. A
  password-protected floppy-sized member is refused (2), and so is a ZIP
  whose members with a boot sector add up to more than 64 MB
  (`kMaxZippedImageBytes`: "… holds more than 64 MB of disk images; no
  release came on that many disks"): a bound, which the plan did not have,
  against a crafted ZIP. A ZIP with no floppy image is a ZIP of install
  files, as before.
* `--download` was planned to stay Deluxe-only, since no other package had
  a known URL when this was written. The Internet Archive search that
  followed found copies of every package, so `--download <id>|all` now
  fetches any of them (`importer/README.md`, "Downloads"). Since the seventh
  release a copy may be several files (`Download::more_images`, each a
  `DownloadPart`): `startrek`'s two copies are the images of both of its
  disks, from the items `afterdark-20b_startrek` and
  `startrektosscreensaver1992win` (2.8 MB, "download 2.8 MB (2 floppy
  images)" in `--list-packages`). Each image is checked against its own
  published size and md5, the progress runs over the whole copy, and a copy
  is used only when every image verifies; they are then imported as a known
  disk set, as `--image <disk 1> --image <disk 2>` would be. Any image
  failing moves on to the next copy.
* Exit codes are unchanged (0 ok · 1 error · 2 source invalid · 3 verify
  failed · 4 network · 5 cancelled). The settings dialog launches
  `adimport --gui` and reads only 0 and 5.
* GUI: the chooser offers "disc image (ISO or floppy image)", "drive or
  folder" and "download After Dark 4.0 Deluxe" (as planned; the download
  choice now lists every release). The file dialog filter
  includes `*.iso;*.img;*.ima;*.vfd;*.flp`. The progress window names the
  package it identified. The final message says "Imported <title>: N
  modules" and lists the installed packages. The restyle queued for later
  is out of scope here. (As built since the seventh
  release: the filter reads "Disc and floppy images, and ZIPs of them or of
  install files", and the disc-image card "An ISO image of a CD, or floppy
  images (.img), zipped or not; select every disk of a set.")
* `win_assets_dir(root)`, in both the importer and the host: `<root>\win`
  if it holds `FILES`, `packages` or `catalog-win.json`; else `<root>` if
  it holds one of those; else `<root>\win`.

### 5.3 `import.json` version 2 (packages other than Deluxe)

```json
{ "version": 2, "tool": "adimport 1.3", "importedUtc": "2026-09-26T18:00:00Z",
  "package": { "id": "ad32", "title": "After Dark 3.2", "recipe": "ad3zip", "root": "packages/ad32" },
  "source": { "kind": "iso", "format": "iso9660", "path": "D:\\…\\afterdark3.2.ISO",
              "imageSize": 61693952, "imageMd5": "8b8be6977375fbf4d54146b9d505aa1c", "imageMd5Known": true,
              "volumeId": "ADW320_C", "parts": [] },
  "verified": "image", "fileCount": 0, "totalBytes": 0, "missingKnown": [],
  "files": [ { "path": "packages/ad32/AD32/GUTS.AD", "size": 0, "md5": "…", "known": "match",
               "from": "INSTALL/GUTS.ZIP!GUTS.AD" } ] }
```

* `package.recipe` is `tree`, `ad3zip`, `intermission` or `ad2kwaj`.
* `kind` is `iso`, `floppy`, `folder` or `download` (and `zip`, as built).
* `format` is `iso9660`, `iso9660+joliet`, `fat12`, `fat16` or `folder`
  (and `zip`).
* `parts` lists every image of a multi-image source (path, size, md5); an
  image from a ZIP is `<zip path>!<member>`. For a known disk set (§3)
  `imageMd5Known` is true, with no `imageMd5`.
* A download of a copy made of several images records its first image's
  `url` and `finalUrl`, and `--md5` replaces only that image's md5 and
  size.
* `from` is the source path; `zip!member` for an archive member (ARJ too:
  `SWSE1.ARJ!ANTSW.DLL`, and `SWSE2.ARJ+SWSE2.A01!JAWAS.IMX` for a member
  that spans volumes); or `alias:<package path>` for a fix-up copy. A
  KWAJ-expanded file's is its compressed name (`PLANETS.AD_`).
* `verified` means what it means today: `image`, `files`, `partial` or
  `none`. The manifest covers the installed files. Every ZIP member's CRC-32
  is always checked, and so is every ARJ segment's. SZDD has no checksum: a
  damaged SZDD file that still expands to its size is caught only by the
  manifest (3), and not at all under `--no-verify`. Neither has KWAJ
  (§8.8).
* The record is UTF-8 JSON whatever the source holds: every name arrives
  as UTF-8 (code page 437 decoded for FAT and ARJ names, FAT volume labels
  and ZIP member names that are not UTF-8; Latin-1 for ISO-9660 names and
  the volume id; UCS-2 for Joliet), and the one string escaper both records
  are written with (`minijson.h` `json_escape`) writes U+FFFD for any byte
  that still does not start a valid UTF-8 sequence.
* Deluxe's `<win>\import.json` stays version 1, unchanged.

## 6. The merged catalog

`catalog-win.json` keeps `"version": 1`, with only additive changes. The
generator becomes `adimport 1.1` (`adimport 1.2` since the covers work,
COVERS.md §2.7; `adimport 1.3` since the sixth release, for `abi` and the
`intermission` recipe). The seventh release added `screen` and the
`ad2kwaj` recipe under the same `adimport 1.3`, so every entry of the six
releases before it is byte for byte what it was.

* **Order.** Installed packages in registry order. Within a package, its
  module dirs in registry order. Within a dir, `*.AD` sorted as today.
  Deluxe's order is exactly today's: `AD40`, `ENGINE\STARRYNI.AD`,
  `CLASSIC`. For every other package, `ENGINE\*.AD` is scanned too, which
  gives `ad10.starryni`. Since the sixth release every package but Deluxe
  lists a dir's `*.AD` and `*.IMX` sorted together (Deluxe's three places
  stay `*.AD` only), and `WINDOWS` is never scanned.
* **Lane** comes from the header (PE32 → `pe32`, NE → `ne16`), never the
  folder. `AD10TH` mixes 16 pe32 and 29 ne16 modules. An Intermission module
  is NE too, so `ne16`; its `abi` field (below) tells it apart.
* **id**: Deluxe keeps `ad40.<base>` (pe32) / `classic.<base>` (ne16). For
  every other package it is `<package>.<base>`, where `<base>` is the
  lower-case file stem. A second file with an id already taken is skipped
  and logged, as today. This does not happen in the corpus.
* **moduleName**: the existing display-name rule (importer README: PE
  `VERSIONINFO` FileDescription, NE resource 2000/20, then STRINGLIST 128,
  then the file name), trimmed at both ends. Simpsons has
  `"Grampa's Wisdom "` and `"Snowball I "`, and After Dark 2.0 began 15 of
  Star Trek: The Screen Saver's 16 names with a space (`" Brain Cells"`,
  which sorted them first in its own list). Then `nameOverrides` apply. Only one exists:
  `ad10` `TOAST2K.AD` → `Toasters 2k (early build)`. It is an earlier build
  that `SETUP.INF` never installs, and without the override it would share
  the name of the installed `TOASTER2.AD`. (Since the sixth release, `swse`
  has fourteen more, below, and since the seventh `startrek` one.)
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
* **Intermission modules** (`swse`, since the sixth release). An NE file
  is one when it exports `SAVERINIT` and `SAVERDRAW` but neither `MODULE`
  (which makes it an After Dark module, whatever else it exports) nor
  `SETCURRSAVER`, and its file name does not start with `IMXX_`: the rules
  of Intermission's own reader (ABI.md §3.8.2), with the exports found by
  name, without case, exactly as the ne16 lane tells the two kinds apart
  (§7.5). An NE file the lane would refuse (`IMXX_*`, `SETCURRSAVER`, one
  of the pair alone, a reader exporting `SAVERMAIN`, or none of these
  exports) is left out of the catalog, its reason logged. The ABI is
  decided by the exports, never by the extension. An Intermission entry
  differs from an After Dark one in five fields:
  * `moduleName`: no resource holds an Intermission module's name, so the
    catalog starts from the file stem and the registry's `nameOverrides`,
    keyed `SAVER/<FILE>`, give the names each module's `SAVERINIT` returns
    (also its section of `SWSE.INI`): `BATTLES` Space Battles, `BIOS`
    Character Biographies, `BLUPRINT` Blueprints, `CANTINA` Cantina,
    `HYPERSPC` Hyperspace, `ICLOCK` Imperial Clock, `JAWAS` Jawas, `POSTERS`
    Poster Art, `RCLOCK` Rebel Clock, `SABRDUEL` Lightsaber Duel, `STORYBRD`
    Storyboards, `SWTEXT` Scrolling Text, `TRENCH` Death Star Trench,
    `VADER` Darth Vader. None collides with an After Dark name.
  * `about` is `""`, with no `credits`: the modules hold no text to show.
  * `controls`: one button, `{"index": 0, "name": "Configure...", "kind":
    "button", "type": "button"}`, when the module exports `SAVERDLGPROC`
    (all 14 do). It runs the module's own settings dialog
    (INTERACTION.md §6.1); the name is Intermission's own "Confi&gure..."
    without the mnemonic.
  * `entry` is `"SAVERDRAW"`.
  * `abi` is `"intermission"`, a new optional field written only for these
    entries, after `md5`/`sameAs`. Absent, it means the After Dark module
    ABI, so every After Dark entry is laid out exactly as before.

  Ids are `swse.<stem>` (`swse.vader`), paths
  `packages/swse/SAVER/VADER.IMX`. With all six releases installed the
  catalog lists 216 modules, 73 of them `sameAs` an earlier entry (`swse`
  adds none: its bytes are its own).
* **After Dark 2.0 modules** (`startrek`, since the seventh release) are
  Classic entries like any other: NE files exporting `MODULE`, lane `ne16`,
  entry `MODULE`, their controls read from the same type-1000 records (40 in
  all; `startrek.mission` has none), `needs` `AD_MOD` and `AD_RSRC`
  (Sounder: `AD_SND`). Ids are `startrek.<stem>` (`startrek.final`), paths
  `packages/startrek/AFTERDRK/FINAL.AD`. Two controls are buttons:
  Communications' **Edit Custom...** (index 3) and Sounder's **Sounds..**
  (index 2) (INTERACTION.md §1.6). What the release adds:
  * One name override, `AFTERDRK/PLANETS.AD` → Planetary Atlas: the name
    resource says `" PlanetaryAtlas"`, and the module's own About heading
    and Berkeley's later `PREVIOUS.INF` (After Dark 4.0 Deluxe's) say
    Planetary Atlas. The names: Brain Cells, Communications, Final Exam,
    Final Frontier, Horta, Ion Storm, The Mission, Ship Panels, Planetary
    Atlas, Scotty's Files, Sickbay, Sounder, Space, Spock, Tholian Web,
    Tribbles. None collides with another release's.
  * **After Dark 2.0's About texts** (the registry's `ad20` rules,
    `catalog.h` `ad20_about`). The last line of 15 of them, the registrant
    stand-in "Berkeley Systems Authorized User." (After Dark 2.0 showed its
    owner's name in its place), is dropped with the blank lines before it;
    and a line break with a space before it and a lower-case letter after
    it, a sentence wrapped by hand (six in the release), is joined into that
    space. The rules are the package's alone: applied to every package the
    join would change four entries of other releases (`classic.dominoes`,
    `classic.om`, `ad32.mmas`, `simpsons.lisa`), which stay as written.
  * `screen` is `"640x480"`, a new optional field written on every
    `startrek` entry, last, and on no other. Some of the modules compose
    a fixed 640×480 scene: on a larger screen The Mission draws at the top
    left beside a grey band, and Final Exam and Sickbay sit small in the
    middle; the others (Ship Panels and Scotty's Files among them) lay out
    for whatever screen they get, and all 16 carry the field so that the
    release looks as it did at 640×480. A front-end gives such a module that
    screen whatever its own resolution setting, scaled to fit, as it gives
    an Intermission module its 640×480 by its ABI; the catalog's screen
    comes before the ABI's. The saver reads the field as `<w>x<h>`, 1 to 5
    decimal digits either side of `x` or `X`, each axis 1..8192 and at most
    4096×4096 pixels in all (the frames its stream parser reads back);
    anything else, or no field, is no screen of its own, and the ABI's rule
    applies as before (`scr/README.md`, "Emulated screen"). However many
    sizes a catalog gives, the saver's desktop seed takes at most two own
    screens a window, 640×480 first, then the smallest (INTERACTION.md
    §8); a first module of another size starts on black.

  With all seven releases installed the catalog lists 232 modules, still 73
  of them `sameAs` an earlier entry (`startrek` adds none: none of its files
  is byte-identical to another release's), and the `packages` list begins
  with `startrek` (1992-11).
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
| packaged, an Intermission module (`swse`; §7.5) | module dir (`C:\SAVER`) → engine dir | engine dir | no AD3 bridge: Intermission's reader `ENGINE\IMIMXPLY.IMQ`, or the native reader | none: the modules build their own |
| packaged, After Dark 2.0: the module dir holds `AD_MOD.DLL` (`startrek`; below) | module dir → engine dir | engine dir | native AD3 bridge with `ENGINE\AD_SND.DLL`, AD_SND 1.0 | none: After Dark 2.0 kept none, and no module asks for one |
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
* **After Dark 2.0** (`startrek`, since the seventh release). Its 16
  modules take the AD3 path unchanged: After Dark 2.0's host, `AD.EXE`
  2.0b, drove them with the same entry point, messages and blocks as
  OLDMOD16 (ABI.md §3.9), and in the lane survey's research prototype the
  native bridge drew byte-identical streams for all 16 to the real
  OLDMOD16's (both over After Dark 4.0 Deluxe's AD_SND 4.0). The package
  ships no OLDMOD16, so the native bridge runs them, over its own AD_SND 1.0
  (§7.4). What else differs is the host's side, and every difference is a
  rule by file, never by package id: the module folder holds `AD_MOD.DLL`,
  After Dark 2.0's module library, which no other release has
  (`host/ne16/package.hh` `after_dark2`).
  * **`AD_PREFS.INI` seeds** (profile seeds, read as seed ⊕ file and never
    written out; `host/win16/dos16.hh` `seed_after_dark2`): `[After Dark]
    Path=C:\AFTERDRK\`, the installer's own spelling, where `AD_MOD.DLL`
    opens `ST_RES\ST_RESDB.DLL` and `ST_SND.DLL`, AD_SND 1.0 lists its sound
    drivers (`*.DRV`) and Sounder finds `SOUNDS\*.WAV` (without it every
    module but Sounder stops with "File not found.", and Sounder with "Can't
    find any .WAV files to play!"); and `[Sound] SoundDriver=AD_MME.DRV`,
    the "Multimedia Windows Sound (Windows 3.1)" driver, which plays through
    MMSYSTEM (AUDIO.md §2.12). The disks' own `AD_PREFS.INI` named
    `AD_MPT.DRV`, the PC speaker's driver, whose `SPALETTE.DLL` busy-waits
    on the timer chip's port 0x40, which the runtime does not have: with
    sound on, 11 of the 16 modules hung at their first sound. So the recipe
    never installs that file (§4.3); a package copy could not have carried
    the settings anyway, since the runtime's empty virtual
    `C:\WINDOWS\AD_PREFS.INI` hides a lower-layer file, and a file's key
    would win over the seed. The modules' own writes land in the state
    overlay, `<state>\startrek\WINDOWS\AD_PREFS.INI`: AD_SND 1.0's `[Sound]
    Mute`, written at each load of a module that wants sound (15 of the 16:
    all but Ion Storm), Communications' `[Communications] MessageText` and
    Sounder's `[Sounder] SoundPath`.
  * **No AD palettes.** After Dark 2.0 stored none: `AD.EXE` built its four
    in code (the same bytes as ADTASK.DLL's 5000/1..4, ABI.md §3.9). None is
    computed here, and none is needed: no Star Trek module makes a palette
    request (each builds its own palettes), and one would fail with 7
    (§7.4). The lane's note says so: "After Dark 2.0 modules make no
    palette requests (one would fail with 7)".
  * **Result 5 is the module's wake.** `AD.EXE` 2.0b took a DRAWFRAME
    result of 5 as its wake: it posted itself its wake message, 0x7EE
    (ABI.md §3.9). Final Exam returns it when a mouse move ends its exam.
    The lane reports an After Dark 2.0 module's 5 as the module's wake (the
    status's `ADWS_WAKE`, which the saver acts on as when the user wakes it;
    INTERACTION.md §3.4, §5.2); for every other module 5 still ends the run
    as the module's error, as AFTERDAR.SCR treated it (ABI.md §3.1).
  * **Guest disk**: the module dir is `C:\AFTERDRK`, as for every After
    Dark module: the installer's own folder, which `Path` names.

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
code. Since the seventh release it runs After Dark 2.0's modules too
(`startrek`): their host, `AD.EXE` 2.0b, sent them the same messages with
the same blocks (ABI.md §3.9), and like the AD 3.x discs theirs ships no
OLDMOD16; the bridge then stands in for `AD.EXE`.

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

      Since the seventh release five of them are required, `adwSoundInit`,
      `adwSoundCleanup`, `adwSetVolume`, `adwSetSoundMute` and
      `adwStopSound`, with one volume pair: `adwGetSystemVolumes` +
      `adwSetSystemVolumes` (AD_SND 3.0.3, 3.2 and 4.0), else
      `adwSavePreviousVolume()` + `adwRestorePreviousVolume()`, which take
      no arguments (AD_SND 1.0, After Dark 2.0's, which has no other pair:
      `AD.EXE` 2.0b called them where OLDMOD16 calls the first, ABI.md
      §3.9). The second pair is looked up only when the first is
      incomplete, so an AD_SND 3.x or 4.x sees exactly the calls it saw
      before. Id 3 is any required entry missing: one of the five, or both
      pairs incomplete.
   2. Call `adwSoundInit(0, buf)` and `adwGetSystemVolumes(&saved)` (AD_SND
      1.0: `adwSavePreviousVolume()`).
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
   5. 0x0E, 0x11 and 0x12 pass through to the lane as today. (So does 5,
      which the lane takes as an After Dark 2.0 module's wake, §7.3.)
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
   palettes is ever embedded in the host: they are After Dark data. After
   Dark 2.0 (`startrek`) has no palette source (its `AD.EXE` computed its
   four); none is computed here, and no module asks for one (§7.3).
6. **Unload**:
   1. `MODULE(3)` if initialized.
   2. `RestoreDC`, `FreeLibrary(module)`, `DeleteObject(hDrawRgn)`.
   3. `adwStopSound`, `adwSetSystemVolumes(saved)` (AD_SND 1.0:
      `adwRestorePreviousVolume()`), `adwSoundCleanup`,
      `FreeLibrary(AD_SND)`. The restore runs only after its save did (a
      load refused with id 3 calls `adwStopSound` and `adwSoundCleanup`,
      each only if resolved).
   4. Free both blocks.
7. **Machinery.** Every guest call goes through `Runtime16::call_far`, so
   faults, `Catch`/`Throw`, virtual time and the census behave as for
   OLDMOD16.

The only side-effect differences from OLDMOD16 are OLDMOD16's own
instructions, API calls and memory. These nudge virtual time through the
instruction-cost model (`ADMIPS`/`ADAPICOST`) and shift selector numbers. So
the oracle compares both bridges with `ADMIPS=0` (§9).

### 7.5 Intermission modules (ne16, `swse`)

Star Wars Screen Entertainment's modules are Delrina's Intermission
modules, not After Dark's; their protocol is ABI.md §3.8. They run in the
same lane as the After Dark 16-bit modules: the Win16 runtime and its knobs,
the frame loop with its long calls and draw budget, input and status, the
desktop seed, the small-screen guest display, the audio pump and the
configure scaffolding are the same whichever protocol drives a module. The
lane has two module protocols (`host/ne16`, `Protocol16`): `ad3`, the After
Dark path of §7.3 and §7.4 (OLDMOD16 or the native AD3 bridge), and `imx`.

* **The kind comes from the exports**, never from the folder, the
  extension or the package id: `MODULE` → `ad3`, even when the file also
  exports Intermission's entries; else `SAVERINIT` and `SAVERDRAW`,
  without `SETCURRSAVER`, in a file not named `IMXX_*` → `imx`
  (Intermission's own rule). A reader (`SAVERMAIN` alone) and an NE file
  with none of these exports are refused with exit 1. The importer gives
  a catalog entry its `abi` by the same rule, `MODULE` first (§6), so the
  catalog and the lane agree on every file. Exports are matched by name,
  without case; the IMX exports are in the NE non-resident names table,
  which the loader searches too. `ADNE16KIND=auto|ad3|imx` overrides the
  kind (default `auto`).
* **What replaces what.** `INTERMIS.EXE` is an NE application with its own
  message loop and control panel, and the Win16 runtime runs libraries only,
  so the lane does INTERMIS's part in C++ through the Win16 thunks, as it
  does AFTERDAR.SCR's: one zeroed `IMINFO` record, the message sequence of
  ABI.md §3.8.4 with every drawing call between `GetDC` + `SaveDC` and
  `RestoreDC` + `ReleaseDC`, and between two calls a pump of the guest's own
  posted messages and due timers, as INTERMIS's loop dispatched them
  (`user16_dispatch_guest`, `host/win16/README.md`). INTRMLIB itself loads
  as the modules' import, and its LibMain registers the dialog control
  classes; none of its engine exports (the module list, idle detection,
  the input hook, the engine palettes) is called. A module whose QUERY asks
  for an engine palette (the record's `+0x53` non-zero, ABI.md §3.8.4;
  none of the 14 does, their `SAVERINIT` writes 0) got from INTERMIS a copy
  (`IMCOPYPALETTE`) of the palette INTRMLIB's `CANISTART(1)` had made at
  start-up from its `CLUT`, `HSV` or `PRIM` resource on a palette display;
  the lane runs no `CANISTART`, whose other work is INTERMIS's, and makes
  that palette itself the same way (`CreatePalette` of the resource,
  through the thunks).
* **Pacing.** INTERMIS called the saver back to back while the machine was
  idle, and the modules pace themselves (ABI.md §3.8.5): most by
  `GetTickCount`, the two clocks by the DOS time, and Hyperspace's stars,
  Scrolling Text's scroll and Death Star Trench one step per call. The
  lane's frame loop, draw budget and long calls apply as they do to an After
  Dark module, with two differences (`host/ne16/lane.hh` "Pacing"):
  * *Pixel cost.* A pixel a blit or fill writes costs the budget
    `ADNE16IMXPIXCOST` (default 4) instead of After Dark's `ADPIXCOST` (2):
    SWSE draws its scenes as full-screen DIB stretches through GDI, the
    slow path WinG was made to avoid, which on the 486s of 1994 ran several
    times slower than After Dark's figure. Each knob changes only its own
    modules.
  * *Carried overruns.* What a pass that completes within its frame does
    beyond what the frame's budget had left is carried into the next
    frames, paid back from their budgets first, and a frame whose whole
    budget goes to it makes no call (it still delivers input, pumps the
    audio and runs the message loop). What is owed stays below six budgets
    and each frame without a call pays a whole one back, whatever its pumps
    did, so at most five frames in a row make none.

  So a module that steps once per call runs at the pace of the lane's
  25-MIPS model: Death Star Trench at 16 passes a second, where one pass a
  frame had run it at 60 (and the carry alone, at After Dark's pixel cost,
  at 31), and Scrolling Text at 19, its crawl taking about 11.4 s where one
  pass a frame had shown it in 3.65 s. Those are seconds of 60 presented
  frames: a streamed run is
  stepped once per frame the front end presents, so the settings preview,
  at 30 frames a second, shows about half as many passes. A pass that runs
  past its frame's deadline is paced by the deadline instead (Long calls)
  and carries nothing, neither in the frames that end inside it nor in the
  one it returns in; the passes that frame runs after it returns are
  carried as usual. After Dark modules carry nothing and keep their pixel
  cost, so their streams stay as they were; `ADNE16IMXCARRY=0` turns the
  carry off.
* **The reader.** By default the real `IMIMXPLY.IMQ` from the engine dir
  (the guest's `C:\WINDOWS\SYSTEM\IMIMXPLY.IMQ`), as the real OLDMOD16 runs
  where a package has it: it is the host side of the module protocol that
  LucasArts shipped, and running it keeps its quirks (the in-place
  40-character name cut, the 999 sentinel, its refusals). A native C++
  reader, IMIMXPLY's dispatch rewritten, is the oracle and the fallback when
  the IMQ is missing: in the host survey's research prototype the two drew
  byte-identical streams for all 14 modules. `ADNE16READER=auto|imq|native`
  chooses (default `auto`: the IMQ when it is there).
* **The guest's disk:**

  | Guest | Lower (read-only) | Upper |
  |---|---|---|
  | `C:\SAVER` (the current directory) | the module dir | `<state>\swse\SAVER`, or memory |
  | `C:\WINDOWS` | `<package root>\WINDOWS` when it exists (§4.1), and the virtual seed files | `<state>\swse\WINDOWS`, or memory |
  | `C:\WINDOWS\SYSTEM` | the engine dir | none |
  | `C:\WINDOWS\TEMP` | — | memory (STRESS's temporary file, ABI.md §3.8.7) |
  | `H:\` | the host's drives, 8.3, read-only | — |

  `C:\SAVER` is where the installer put the modules (`INSTALL.DAT`'s
  `DestDir`), and what the modules and their settings name:
  `SWSE.INI [Scrolling Text] TextFile=C:\SAVER\SWTEXT.TXT`, INTRMLIB's
  default saver directory, the music and DLLs opened by bare name.
* **Seeds** (profile seeds, read as seed ⊕ file and never written out):
  `SYSTEM.INI [boot] display.drv=pnpdrvr.drv`; when the module dir holds
  `SWSE.DLL`, `SWSE.INI [technology] display.drv=pnpdrvr.drv`,
  `WinGFound=1`, `DibBlit=GDI`, so SWSE draws with GDI and the DIB driver
  and never looks for WinG (ABI.md §3.8.6); `ANTSW.INI [Intermission]
  Volume` = the engine volume (0–100) when sound is on and 0 when it is off
  (Intermission's "Off": no effects and no music), and `Saver
  Path=C:\SAVER`. Like the `WINDOWS` layer, these are rules by file, never
  by package id.
* **No WinG, no `DIB.DRV`.** Neither is installed (§4.3). The Win16 runtime
  has its own DIB driver, and it supplies what these modules need beyond
  the After Dark surface: `GetTempFileName` creating its file,
  `AccessResource`, resources counted as Win16 counted them, `MulDiv`,
  `PaintRgn`, `CreateHatchBrush`, `CreateDIBPatternBrush`,
  `GetSystemPaletteUse`, `SetHandleCount`, `GlobalWire`/`GlobalUnWire`,
  `GetMenu`, `GetWindowTask`, `GetNextWindow`, `EnumChildWindows`, INT 2Fh
  1684h answering that no VxD is there, and `ChooseFont` in configure mode
  (`host/win16/README.md`, "Intermission modules").
* **Status and input.** An Intermission module is never interactive
  (IMIMXPLY never sets the input flag, ABI.md §3.8.3), so the status source
  stays 0. The lane gives it no key messages: a `KEY` line only changes the
  key state `GetAsyncKeyState` reads, because a key the saver does not wake
  on (Shift, Ctrl, Caps Lock, Num Lock) would otherwise reach SWSE's
  `USERABORT` through `GetInputState` and stop a module's start for good.
  What SWSE's `FORCETOWAKE` posts to its task is removed and counted by the
  pump, and is not a wake: the saver's own wake rules decide
  (INTERACTION.md §5.2).
* **Buttons.** `--configure <module> --button 0` runs Intermission's
  Configure: `SAVERMAIN` 10, 7, 8 and 11 (INTERMIS's control panel sent no
  7), and 8 opens the module's own dialog, whose settings land in the state
  overlay's `SWSE.INI` (INTERACTION.md §6.1, §7).
* **Sound.** Effects go through the Win16 runtime's `sndPlaySound`, as After
  Dark's do; music through MEMMIDI's MIDI output on multimedia timers
  (AUDIO.md). The engine volume reaches both: the effects through the
  `ANTSW.INI` Volume seed above, from which SWSE sets `waveOutSetVolume`,
  and the music through the MIDI bus, which the lane sets from it as After
  Dark's `midiOutSetVolume` would (50 is half amplitude), standing in for
  the Windows mixer's synthesizer line: Intermission itself set only the
  effects' volume.
* **`--capabilities`** gains `abis=afterdark,intermission`, the module ABIs
  the build's lanes run (pe32: `afterdark`; ne16: `afterdark` and
  `intermission`).

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
* Names and the volume label are decoded from code page 437 (the label:
  the root's first label entry, trailing spaces dropped, its case kept, a
  leading 0x05 meaning 0xE5 as in a name); the label is `import.json`'s
  `volumeId`.
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
  * Member names become UTF-8 as the central directory is read: as stored
    when general-purpose bit 11 is set (each byte that is not UTF-8
    becomes U+FFFD) or when they are UTF-8 without it (archivers outside
    Windows write that); any other name is code page 437, the
    specification's default and what Explorer and 7-Zip on an English
    Windows write. So images named `DISKüö1.IMG`, or `TREKü.IMG` beside
    `TREKö.IMG`, import as the known disk set.
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

* CAB (MSZIP/Quantum/LZX) and InstallShield 5 CABs. SZDD and KWAJ, the two
  COMPRESS formats of `_` files, were on this list for the five After Dark
  releases, which use neither; Star Wars Screen Entertainment uses SZDD, so
  it is §8.7 now, and Star Trek: The Screen Saver KWAJ, §8.8.
* The InstallShield 3 `.Z` archive `INS0762.LIB` (PKWARE DCL implode): it
  holds only the installer's own `DUNZIP.DLL`/`RESOURCE.DLL`.
* An `INSTALL.INS` bytecode interpreter: the placement is baked into
  `ad3zip`. Likewise no interpreter of Presage's `INSTALL.DAT`: the
  placement is baked into `intermission`; nor of Microsoft Setup's
  `ST_NSTLL.INF` and its MS-Test script `AD_NSTLL.MST`: `ad2kwaj` is baked
  too.
* A `SETUP.INF` parser: the `ad10` fix-ups are baked.
* HFS and StuffIt (Mac halves, §12).

### 8.6 ARJ (new with the sixth release: `arj.h/.cc`)

Star Wars Screen Entertainment's `SWSE1.ARJ` (19 members) and its
four-volume `SWSE2.ARJ` + `.A01`–`.A03` (15 members, three of them split
across two volumes each) are ARJ 2.x archives: DOS host, archiver version 4,
minimum version 1, every member method 1, bare 8.3 names. The reader ports
the research reference `research/win/pkg/swse/arj.py`: its container code
is our own, and its decoder a modified version of UNARJ's (`DECODE.C`, with
`UNARJ.C`'s bit reader; © Robert K. Jung, whose terms
`THIRD_PARTY_LICENSES.md` quotes). It accepts only what ARJ 2.x writes:

* **Container.** A main header at byte 0, then local headers one after
  another, each followed by exactly its data, then the end marker
  (`60 EA 00 00`; nothing after it is read). Every basic and extended
  header's CRC-32 is checked; header sizes 30–2600, at most 16 extended
  headers, minimum version ≤ 3, flags among VOLUME, EXTFILE, PATHSYM and
  BACKUP (a garbled, password-protected archive is refused by name), file
  type 0 or 1, methods 0–4, bare member names that pass the name rules (no
  `/`, `\`, `:`, `..` or DOS device), at most 65,535 members, no two whose
  names Windows takes for one file (compared as the file system compares
  them, letters outside ASCII included: code page 437's `ü` and `Ü` are one
  name). The volumes are held in memory (at most 64 MB each; the disc's
  largest is 1.4 MB).
* **Volumes.** A member that spans volumes is its segments: the last member
  of one volume carries VOLUME_FLAG, and the first member of the next
  continues it (EXTFILE_FLAG, with the byte position it resumes at). Every
  rule of that is checked: the volumes' own VOLUME flags, the continuing
  member's name, its position, the order; the recipe follows a chain by the
  volume names ARJ gives (`X.ARJ`, `X.A01`, … `X.A99`). Each segment is
  compressed on its own and decoded on its own.
* **Decoding.** Methods 1–3 are UNARJ's LZH (one decoder; the number only
  records the encoder's effort), with the bounds UNARJ lacks: table sizes,
  code lengths (≤ 16), zero runs and constant symbols checked before use; a
  Huffman table must be a complete code (Kraft sum exactly 1, which every
  one of the disc's 518 blocks meets); a match may not reach before the
  start or beyond the 26,624-byte window; no stream may produce more than
  its recorded size; the bit reader's look-ahead may read at most 2 bytes
  past a segment (exactly what the disc's streams need). When the recorded
  size is reached, the block being decoded may not still hold symbols;
  what follows it (further blocks, trailing bytes) is never read, as UNARJ
  never reads it (all 37 of the disc's segments end on a block boundary).
  Method 4 ("fastest") has the same output and window bounds and simply
  stops at the recorded size; method 0 is a copy.
* **Checks.** Each segment's size and CRC-32 (zlib's) as it streams; ARJ
  stores no whole-file CRC. A damaged member is a corrupt source (2), never
  a verify failure, as for ZIP. Nothing bounds the expansion ratio (a block
  of constant tables costs 0 bits per symbol); as for deflate, the recorded
  sizes, the staging budget and the manifest's sizes bound what is written.

### 8.7 SZDD (new with the sixth release: `szdd.h/.cc`)

Microsoft COMPRESS's "A" format, the `*.XX_` files: Star Wars Screen
Entertainment's `STRESS.DL_` and its MIDI files (the disc has 24, of which
the recipe reads five). A 14-byte header — magic `SZDD 88 F0 27 33`, mode
`'A'`, the dropped extension character, the expanded size — then LZSS: a
flag byte gives, least significant bit first, for each of the next eight
tokens a literal byte (1) or a two-byte match (0: a 12-bit window position
and a length of 3–18) into a 4096-byte window that starts filled with
spaces and is written from `0xFF0` (eight of the disc's files read those
initial spaces: the four `GM_*.MI_`, `STRESS.DL_`, `WING.DL_`, `WINGDE.DL_`
and `WINGPAL.WN_`). Windows' own `EXPAND.EXE` gives the same bytes. KWAJ
and the other modes are refused, not guessed. The expander is strict:
exactly the header's size, no match past it, no whole byte left after the
last token (all verified 0 over the disc's 24 files). SZDD has no checksum: a
damaged file that still expands to its size is caught by the manifest (3),
and not at all under `--no-verify`.

### 8.8 KWAJ (new with the seventh release: `kwaj.h/.cc`)

The second format of Microsoft's COMPRESS, which the Microsoft Setup
Toolkit 2.0 expanded on the way: 59 of Star Trek: The Screen Saver's 62
files (2,710,666 bytes, expanding to 5,882,366), every one method 3 (LZ +
Huffman) with header flags 0, so the data starts right after the 14-byte
header. Neither 7-Zip nor Windows' `EXPAND.EXE` decodes it. The reader is
written from the format's public description, through the research
reference `research/win/pkg/startrek/tools/kwaj.py` (which follows
libmspack's documentation of the format); it holds no third-party code. The
reference, libmspack's `msexpand` and Deark 1.7.3 expand all 59 files, and
117 synthetic samples of every method and header flag, to the same bytes,
and every expanded size and date equals `ST_NSTLL.INF`'s. The reader accepts
only what the release uses:

* **Header.** Magic `KWAJ 88 F0 27 D1`, then the method, the data offset
  and the header flags (little-endian words). Method 3 only, flags 0 only,
  data offset 14 only: another method, any flag (the optional header
  fields), another offset and an SZDD file are refused by name, and the
  SZDD reader refuses a KWAJ file likewise.
* **Tables.** The stream is read most significant bit first: six 4-bit
  table types, each 0–3 (the sixth must be 0, as it is in all 59 files),
  then five canonical Huffman codes, MATCHLEN (16 symbols), MATCHLEN2 (16),
  LITLEN (32), OFFSET (64) and LITERAL (256), whose code lengths are fixed
  (type 0: 4, 5, 6 or 8 bits), delta-coded (1: `0` the same, `10` one more,
  `11` then 4 bits; 2: a 2-bit selector, 3 meaning 4 bits, else the previous
  length plus the selector minus 1) or plain (3: 4 bits each). Every length
  is 0–16, and every code must be complete (Kraft sum exactly 1, as all 295
  of the release's are), decoded bit by bit.
* **Tokens.** A 4096-byte window that starts filled with spaces (18 of the
  files copy 389 bytes from it). A token is a MATCHLEN symbol, or a
  MATCHLEN2 symbol right after a literal run shorter than 32: a symbol s > 0
  is a match of s + 2 bytes (3–17) at distance (OFFSET symbol << 6 | 6 raw
  bits), 0 meaning 4096, copied byte by byte (so it may overlap itself);
  s = 0 is a run of LITLEN + 1 LITERAL symbols.
* **The end.** KWAJ records no length and has no checksum: the stream
  simply ends. A token counts only once it is complete. When a read needs a
  bit past the end, the file ends cleanly if the unfinished token began
  fewer than 8 bits before the end: the encoder pads its last byte with
  1-bits, which in 6 of the 59 files read as the start of a token that must
  produce nothing. A token cut 8 or more bits before the end ("the
  compressed data ends inside a token (N bits before the end)"), or a
  stream cut inside its tables, is an error.
* **Bounds.** The output streams in chunks of at most 64 KiB and never
  passes the caller's `max_size`, checked before each token's bytes
  (`KwajTooLarge`); a match costs at least 8 bits and makes at most 17
  bytes, so it is at most 17 times the input anyway.
* **Checks.** Nothing in the file checks the data. A damaged table or token
  is a corrupt source (2). A damaged file that still decodes (a flipped
  literal, or a file cut on a token boundary, which gives a clean prefix)
  is caught only by the manifest (3; before a byte is written when its size
  differs, §4.3), as with SZDD, and not at all under `--no-verify`.

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
* **The sixth release** (`swse`), the same way:
  * Importer, synthetic: two new suites, `import.arj` (stored round trips;
    every damaged or foreign header refused with its message; a member
    split over three volumes and every joining rule broken; thirteen fixed
    method-1/method-4 vectors from the research encoder, which the Python
    reference and 7-Zip decode alike, cut, bit-flipped and resized; streams
    crafted bit by bit to break each decoder bound; no encoder, since the
    decoder derives from UNARJ, whose terms are for programs that are not
    ARJ archivers, §8.6) and `import.szdd` (round trips, two fixed vectors
    that Windows'
    `EXPAND.EXE` expands the same way, the header, cut and overlong data).
    `import.packages` gains a made-up Presage install (a synthetic
    `INSTALL.DAT`, stored and LZH members cut across ARJ volumes, SZDD loose
    files, decoys that must never be read) as a folder, an ISO, five 1.44 MB
    floppies in either order and a flat ZIP: identification, every install
    disk needed, the exact file set and `from` forms, the invariants, damaged
    volumes (2) and an SZDD literal changed (3). `import.catalog` tells IMX
    modules apart by their exports as the lane does (`MODULE` first; a file
    with `SETCURRSAVER`, named `IMXX_`, a reader or one of the pair alone
    left out and logged; no `SAVERDLGPROC`; lower-case names) and checks
    `abi` (last, IMX only).
  * Importer, real images: `import.pkg_real` takes five images and the swse
    CD mounted by Windows, checks the baked recipe against the real
    `INSTALL.DAT` (the loose files are its lines 4, 10–13 and 37; every other
    line is an archive, another MIDI set or a file never read), and merges
    all six releases: 216 modules, 73 `sameAs`. `import.download_real`
    fetches swse's three copies (the ISO and the Redump BIN verified
    `image`, the ZIP `files`).
  * Lanes: before any lane or Win16 change, 300-frame `FBHASH` streams of
    all 202 After Dark catalog modules were recorded in three runs each
    (sound off, sound on, and a seeded desktop) with the five-release host
    (`research/win/pkg/swse/baselines`, gitignored, with its `compare.py`).
    A change meant to keep behaviour must reproduce every stream; a
    deliberate fix that changes an After Dark stream is explained module by
    module. The Win16 work for this release changed three modules' streams
    that way, and those streams were accepted as the new baselines
    (`baselines-w`, recorded in its `ACCEPTED.json`); every other stream,
    and every build since, gives 0 differences:
    * `ad32.ybyh` (You Bet Your Head, After Dark 3.2), all three runs from
      frame 54. `FreeResource` now frees a resource when its use count
      reaches 0, as Win16 did, and the next `LoadResource` reads it afresh;
      the loader of YBYH's engine, `ADXPL300`, frees its resource copies,
      so the guest's memory and instruction counts change and its pacing
      moves one phase: the same animation, a phase apart. The same build
      with `FreeResource` keeping the blocks reproduces the old streams,
      and with `ADMIPS=0` the two give identical streams.
    * `classic.artist` and `ad10.artist` (Artist, in Deluxe and the 10th
      Anniversary), the sound-on run, frame 202 only. `EnumChildWindows`
      now calls its callback for each window, the desktop's children being
      the top-level windows; Artist's callback reads two window titles
      (`GetWindowText`), whose virtual time moves one pixel of that frame.

    The Intermission side is checked by the ne16 lane's own suites:
    `ne16.unit` (the kind rule, both readers and the IMX protocol on
    host-implemented modules) and `ne16.swse` (the 14 real modules from an
    imported package; BUILDING.md, "Test").
* **The seventh release** (`startrek`), the same way:
  * Importer, synthetic: a new suite, `import.kwaj` (`tests/kwaj_builder.h`,
    which compresses nothing: eight fixed vectors, made-up data the research
    encoder wrote, and six streams written token by token — 16-bit codes;
    symbols without codes and MATCHLEN2's symbol 0; distance 4096; a 70 KB
    file in two chunks; the disks' padding quirk; MATCHLEN2 after a run of
    31 literals and MATCHLEN after one of 32 — each expanded to the same
    bytes by the research reference, libmspack and Deark, and pinned by its
    md5; every refused header and table; the end rule, including a partial
    literal run at the end, of which nothing is output; `max_size` and
    `KwajTooLarge`; the 64 KiB chunks; every single-bit flip of the vectors
    of 1 KB or less, 35,416 of them: an error, other bytes, or (a flipped
    padding bit) the same bytes; never a crash or more than the bound). `import.fat` reads every image from memory as
    well as from its file, and ZIPs of floppy images (a scan beside them, a
    member with no boot sector, the bound, a damaged or password-protected
    member), and decodes a code page 437 volume label (also one whose
    leading 0xE5 is stored as 0x05); `import.zip` reads member names as
    UTF-8 (bit 11; UTF-8 without it kept; else code page 437: `ü` and `ö`
    two names, `ü` and `Ü` one; a byte that is not UTF-8 under bit 11 read
    as U+FFFD). `import.packages` gains a made-up Microsoft Setup install of
    KWAJ files (made-up modules with After Dark 2.0's leading space in their
    names and its About stand-in, and decoys that must never be read, locked
    in a folder source) as a folder, its two floppies in either order, the
    ZIP of them, a flat ZIP and an ISO: identification (`SETUP.LST`'s
    title, its case and size, the tag file beside it), known disk sets and
    every refusal, every install disk needed, the exact file set and `from`
    forms, the `ad2kwaj` invariants, damaged KWAJ files (2 for a table or a
    header flag; 3 for a changed literal, and for a file cut to a clean
    prefix, whose size the manifest refuses before a byte is written), and
    the staging budget over KWAJ's expanded sizes. `import.catalog` checks
    the About rules (`ad20_about`, and their scope) and `screen` (last, on
    the package's entries alone); `import.pkg_download` fetches the
    two-image copies from its loopback server (the progress over both, a
    missing or damaged second image moving to the other copy, reuse, one
    image already on disk); `import.gui_model` lists seven releases.
  * Importer, real images: `import.pkg_real` finds the two disk images by
    md5 (loose, or in the ZIP they came in), imports them (27 files; 16
    Classic entries with `screen`, trimmed names, the Planetary Atlas
    override, the About rules and two buttons), checks the baked recipe
    against the disks' own `ST_NSTLL.INF`, KWAJ-expanded from disk 1 (each
    of the 27 rows is an INF line with the same disk, installed name and
    size, placed where its section installs it; the tag files are the INF's
    `[Source Media Descriptions]`; each of the other 34 lines is `SETUP.LST`
    or a file the recipe never reads), and merges all seven releases: 232
    modules, 73 `sameAs`, `startrek` first. `import.download_real` fetches
    both copies of the two images (each verified `image`) and checks every
    image URL with a Range request.
  * Lanes: `ne16.unit` gains a synthetic AD_SND exporting only AD_SND 1.0's
    entry set (no Berkeley bytes: the bridge's call sequences for a load, an
    unload and a button, its lookups, error 3), the seeds and their rule by
    file, and result 5 as an After Dark 2.0 module's wake, headless and
    streamed, every other result as before; `win16.unit` the seeds, Num
    Lock's toggle, `DlgDirList`'s `[-h-]`, the guest DOS's drives and
    per-drive directories (AH=0Eh/19h/3Bh/47h, `X:name`, `TF_FORCEDRIVE`,
    the 66/67-character boundary) and DIBs drawn into monochrome bitmaps,
    the single-entry rule on multi-colour DIBs included; `win32.unit` the
    configure script's `PICK`; `core.unit` and `core.e2e` the `NUMLOCK`
    line, `ADNUMLOCK` and `numlock=1`. `ne16.startrek` runs the 16 real
    modules of an imported package, Sounder's folder chosen through `[-h-]`
    and played by a later run among them; `ne16.interaction` picks Globe's
    map through `[-h-]` in Deluxe and 3.2 and loads it in a later run
    (BUILDING.md, "Test"). The saver's side (the catalog's `screen`,
    Num Lock) is in `scr/README.md`.
  * Before any lane or Win16 change, the tree's binaries and the 14 Star
    Wars Screen Entertainment modules' 900-frame streams were frozen
    (`research/win/pkg/startrek/baselines`); the 202 After Dark modules keep
    `baselines-w`. The Win16 work for this release changed the streams of
    three After Dark modules on purpose (nine streams: runs A, C and S of
    each), and they were accepted
    (`research/win/pkg/startrek/baselines-st`: `baselines-w` with those nine
    streams re-recorded, listed in its `ACCEPTED.json`); every other stream, the 14
    Star Wars streams included, and every build since, gives 0 differences:
    * `classic.mowin`, `ad32.mowin` and `ad10.mowin` (Mowin' Man, in Deluxe,
      After Dark 3.2 and the 10th Anniversary), all three runs from frame 9.
      `StretchDIBits` and `SetDIBitsToDevice` into a memory DC holding a
      monochrome bitmap now hand real GDI the DIB's own colours, as
      `SetDIBits` and `CreateDIBitmap` already did. Real GDI makes only the
      colour-table entry nearest white 1 (the first of equal ones) and
      every other entry 0 (`DIB_PAL_COLORS` entries through the DC's
      palette); with the key table's greys only hardware index 255 could be
      1, so every mask drawn that way came out empty. Mowin' Man's mower was drawn inside a white box;
      it is now transparent. (Without the fix Star Trek's Scotty's Files drew
      no blueprints, and The Mission no console lights.) The other 21 of the
      24 16-bit modules that take this path within 1800 frames are
      unchanged.

    Run A with `ADNUMLOCK=1` (the saver now passes the real Num Lock toggle
    to every 16-bit module) differed from `baselines-w` in those three
    streams alone: no module before this release reads Num Lock's toggle as
    it starts.

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
   * the project's status notes;
   * `tools/package.sh`'s dist README ("Deluxe only" wording; since rewritten for the five releases);
   * the scr status strings that count "After Dark 4 / Classic" (the scr
     workflow owns them). These are part of the user-requested wording
     pass.

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
  * pacing: still open. `TOILET` (`ad10`/`tt`) costs about 16 ms
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
  can be. (Since the seventh release one AD 2.x release is: Star Trek: The
  Screen Saver. After Dark 2.0 itself and More After Dark, where their
  disks use the same Microsoft Setup installer, which was not checked,
  would be further `ad2kwaj` packages: a registry entry with the setup
  title, the disks' tag files, the loose-file table and a manifest.)
* **Deferred on purpose:** AD 3.2's built-in Starry Night (above: it needs
  an NE application mapped as a library; Deluxe and the 10th Anniversary
  ship Starry Night as a module), `ECOLOGIC.DLL` in the catalog (above),
  and module file-dialog templates (`OFN_ENABLETEMPLATE`, logged and
  ignored by `win32/comdlg32.cc`; no module in the corpus uses one).
* **Star Wars Screen Entertainment: other builds.** One manifest covers
  this CD, and with it the Redump BIN of the same pressing and the flat ZIP
  of its files. At least three US English Windows builds exist (A,
  archived 1994-08-19/20; B, 1994-10-04; C, this CD's, 1994-10-13), plus a
  German (Softgold, December 1994) and a Japanese edition. They share the
  installer's short name, so they are identified as `swse`; a complete set
  of another build fails verification (3: the online five-floppy set of
  build B differs in 7 of the 14 modules), and `--no-verify` imports it.
  They are documented, not supported: supporting them would take one
  manifest per build (a `Package::builds` list) and, for the other
  languages, names that are not keyed by path. The Japanese edition was not
  examined.
* **Intermission itself.** Delrina's Intermission 4.0 (on the Internet
  Archive, not surveyed), other Intermission module packs, and the eight
  readers the disc ships besides the IMX one (`IMAD_PLY.IMQ`, which ran After
  Dark modules under Intermission; FLC/FLI, SCR, SAP, SPX, NSS, ASA) are
  out of scope: the importer never installs those readers, and each such
  release would need its own survey, as the future After Dark packages
  above do.
* **WinG, natively.** Star Wars Screen Entertainment runs with the GDI
  technology its own setup program offered (ABI.md §3.8.6). A native WinG
  (WinG was the default on a 1994 PC that had it) would reuse the DIB
  driver's "DC over a guest DIB" machinery; it is deferred. By the survey's
  reading of the code, the 8-bit image is the same either way.
* **Covers from installed files.** Every picture of that disc is inside
  its ARJ archives, where a disc cover source cannot reach, so an offline
  import shows the generated cover until `--refresh-covers` fetches one
  (COVERS.md §2.3). Star Trek: The Screen Saver's disks likewise hold no
  picture a disc source can reach: every one is KWAJ-compressed.
* **Star Trek: The Screen Saver, other forms and builds.** A folder that
  keeps the two disks apart, in `DISK1\` and `DISK2\` (the INF's own
  `..\disk1` and `..\disk2`), is not a source, only a flat copy of both
  disks' files: when a root matches no release, identification could be
  retried on the union of its `DISK<n>` folders. An installed `C:\AFTERDRK`
  is no source (§3). Other builds were not surveyed: After Dark 4.0
  Deluxe's `PREVIOUS.INF` shows one that also shipped After Dark 2.0's
  generic Messages module (`AD_MESG.AD`, whose data file, `AD_MESG.ADS`,
  these disks carry unused).
* **After Dark 2.0's host, further.** `AD.EXE`'s built-in modules (Starry
  Night 2.0, the Randomizer, MultiModule) are not catalogued: Starry Night
  is NE application code, which the Win16 runtime does not run, as AD 3.2's
  built-in Starry Night inside `ADW30.EXE` (above). The bridge writes 300 at
  `AD_SYSTEM+0x14` for After Dark 2.0 modules too, where `AD.EXE` wrote 201
  (ABI.md §3.9): the one module that checks it, Sounder, wants at least 200.
  `AD.EXE`'s other results (1, 4, 6–9, 15: a message) still end the run as
  the module's error; none occurs in the release. After Dark 2.0 did not
  wake on the arrow keys, and the saver does: no Star Trek module uses them
  outside Final Exam's exam, which takes every key (INTERACTION.md §1.7).
* **Num Lock outside Win16.** The pe32 lane ignores `NUMLOCK` and
  `ADNUMLOCK`; its `GetKeyState(VK_NUMLOCK)` still reports no toggle, as
  before (Final Exam, the one module known to read it, is 16-bit).
* **Deduplication in the front-end** (`sameAs`) and grouping by package:
  decided by the settings dialog (COVERS.md §1.7, §1.8): grouped by
  release, byte-identical copies played once per Random pass.
