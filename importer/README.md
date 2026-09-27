# adw_import / adimport.exe

Puts the original Windows After Dark files where the hosts read them
(DESIGN.md §6, §7; the full specification is `docs/PACKAGES.md`), and keeps
each release's box cover (DESIGN.md §9, `docs/COVERS.md` §2). It is Long
After Dark's importer, and it knows five After Dark releases, the *packages*
of its built-in registry (`packages.h`):

| id | Release | Medium | Recipe | Installs to (`<win>` = `<assets root>\win`) | Modules | Internet Archive copy |
|---|---|---|---|---|---|---|
| `deluxe` | After Dark 4.0 Deluxe | hybrid CD, plain files | `tree` | `FILES\{AD40,CLASSIC,ENGINE,AFI}` | 84 | the CD image, 381.7 MB |
| `ad10` | After Dark 10th Anniversary | hybrid CD, ISO-9660 + Joliet, plain files | `tree` | `packages\ad10\{AD10TH,ENGINE,AFI}` | 46 | the CD image, 143.3 MB |
| `ad32` | After Dark 3.2 | hybrid CD, InstallShield 3 + encrypted PKZIP | `ad3zip` | `packages\ad32\{AD32,ENGINE}` | 44 | the CD image, 58.8 MB (3 copies) |
| `tt` | Totally Twisted After Dark | hybrid CD, InstallShield 3 + encrypted PKZIP | `ad3zip` | `packages\tt\{TWISTED,ENGINE}` | 13 | the CD image, 37.9 MB |
| `simpsons` | The Simpsons Screen Saver | two floppies (FAT12), InstallShield 2 + encrypted PKZIP | `ad3zip` | `packages\simpsons\{SIMPSONS,ENGINE}` | 15 | a ZIP of the install files, 2.6 MB (2 copies) |

```
adimport --image <image> [--image <image2> …] | --iso <image> | --from <drive or folder>
         | --download [<id> | all]
         [--package <id>] [--dest <assets root>] [--gui] [--no-verify] [--quiet]
         [--download-dir <dir>] [--url <url> [--md5 <hex>]] [--no-cover-download]
adimport --catalog-only [--dest <assets root>] [--quiet]
adimport --list-packages [--dest <assets root>]
adimport --remove <id> [--dest <assets root>] [--quiet]
adimport --set-cover <id> <picture> [--dest <assets root>] [--quiet]
adimport --clear-cover <id> [--dest <assets root>] [--quiet]
adimport --refresh-covers [<id> | all] [--force] [--dest <assets root>] [--download-dir <dir>] [--quiet]
adimport --gui --change-cover <id> [--dest <assets root>] [--download-dir <dir>] [--no-cover-download]
adimport --gui --refresh-covers [<id> | all] [--force] [--dest <assets root>] [--download-dir <dir>]
```

Every import ends by rewriting `catalog-win.json` over every installed
package (see **Catalog**); `--catalog-only` imports nothing and rewrites it
from the files already there. `--list-packages` prints each registry
package, whether (and how verified) it is installed, and the size of its
download, and its cover. `--remove <id>`
deletes one package (Deluxe: `FILES` and its `import.json`) and rewrites the
catalog. The cover commands are described under **Covers**.

## Sources

Every source is read through one view, `SourceFs` (`source.h`):

| Source | What is read |
|---|---|
| `--image` (= `--iso`) | The type is sniffed from the content, never the extension. **ISO-9660** (`iso9660.h`): level 1/2, Joliet SVD preferred when present (each Joliet entry is paired with its 8.3 twin), multi-extent files, one-sided both-endian fields, cooked 2048 or raw 2352-byte sectors; the Apple partition map and HFS half of a hybrid disc are ignored. Otherwise, a local file header at byte 0: a **ZIP of install files** (`zip.h`, held in memory, at most 256 MB), whose members are the files at the source's root — bare names only (a ZIP that nests them in a folder is refused: "a ZIP source must hold the install files at its root"), none password-protected (the installer's own encrypted archives are members like any file), each inflated and its size and CRC-32 checked as it is read. Otherwise **FAT12/16** (`fat.h`): BPB-driven, strict (see below). The image md5 is always computed. Several `--image`s (split floppies) are read as one tree: directories merge; a file present in more than one image must have the same size (checked when listed) and bytes (checked when read), else the source is invalid ("… they are not the disks of one release"). An image given twice, or a byte-identical copy of one (same md5 and size), is read once and logged as ignored, so the import still counts as one known image. Known images of two different releases are refused before anything is read: "these images are two different releases (…); import each image on its own". |
| `--from` | A folder or drive root. The root of a **CD drive** (a disc, or an image Windows mounted) is read as the disc itself, through the ISO reader on the raw volume (`\\.\E:`, sector-aligned reads): Windows lists a Joliet disc such as the 10th Anniversary by its long names ("Toaster 2k.ad"), and only the disc's own pairing gives the 8.3 names the release, its manifest and the catalog ids use (`TOASTER2.AD`). When the volume cannot be opened it falls back to the listing (logged). Anything else: names are taken as listed, upper-cased (never the volume's generated `~1` alias, which depends on the drive's 8dot3name setting). |
| `--download` | The package's Internet Archive copy (see **Downloads**; Deluxe when neither `--download <id>` nor `--package` names one) via WinHTTP into `<data folder>\downloads` (see **Destination and atomicity**; or `--download-dir`): redirects followed by hand (never from https to http), resume from `<file>.part` with `Range`, restart when a server ignores or botches it, the published size checked before a byte is written and while it streams (a body with no `Content-Length` included; without a published size, 2 GB at most), and the md5 (CNG) before the rename. An already-downloaded file that matches is reused. Retries: 5 failed connections in a row, reset whenever a connection gets the file further (cap 50). One download per destination at a time (`<file>.lock`, delete-on-close). The downloaded file is then read as an `--image` would be. `--url` fetches another URL instead (saved under its own decoded file name — `download.iso` when that is a DOS device such as `NUL.iso` — checked against `--md5` when given); `--md5` must be 32 hex digits. Without `--md5`, the URL is recorded in `<file>.source` (hidden) and the file, or its `.part`, is reused only for that same URL. `--download all`: every package in turn. A cancel (the window's Cancel, or Ctrl+C at the console) stops a download at once, even while it waits on the network. |

## Downloads

Each registry package lists its Internet Archive copies (`packages.h`
`Download`: URL, file name, size, md5, kind), from the archive.org search
verified on 2026-09-26 (`research/win/pkg/sources/sources.json`: every URL
answered 302 → 200 with the stated size and `Accept-Ranges`, and each file's
md5 was compared with the user's own copy):

| id | Copies, in the order tried | Saved as | Size | md5 | Kind |
|---|---|---|---|---|---|
| `deluxe` | `after-dark-4-deluxe` | `After Dark 4.0 Deluxe (1996)(Berkeley Systems)[Mac-PC].iso` | 400234496 | `d875a603…` | image |
| `ad10` | `ad10th` (the only exact copy; darkened once, restored 2024-07-19) | `ad10th.iso` | 150228992 | `a8d08841…` | image |
| `ad32` | `after-dark-v3_2`, `berkeley-systems-after-dark-for-windows`, `3x-…-after-dark-3.2-nirvana` (uncurated, last) | `After Dark 3.2 (1995)(Berkeley Systems)[Mac-PC].iso` | 61693952 | `8b8be697…` | image |
| `tt` | `TTW320CD` | `TTW320CD.ISO` | 39784448 | `541b9cfd…` | image |
| `simpsons` | `SIMPSONS_WIN/SIMPSONS.zip`, then `AfterDarkSimpsons/After Dark - The Simpsons.zip` | the same names | 2752575 / 2752010 | `90a85bf6…` / `1d608334…` | zip |

* **Disc images** (`kind` image): the md5 is the package's known image, so
  the import is exactly an `--image` import of that file: `verified: image`.
* **The Simpsons** has no image of its floppies online (the known image is
  the owner's own merge of both disks, with their notes, so nothing can
  match its md5). Its copies are flat ZIPs of the 28 install files, every
  one md5-identical to the floppies' (the readme `CHANGES.TXT`, which the
  recipe never reads, is missing). The ZIP is checked against its published
  md5, read as the install folder (see `--image` above) and every installed
  file is verified against the manifest: `verified: files`.
* **Copies are tried in order.** One whose file is already complete in the
  downloads folder goes first, so nothing is fetched for a package
  downloaded before, from whichever copy. When a copy cannot be fetched
  (network: 404, DNS, …) or is not the published file (a server announcing
  another size is refused before a byte is written; a wrong md5 deletes the
  file), the next copy is tried; cancel, local I/O and the download lock end
  it at once. Copies with the same bytes share a file name, so a transfer
  interrupted on one resumes from the next. When every copy fails the
  import fails with 3 when one delivered a wrong file, else 4, and the
  message says to import from the disc with `--image` or `--from` (the
  Internet Archive does withdraw items). A different file is never accepted.
* `--download all` imports every package in registry order, each as its own
  `--download <id>` (going on after a failure, stopping at a cancel); the
  exit code is the first failure's, 0 when all were imported.

Names that could escape the staging directory or are DOS devices (`CON`,
`NUL.AD`, `COM1`…) reject the source, from every reader (`names.h`).

**The staging budget.** A folder that the source lists under two names (an
ISO directory record, a FAT subdirectory or, in a folder, a junction that
reaches a directory already walked: `SourceFs::dir_key`) rejects the source,
as does nesting deeper than 16. An import plans at most 20,000 files and
2 GB (`ImportOptions::max_files`, `max_bytes`; the largest release is 175
files, 45 MB), and a planned file whose size differs from the manifest's
fails verification before anything is written (`--no-verify` imports it), so
a damaged or crafted source cannot fill the disk with copies before it is
checked.

**FAT12/16.** Bytes/sector 512–4096, sectors/cluster a power of two,
reserved sectors ≥ 1, one or two FATs, a root directory, a sector count and
a FAT size; the `55 AA` signature; the image at least as long as its
sectors; FAT12 below 4085 clusters, FAT16 below 65525, FAT32 refused. A
cluster chain that loops, meets a free, bad or out-of-range cluster, or ends
before the file does rejects the source. Deleted, long-name and volume-label
entries and `.`/`..` are skipped; a leading `0x05` is `0xE5`; names are
decoded from code page 437. Directory data is read when a directory is
listed, file data only when a file is read.

## Identification

1. **Image md5.** A match with a registry package's known image names the
   package; its fingerprint must then match too.
2. **Fingerprints** (folders, and images with an unknown md5), every package
   in registry order; exactly one must match:
   * `tree` packages: a FILES dir (`ADE\FILES`, `FILES` or the root) holding
     the first module dir and `ENGINE`, plus the package's marker
     (`ad10`: `AD10TH\ADXPL40.DLL`) and none of its absent dirs (`ad10`: no
     `AD40`). Deluxe: `AD40` + `ENGINE`, as always.
   * `ad3zip` packages: an install dir (`INSTALL`, or the root of a floppy)
     holding `INSTALL.INS`, `SETUP.PKG`, `ENGINE.ZIP` and `MODMISC.ZIP`; the
     package is the one whose engine DLL is a member of `MODMISC.ZIP`
     (`ADXPL300.DLL` / `ADXPL40.DLL` / `ADXPL310.DLL`). Central-directory
     names are not encrypted, so no password is needed to identify.
3. `--package <id>` restricts step 2 to that package and refuses a source
   that is something else.

No match, several matches, or an md5 that names another package: exit 2,
with a message naming the known releases.

## Recipes

**`tree`** copies the package's copy dirs from the FILES dir byte for byte
(`deluxe`: `AD40`, `CLASSIC`, `ENGINE`, `AFI`; `ad10`: `AD10TH`, `ENGINE`,
`AFI` — `GAMES`, `WALLPAPR` and the non-After Dark root folders are left),
then the package's fix-ups: copies under the names the modules open, made
only from a source file that matched the manifest (or came from the known
image). `ad10` has four: `AD10TH\TT_SND.DLL` (from `MUSIC\`), and
`MUSIC\Toasters2k.mid`, `Flying Toasters.mid`, `Baby Toasters.mid`. Their
`import.json` `from` is `alias:<source path in the package>`.

**`ad3zip`** reproduces what the InstallShield scripts did, flattened into
one module dir `M` (the lanes mount it as `C:\AFTERDRK`) and `E` =
`ENGINE`. Every archive's central directory is read; members are
decrypted (traditional PKWARE "ZipCrypto"), raw-inflated through zlib and
checked against their size and CRC-32 (`zip.h`; a mismatch is a corrupt
source, 2, never a verify failure):

| Archive | Goes to |
|---|---|
| any ZIP with an `*.AD` member | every member → `M\` |
| `MODMISC.ZIP` | every member but `EDITFILE.TXT` → `M\` |
| `WIN.ZIP` | `AD_RSRC.DLL` → `M\` |
| `BITMAPS.ZIP`, `TRACES.ZIP`, `SOUNDS.ZIP` | → `M\BITMAPS\`, `M\TRACES\`, `M\SOUNDS\` |
| `MUSICG.ZIP`, else `MUSIC.ZIP` | `*.MID` → `M\MUSIC\`; `*.DLL` (`TT_SND`, `SIMP_SND`) → `M\` |
| `AFI.ZIP` | the package's folder AFI → `M\FOLDER.AFI` |
| `ENGINE.ZIP` | `AD_SND.DLL`, `ADTASK.DLL`, `ADW30.EXE`, `ADW30.INI`, `ECOLOGIC.DLL` → `E\` |
| `HELP`, `MULTIS`, `WINSYS`, `WAVEMIX`, the unused `MUSIC` | skipped |
| anything else | skipped and logged |

Only the archives and `INSTALL.INS` are read: the Simpsons floppy's
`CEREAL.TXT` and `SERIAL.TXT` (the original owner's notes) are never opened,
copied, hashed or listed. `simpsons` also requires all 15 module archives,
so a split-floppy source must include both disks.

**The archive password** is never stored: it is derived from `INSTALL.INS`
at import time (§8.4). Candidates are the script's strings of 4–32 printable
bytes — its own length-prefixed strings (a 16-bit length, then the bytes:
the real scripts follow the password with an opcode byte that is printable,
so a bare printable run would be one byte too long) and plain printable
runs — the first one after "Cannot initialize for unzip!" first, then the
rest in file order. The first candidate that opens the smallest encrypted
member (check byte, then full decrypt, inflate and CRC-32) and the smallest
encrypted member of another archive is the password. It is kept in memory
only: never logged, never in `import.json`.

**Required files and invariants.** A package's `required` files (§2) must be
in the plan (2 otherwise). After staging, every package but Deluxe must
satisfy PACKAGES.md §4.2 (2 otherwise): I1 no `AD_SND`/`OLDMOD16`/`OLDMOD32`/
`ADTASK`/`ADW30.EXE` beside the modules; I2 every non-system DLL a module
imports (but `AD_SND`) beside it; I3 `ENGINE\AD_SND.DLL` plus either
`ENGINE\OLDMOD16.DLL` + `AFTERDAR.SCR` or `ENGINE\ADTASK.DLL`; I4 every
`*_SND.DLL` sound database in a module folder, and (`ad3zip`) every MIDI in
`M\MUSIC\`.

## Destination and atomicity

`<root>` is `--dest`, else `%AD_ASSETS_DIR%`, else `<data folder>\assets`.
Everything goes to `win_assets_dir(<root>)`: `<root>\win` when it holds
`FILES`, `packages` or `catalog-win.json`; else `<root>` itself when that
holds one of them; else `<root>\win` — the rule `adhostwin` applies to
`AD_ASSETS_DIR`.

**The data folder** is Long After Dark's per-user folder,
`%LOCALAPPDATA%\LongAfterDark`, which the host and `LongAfterDark.scr` use
too; the default assets root and the default downloads folder
(`<data folder>\downloads`, with cover pictures in its `covers\`) are in it.
`importer.h` `data_folder()` is `<base>\LongAfterDark`, on the same base as
the host and the saver (the shared helper
`host/core/include/adw/core/data_root.h`, whose include directory
`adw_import` adds privately): `AD_LOCALAPPDATA` when set and not blank, else
`LOCALAPPDATA`, else `SHGetKnownFolderPath(FOLDERID_LocalAppData)`. Working
it out reads the environment only, never the disk, and creates nothing;
nothing is created under it until an import or a download writes there. A
run with explicit locations never uses it: `--dest` (or `AD_ASSETS_DIR`),
and `--download-dir` for a download. Cover commands and imports take the
downloads folder only when a cover download actually starts. `--help` names
the folder.

**One package, one directory.** One operation per win dir at a time
(`import.lock`, delete-on-close); holding it makes the recovery below safe.
*Deluxe* is staged in `FILES.importing-<pid>`, re-read and re-hashed, and
swapped in with two renames, `import.json` and the catalog after it — the
flow it always had. *Every other package* is staged in
`packages\<id>.importing-<pid>` (files re-read and re-hashed, the manifest,
`required` and the invariants checked, its `import.json` written into the
stage), the merged catalog is rendered over the stage plus every other
installed package, then `packages\<id>` → `packages\<id>.old-<pid>`, stage →
`packages\<id>`, catalog tmp → `catalog-win.json`, old tree deleted. An
import of one package writes only its own directory, `catalog-win.json*`
and `import.lock`; it never modifies `FILES`, `import.json` or another
package. Cancel is honoured up to the first rename and ignored after it (a
finished import never reports 5).

**Recovery**, at the start of every operation under the lock: Deluxe's
(unchanged: no `FILES` but a `FILES.old-<pid>` is put back; a finished swap
whose `import.json.tmp`/catalog tmp renames did not happen is completed);
then every `packages\<id>.old-<pid>` is put back when `packages\<id>` is
missing, else deleted; every `*.importing-*` and `*.removing-*` is deleted;
and the catalog is rewritten when anything was recovered or a package
operation's catalog tmp was left behind. Folders in `packages\` that are not
registry ids are ignored (and logged).

*Installed* means: Deluxe when `<win>\FILES` is a directory; another
package when `<win>\packages\<id>\import.json` exists. `--catalog-only`
needs one installed package, not `FILES`.

**Exit codes** (`adw::import::Status`): 0 ok · 1 error (usage, local I/O,
another operation running, `--remove` of something not installed) · 2
source invalid · 3 verify failed · 4 network · 5 cancelled.

## Verification

Each package has a manifest (path, size, md5 of every installed file,
fix-ups included — never After Dark bytes): `known_files.inc` (Deluxe, 175
files) and `known_files_<id>.inc` (`ad10` 147, `ad32` 89, `tt` 26,
`simpsons` 30). `"verified"` is `image` (the image md5 is the package's known
image), `files` (every file of the manifest is there and matched it, and
nothing else was installed), `partial` (some installed files are not in the
manifest, or some of the manifest's are missing: `missingKnown` lists them),
or `none` (`--no-verify`, or no manifest). A file that differs
from the manifest fails the import with 3 unless `--no-verify`. Regenerate a
manifest with `gen_known_files.py <import.json of a verified image import>`:
it accepts Deluxe's version-1 record and every other package's version-2
record, only when the image md5 is that package's known one.

## import.json

Deluxe keeps `<win>\import.json`, version 1, unchanged:

```json
{ "version": 1, "tool": "adimport 1.0", "importedUtc": "2026-09-25T23:10:00Z",
  "source": { "kind": "download", "path": "C:\\…\\downloads\\After Dark 4.0 Deluxe….iso",
              "url": "https://archive.org/download/…", "finalUrl": "https://…archive.org/…",
              "isoSize": 400234496, "isoMd5": "d875a60338b73f44b7befa06bdd33aeb",
              "isoMd5Known": true, "joliet": false, "volumeId": "AD_DELUXE" },
  "verified": "image", "fileCount": 175, "totalBytes": 34351595, "missingKnown": [],
  "files": [ { "path": "FILES/AD40/3DMINOR.MID", "size": 17373, "md5": "…", "known": "match" } ] }
```

Every other package writes version 2 inside its root (PACKAGES.md §5.3):

```json
{ "version": 2, "tool": "adimport 1.2", "importedUtc": "…",
  "package": {"id": "ad32", "title": "After Dark 3.2", "recipe": "ad3zip", "root": "packages/ad32"},
  "source": { "kind": "iso", "format": "iso9660", "path": "D:\\…\\afterdark3.2.ISO",
              "imageSize": 61693952, "imageMd5": "8b8be6977375fbf4d54146b9d505aa1c", "imageMd5Known": true,
              "volumeId": "ADW320_C", "parts": [] },
  "verified": "image", "fileCount": 89, "totalBytes": 6037078, "missingKnown": [],
  "files": [ {"path": "packages/ad32/AD32/GUTS.AD", "size": 16688, "md5": "…", "known": "match",
              "from": "INSTALL/GUTS.ZIP!GUTS.AD"} ] }
```

`kind` is `iso`, `floppy`, `zip`, `folder` or `download`; `format` is
`iso9660`, `iso9660+joliet`, `fat12`, `fat16`, `zip` or `folder`; the image
fields appear for image sources, downloads included (`imageSize`/`imageMd5`
for a single image: for the Simpsons download, the ZIP's); `parts` lists
every image of a multi-image source. A download adds `url` (the copy
fetched), `finalUrl` (where its redirects led; absent when an earlier
download was reused) and `md5Checked` (the file matched its published md5,
or `--md5`). Deluxe's version-1 record keeps its `url`/`finalUrl`. `from` is the source path, `zip!member`
for an archive member, `alias:<path>` for a fix-up. `files` is sorted by
path; `known` is `match`, `mismatch` or `unknown`.

## Catalog

`<win>\catalog-win.json` is what the front-ends read (DESIGN.md §6a,
PACKAGES.md §6). It is generated from the module binaries without executing
anything (`catalog.h`, a C++ port of the prototype
`research/win/make_catalog.py`, using the `adw::loader` PE/NE readers),
following ABI.md §2.10, over every installed package in registry order:

| | AD4 lane (`pe32`) | Classic lane (`ne16`) |
|---|---|---|
| Lane | the file header says PE32 | the file header says NE |
| Files, in order | Deluxe: `AD40\*.AD` sorted, `ENGINE\STARRYNI.AD`, `CLASSIC\*.AD` sorted; every other package: its module dirs in registry order, then `ENGINE\*.AD`, each sorted | same |
| `id` | Deluxe `ad40.<base>`; else `<package>.<base>` | Deluxe `classic.<base>`; else `<package>.<base>` |
| name | `VERSIONINFO` `FileDescription` | resource `2000/20` |
| `about` | `2000/40`, RTF reduced to plain text | `2000/30`; `credits` from `2000/10` |
| `controls` | `1000/1..4` (slot = name − 1) | same |
| `entry` | `_Module@4` when exported (STARRYNI), else `Module` | `MODULE` |

A missing name falls back to `STRINGLIST 128[0]`, then the file name; it is
trimmed at both ends, then the package's name overrides apply (`ad10`
`TOAST2K.AD` → "Toasters 2k (early build)"): that is `moduleName`.
`displayName` is unique within a lane, case-insensitively: the first module
with a name keeps it, a later one becomes `name (<short title>)`, and if
that is taken too `name (<short title>, <FILE>)` — so today's front-end,
which lists by lane, needs no change. PE resources in several languages
(STARRYNI has five) are read as an English Windows loads them: 0x409, then
neutral, then the lowest id. Text is Windows-1252, written as UTF-8. Each
control carries `index`, `name`, `kind`
(`stringslider`/`numslider`/`popup`/`checkbox`/`button`) and `type`, plus per
kind: string sliders `items` + `values` (the value sent for each stop; the
host's prepended 0 stop and repeated, bold last label included), `default`
(a value) and `defaultStop` (+ `boldStop`); numeric sliders `min`/`max`,
`default` clamped into them and `rawDefault` as stored, `unit` + `unitPos`
when the record names a unit; popups `items` + clamped `default`; checkboxes
`default` 0/1. Modules also list `needs` (non-system DLLs they import) and
`system`, then `package`, `packageTitle`, `moduleName`, `md5` (of the file)
and, when an earlier entry has the same bytes, `sameAs` (its id). The
top-level `packages` list (between `generator` and `modules`) gives each
installed package's `id`, `title`, `shortTitle`, `root`, `verified`,
`importedUtc`, module count and `cover` (COVERS.md §2.7; paths relative to
`<win>`):

```json
"cover": { "origin": "download", "tile": "covers/simpsons/tile.png", "tileMd5": "…",
           "image": "covers/simpsons/original.png", "width": 600, "height": 776, "art": "box",
           "label": "Box front", "credit": "Wikisimpsons", "original": "download" }
```

`origin` is `user`, `download`, `disc` or `generated` (then the object holds
nothing else); `original` is the origin of the original, under a user picture
too. Every catalog write checks the covers: a missing, damaged or stale tile
whose picture is sound is rendered again, and a damaged `cover.json` lists as
generated (logged) until `--refresh-covers` repairs it. The generator is
`adimport 1.2` (the catalog `version` stays 1); the layout matches Python's
`json.dump(indent=1, ensure_ascii=False)`, so it diffs cleanly against the
prototype's.

A module that cannot be read is left out and logged, never a reason to
refuse the import; two files with one id keep the first. The catalog is
rendered from the stage and swapped in with the files (a failed import
leaves the old one); `--catalog-only` holds `import.lock` while it scans and
replaces the file atomically.

Over the real corpus: Deluxe 84 (23 `pe32` + 61 `ne16`; semantically
identical to the prototype's apart from the new fields and one correction —
the prototype never parses exports, so it lists STARRYNI's entry as
`Module`; the module exports `_Module@4`), `ad10` 46 (17 + 29), `ad32` 44,
`tt` 13, `simpsons` 15 (all `ne16`): 202 with all five installed, 73 of them
`sameAs` an earlier entry.

## Covers

Each imported release has a box cover (`docs/COVERS.md` §2; the settings
dialog's release strip shows them). The tile shows the first of:

1. **your own picture**, set with `--set-cover` (or "Change cover…" in the
   GUI);
2. **the original**: the best of the release's *cover sources* captured so
   far (`packages.h` `CoverSource`, tried in the release's own order);
3. **a generated cover**, which the front-ends draw themselves.

| id | Sources, in the order tried | original.png |
|---|---|---|
| `deluxe` | the box front from Berkeley Systems' 1997 product page, through the Wayback Machine (`box.deluxe.gif`), then the setup wizard's art on the disc (`ADE\PAGE1.BMP`), then the disc label (`archive.org/download/after-dark-4-deluxe/disc.jpg`) | 162×195 / 118×226 / 1488×1452 |
| `ad10` | the disc label (`ad10th/01_ad10_cd.jpg`), then `ADE\PAGE1.BMP` | 800×794 / 118×226 |
| `ad32` | the installer splash on the disc (`INSTALL\SETUP.BMP`, cropped to 387×183 above its warning), then two scans of the disc label | 387×183 |
| `tt` | the box front from Berkeley Systems' 1997 product page (`box.twistedL.jpg`), then the box front of Sierra's later edition from The Sierra Chest (`01_front.JPG`), both through the Wayback Machine, then the installer splash (`INSTALL\SETUP.BMP`, cropped to 387×204), then the `TTW320CD` item's TIFF scan of the disc | 127×162 / 498×599 / 387×204 / 2014×2048 |
| `simpsons` | the box front from Wikisimpsons, then the Wayback Machine's copy of the full-box scan it was cut from (cropped to 600×776), then `SETUP.EXE`'s bitmap 7500 (cropped to 387×172) | 600×776 / 387×172 |

The box fronts are small (a tile is never shown larger than 160×200 px),
but they are the retail boxes; a disc label or an installer splash is what
stands in when they can't be fetched. Wayback Machine URLs are the `id_`
form, which serves the archived file's own bytes (the md5 is of those).

The registry holds only URLs, md5s, sizes, paths and crops: no picture is
shipped. Downloads are HTTPS, checked against their published md5 and size
before use (a wrong file is deleted and the next source tried), and kept in
`<download dir>\covers\` (`--download-dir`; by default
`<data folder>\downloads\covers`, taken only once a download starts), where a later import or
refresh reuses them without a request. A disc source is read from the source
being imported (only the named file; with its md5 checked, so another pressing
is skipped); a bitmap resource of an NE or PE file gets a `BITMAPFILEHEADER`
before it is decoded. The Simpsons art belongs to Fox: it is fetched onto the
user's machine at import time and never bundled.

**During an import** a `cover` phase comes between `verify` and `finalize`
(`Progress::Phase::cover`, "Getting the cover art"). It tries only the sources
better than the stored original (a re-import fetches nothing it already has,
and a better source replaces a fallback). Each download may take two attempts
with a 15-second timeout; after a network-class failure (DNS, connect, TLS, a
timeout) the remaining downloads are skipped and the disc art is used.
`--no-cover-download` skips the downloads altogether. The files are staged in
`covers\<id>.importing-<pid>` and moved in with the package swap (Deluxe
included). **A cover never fails an import**: when every source fails, the
cover stays as it was (or generated), a log line says so, and `cover.json`
records what was tried. `user.png` is never touched by an import, and
`--remove` keeps `covers\<id>` for a later import.

**On disk**, under `<win>\covers\<id>\`: `original.png` (decoded with WIC, EXIF
orientation applied, cropped, long side capped at 2048 px, RGBA), `user.png`
(your picture, normalized the same way), `tile.png` (640×800, opaque) and
`cover.json` (version 1: where the original came from with its md5s, your
picture's file name, the tile's renderer version, the last attempts). Every
file is written as `<name>.tmp-<pid>` and renamed; a stored file counts only
while its md5 matches `cover.json`. The recovery sweep deletes
`covers\*.importing-*` and `covers\<id>\*.tmp-*`.

**The tile** (`cover_image.h`; pure functions, so the tests pin them): a disc
label becomes a circle 552 px across, centred at (320, 368), with an
anti-aliased rim, on the night gradient `#262B4F` → `#12152A`; any other
picture within ±15% of 4:5 fills the tile, and anything else is contained on
bands of the mean colour of its two outermost rows (or columns), with
transparency flattened onto them. A picture of 256 colours or fewer scaled by
2 or more is scaled up by a whole factor with nearest-neighbour first, then
with the cubic resampler. Raising `kRendererVersion` makes the next catalog
write render every stored tile again.

```
adimport --set-cover <id> <picture> [--dest <root>] [--quiet]
adimport --clear-cover <id> [--dest <root>] [--quiet]
adimport --refresh-covers [<id> | all] [--force] [--dest <root>] [--download-dir <dir>] [--quiet]
adimport --gui --change-cover <id> [--dest <root>] [--download-dir <dir>] [--no-cover-download]
adimport --gui --refresh-covers [<id> | all] [--force] [--dest <root>] [--download-dir <dir>]
```

* `--set-cover` takes any picture Windows can read (PNG, JPEG, GIF, BMP, TIFF,
  ICO, JPEG XR; WebP, HEIF or AVIF when their codecs are installed; 32 to
  16384 px a side, at most 64 MB): exit 0, 1 (usage, not imported, the lock
  held, local I/O) or 2 (the picture can't be read). It stays on this
  computer; only its file name is recorded.
* `--clear-cover` goes back to the original: exit 0 (also when there is
  nothing to clear, which writes nothing) or 1.
* `--refresh-covers` (every installed release when no id is given) tries the
  downloads better than the current original, and repairs missing or damaged
  files: exit 0 when every release has the best cover this run could reach, 4
  when a download failed (the previous cover is kept; the output says which),
  1 on an error. `--force` tries every download, and keeps the stored
  original when none works. Nothing fetches covers in the background.
* `--change-cover` opens only the GUI's cover window (`gui/README.md`).
* `--gui --refresh-covers` is `--refresh-covers` in a progress window, then
  a page saying what it got: exit 0 when a cover changed, else the first
  failure (4 when a download failed), else 5. It is what the settings
  dialog's **Get the covers** link runs, and the importer's Sources page has
  the same action for the releases still showing a generated cover.
* `--list-packages` adds `; cover: <origin> (<label>)` to each installed line.
* The cover commands are modes, like `--catalog-only`: one at a time, each
  with only its own options; `--no-cover-download` belongs to imports.
* The library calls are `covers.h`: `cover_info`, `set_cover`, `clear_cover`
  and `refresh_covers` (each takes `import.lock`, writes atomically and
  rewrites the catalog when anything changed).
* `AD_COVER_DOWNLOAD=0` in the environment turns every cover download off, as
  `--no-cover-download` does (tests whose command lines are fixed use it).

**Installs made before covers** have no `covers\` and show generated covers
until `adimport --refresh-covers` (downloads) or a re-import (disc art too).
The settings dialog offers the refresh itself: while any release shows a
generated cover, **Get the covers** appears under the strip's status line
(and the importer's Sources page says how many releases have no cover yet,
with the same button). Only the 3.2 installer splash needs a re-import from
the disc; a refresh gets that release's disc-label scan.
To use your own pictures instead, for example the box photos kept with the
research notes:

```
adimport --set-cover deluxe "<repo>\research\win\pkg\covers\deluxe\cover_supplied.png"
adimport --set-cover tt     "<repo>\research\win\pkg\covers\tt\cover_supplied.png"
adimport --set-cover ad10   "<repo>\research\win\pkg\covers\ad10\cover_supplied.png"
```

## GUI

`adimport --gui` (and `adimport` started from Explorer with no arguments)
shows the themed windows in `gui/` (`adw_import_gui`; COVERS.md §4). They
are documented in [`gui/README.md`](gui/README.md): the pages (Sources,
Downloads, Progress, Result, Cover), their command ids, the exit codes and
the test hooks (`AD_IMPORT_TEST_SCREENSHOT` / `_STATE`, `AD_IMPORT_TEST_PICK`).
`adimport.cc` hands them a `gui::Request` (`gui/gui.h`) and returns
`gui::run`'s result as the exit code: 0 when anything was imported or a
cover changed, else the first failure, else 5 (cancelled, nothing
changed). `--change-cover <id>` opens only the cover window, and
`--refresh-covers` only a progress window over the cover downloads.
`--download-dir` applies to the windows as well: where the Downloads page
looks for "already downloaded", where downloads and cover pictures go.

Started from Explorer with no console, it behaves as `--gui`. The
manifest's `consoleAllocationPolicy=detached` (Windows 11 24H2+) keeps a
GUI parent from giving it a console window; on earlier Windows the parent
should pass `CREATE_NO_WINDOW`, as LongAfterDark.scr's settings dialog does (it
reads exit 5 as "cancelled, nothing changed"). `AD_GUI_AUTOCLOSE=1` skips
the final result page (tests).

## Tests

`import.md5` (RFC 1321 vectors, streaming, files; its scratch folder is
removed), `import.iso` (synthetic
images from `tests/iso_builder.h` in seven layouts, each also through the
sector-aligned read path a raw CD volume uses, plus Joliet directories
paired with their 8.3 twins by shared file data), `import.zip`
(`tests/zip_builder.h`: round trips, the check byte incl. flag bit 3, wrong
passwords and check-byte false positives, bad CRC, damaged data, size
mismatch, truncation, ZIP64, multi-disk, strong encryption, unsupported
methods, zip-slip and device names, duplicates; the password candidates and
their order, decoys, a check-byte trap, no password, two archives under
different passwords), `import.fat` (`tests/fat_builder.h`: 1.44 MB, 2.88 MB,
FAT16 and 1 KB-sector volumes, a fragmented file, subdirectories, skipped
entries, every refused BPB, loops, free/bad/out-of-range clusters, short
chains; content sniffing; the union of two floppies; directory keys, an
aliased subdirectory, and a union's keys), `import.import`
(Deluxe image/folder imports byte-compared, import.json parsed with phosg,
atomic failure cases, hostile and device names, one folder listed twice
(siblings, a loop and a 3^12 bomb, in both trees; a folder junction), the
staging budget, a size mismatch refused before the copy, lock, late cancel, recovery
from both interrupted-swap states, an unrepresentable timestamp,
`AD_ASSETS_DIR` trimming, manifest shape), `import.packages`
(`tests/pkg_fixture.h`: all five releases as folders, ISOs and FAT images,
split floppies in either order; identification, unknown, ambiguous,
`--package`, image md5s; exact file sets, fix-ups only from matching
sources, each invariant, required files and archives, the password never
written or logged, the owner's notes never read; per-package atomicity,
failed and cancelled re-imports, the lock; recovery from every interrupted
swap and removal; `--catalog-only` without `FILES`, `--remove`,
`--list-packages`; a source missing a known file verified `partial`, a known
file of another size refused before the copy; the merged catalog's ids, order, names, overrides,
`sameAs`, `packages`, and Deluxe's entries unchanged; `adimport.exe`'s
options and exit codes; the registry's cover sources — at least one per
package, HTTPS URLs with 32-hex md5s, sizes and unique file names, disc
paths, crops, and the §2.3 decisions), `import.covers` (offline: synthetic
pictures written through WIC, synthetic registries and the loopback server;
the tile rules — fill or contain on both sides of ±15% of 4:5, band colours,
the disc's circle and anti-aliased rim, nearest-then-cubic, crops inside and
outside, all eight EXIF orientations, the 2048 cap; decoding BMP, PNG, JPEG,
GIF and TIFF, a JPEG with EXIF orientation 6, NE and PE `RT_BITMAP`
resources, 16×16, random bytes and a file over 64 MB refused; downloads
checked by md5 and size through a redirect, 404 and a wrong md5 falling
through to the next source with the bad file deleted, a stalled server timing
out with the other downloads skipped and the disc art used, reuse from
`<download dir>\covers` without a request, `--no-cover-download`, a disc file
of another pressing; during an import `covers\<id>` and `packages[].cover`,
a cancel in the cover phase, every source failing with exit 0 and the same
`import.json`, a re-import fetching nothing and keeping `user.png`, a better
source replacing a fallback, `--remove` keeping the cover, the recovery
sweep, Deluxe's `FILES` byte-identical; `set_cover`, `clear_cover` and
`refresh_covers` — not imported, unknown, an unreadable picture, the lock
held, `changed`, no catalog write for nothing to clear, disc → download once
the server answers, `--force`, a damaged `original.png` fetched again, a
missing tile and a stale renderer rendered again by `--catalog-only`, a
damaged `cover.json` listed as generated until refresh, an unreadable or
damaged one reported by `cover_info` (`CoverInfo::error`), numbers out of
range in it read as absent, bitmap resources with a damaged colour count
refused — and `adimport.exe`'s
exit codes for the three commands), `import.download` (loopback server: redirects, drop
+ resume, 416, Range ignored or botched, md5 mismatch, 404, refused, cancel,
more drops than `max_attempts` with progress, the destination lock; the
redirect policy (no https to http); a body with no length past `max_size` or
the published size; reuse and resume only for the recorded URL; a cancel
token stopping a download blocked on a silent server),
`import.cli` (exit codes, `--md5` validation, `--catalog-only`,
`--download` against the loopback server, a URL named after a device; the cover modes' exclusivity and
options, `--no-cover-download` only with imports, `--change-cover`'s
companions, the `--list-packages` cover column; `AD_GUI_TESTS=1` also drives
the progress window, the source chooser and its Internet Archive list, back
and out), `import.pkg_download` (every package from its registry copies on a
loopback server that redirects like archive.org: the four disc images
verified `image`, the Simpsons as a flat ZIP verified `files`, the ZIP as
`--image`, nested and password-protected ZIPs refused; fallback after a 404,
a wrong size — refused before a byte is written, the `.part` resumed by the
next copy — and a wrong md5; every copy failing (3 or 4, nothing left
behind); reuse of a file from any copy without a request, a stale file of
the wrong size fetched again; no copy known; `--url` with and without a
package; `--md5` over the registry; the download record in both
`import.json` versions; `import_downloads` over all five and a cancel
mid-way; the built-in copies' shape — archive.org URLs, image md5s equal to
the known images, file names per content, the Simpsons ZIPs' md5s; and
`adimport.exe`'s `--download <id>`/`all` parsing and `--list-packages`
sizes), `import.catalog` (the RTF, text,
STRINGLIST and control-record readers against hand-made inputs, whole
synthetic PE32/NE modules from `tests/module_builder.h` with multi-language
resources, a FILES tree with junk and duplicate ids, the JSON layout, and
`--catalog-only` as a library call incl. the lock), `import.catalog_real`
(skipped, exit 77, unless both the imported assets and the prototype's
`research/win/catalog-win.json` are present: the generated catalog, through
the library and through `adimport --catalog-only` on a scratch copy of the
modules, must equal the prototype's field by field, the STARRYNI erratum
and the PACKAGES.md §6 fields aside).

Opt-in: `import.covers_real` (`AD_E2E=1`) fetches every registry cover
download into a scratch folder, checks its md5 and size, decodes, crops and
renders it; with `AD_E2E_PKG=1` it also imports the real images (by md5, from
`AD_SOURCE_ISO_DIR` or `<repo>\source_iso`, the Deluxe ISO also from the
downloads folder, which is only read) with `--no-cover-download`, so every
disc source is extracted, and checks the originals' sizes (387×183, 387×172,
387×204, 118×226). It writes every tile side by side into
`<build dir>\covers-sheet.png` for a person to look at, and deletes its
scratch tree unless `AD_E2E_KEEP=1`. Every other opt-in import runs with
`--no-cover-download`. `import.download_real` (`AD_E2E=1`) runs `adimport --download <id>`
for every package into a scratch downloads folder and a scratch root under
the build tree. A local file with a copy's published size and md5
(`AD_E2E_LOCAL_DIRS`, `;`-separated; default `<repo>\source_iso`,
the installed data folder's `downloads` and its `verify\`) is hard-linked
or copied in first, so what can be verified locally is not fetched again
(`AD_E2E_NO_SEED=1` fetches everything); the importer still checks its md5.
Each import must exit 0 with kind `download`, the expected `verified`,
nothing missing, every file a manifest match and 84/46/44/13/15 modules. The
Simpsons' second copy is fetched with `--url`/`--md5` and imported the same
way. Every registry URL, fallbacks included, must then answer a Range
request with the published size and the same bytes as the verified file
(its last 64 KiB, and an ISO's primary volume descriptor). The scratch tree
is deleted afterwards unless `AD_E2E_KEEP=1`.
`import.e2e` (`AD_E2E=1`) downloads the Deluxe image into
`<scratch>-downloads` (or `AD_E2E_DOWNLOAD_DIR`; the user's own download in
the installed data folder is linked in first when it is there), imports it
through `--download`, `--iso`, `--from` the imported tree and `--from` the
image mounted by Windows, checks every file against the manifest, and
byte-compares the seeded tree at the installed data folder's
`assets\win\FILES` (`AD_E2E_SEED`).
`import.pkg_real` (`AD_E2E_PKG=1`) finds the four package images by size and
md5 in `AD_SOURCE_ISO_DIR` (default `<repo>\source_iso`) and imports each
into a fresh scratch root (exit 0, `verified: image`, nothing missing, the
installed files exactly the manifest, the §4.3 counts, 46/44/13/15 modules
in the right lanes), mounts the 10th Anniversary and Totally Twisted images
with Windows and imports `--from` the drive (the same files as the image;
skipped when mounting is refused), then all four plus Deluxe `--from` the
installed assets (read only) into one root (202 modules, unique names per
lane, consistent `sameAs`, every Deluxe field as the installed catalog has
it), and checks that re-importing each package changes nothing else.

**Before a release** (these stay opt-in: they need the real images, the
network or both, and take minutes), in a Release build directory:

```
AD_E2E_PKG=1 ctest -R "import\.(pkg_real|covers_real)"          # the four images in source_iso
AD_E2E=1 ctest -R "import\.(e2e|download_real|covers_real)"      # the Internet Archive copies (~800 MB)
ctest -L gui                                                     # real windows on the desktop, briefly
```

Every one must pass (or report skipped, 77, for a missing image), and none
leaves anything outside its scratch tree under the build directory.

**No test touches the user's data folder.** Every suite first points
`AD_LOCALAPPDATA` at `<scratch>\localappdata` (`tests/test_util.h`
`sandbox_data_root`), so its own defaults and those of every `adimport` it
starts (some run without `--dest`) resolve there, and nothing is written
to the real one. The suites that read the user's installed data
(`catalog_real`, `e2e`, `pkg_real`, `download_real`, `covers_real`) find it
before that with `installed_data_root()`/`installed_assets_root()`, read
only. `import.import` checks that `AD_ASSETS_DIR` alone is the assets
root, and that the defaults are `<AD_LOCALAPPDATA>\LongAfterDark\…` (trimmed,
with a trailing separator tolerated), else `<LOCALAPPDATA>\LongAfterDark\…`
when `AD_LOCALAPPDATA` is blank, without creating anything; `import.cli`
runs `adimport` over a scratch base: a listing names the default assets
root and creates nothing, explicit locations (`--dest`, `AD_ASSETS_DIR`,
the cover commands, an import without cover downloads) leave the data
folder alone, and a download without `--download-dir` lands in
`<data folder>\downloads`. `import.gui_flow` walks Sources and Downloads
with no `--dest` and checks that nothing is created in the data folder.

The tests delete their scratch trees with `remove_tree` (`winutil.h`), not
`std::filesystem::remove_all`, which this toolchain's libc++ makes about a
thousand times slower on Windows.
