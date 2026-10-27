# The `.napmod` format (version 1)

A `.napmod` is an ordinary zip (deflate or stored). The same tree unpacked in a folder is also a mod: `napple mods pack <folder>` turns it into a package and every `napple mods` command accepts either.

```
mod.json              required: identity and description
manifest.sha256       required in a package (written by pack): "<sha256>  <path>" for every other file
patches/              code patches: *.pchtxt and <BUILDID>.ips
cheats/<BUILDID16>.txt   Atmosphere cheat files
files/                a romfs overlay: files/<romfs path> shadows the game's file
config.json           per-title settings
shaders/<hash>.msl    shader replacements (validated now, used by a later gfx hook)
thumbnail.png         optional, <= 1 MiB (the name is given by mod.json "thumbnail")
```

Paths use `/`. Absolute paths, `..`, `.`, empty components, backslashes, drive letters, control characters, symbolic links, encrypted entries and duplicate names are refused (the package is rejected, not repaired). Limits: 50 000 entries, 8 GiB uncompressed, 1 GiB per file, compression ratio at most 2000:1.

## mod.json

```json
{
  "format": 1,
  "id": "example.synthetic-fix",
  "name": "Synthetic Fix (example)",
  "version": "1.0.0",
  "titleId": "0100E4A100000000",
  "buildIds": ["0123456789ABCDEF0123456789ABCDEF"],
  "author": "Napple docs",
  "license": "CC0-1.0",
  "requires": { "napple": ">=0.0.1" },
  "components": { "patches": ["patches/fix.pchtxt"], "cheats": [], "romfs": false, "config": false, "shaders": false },
  "description": "What it does, in plain words.",
  "thumbnail": "thumbnail.png",
  "signature": null
}
```

| Field | Rule |
|---|---|
| `format` | `1`. Packages with another number are refused. |
| `id` | 3 to 64 of `a-z 0-9 . _ -`, not starting with a symbol. Stable across versions; reverse-DNS style is recommended. |
| `name`, `description` | Human text (name 1..80 characters, description up to 4000 bytes). |
| `version` | `MAJOR.MINOR.PATCH` (a `-pre` or `+build` suffix is allowed). |
| `titleId` | The application title id, 16 hex digits. |
| `buildIds` | The main-module build ids of the game versions the mod was made for (1 to 64 hex digits each, trailing zeros may be dropped). Empty means "claims to fit every version" and gets a warning. |
| `author`, `license` | Free text; empty values are warned about. Say who may share the mod (an SPDX id is best). |
| `requires.napple` | `>=X.Y.Z`; the mod is refused by older Napple versions. |
| `components` | What the mod claims to contain. If the key is absent `pack` derives it. If present it must match the files exactly: a patch, cheat file, overlay, config or shader that is not declared is an error, so the capability summary cannot lie. |
| `thumbnail` | A PNG or JPEG inside the mod, at most 1 MiB. |
| `signature` | Reserved for a detached signature. Must be `null` or absent in format 1. |

Unknown keys give a warning and are ignored (forward compatibility); they are never interpreted.

## Patches

### Offsets

Patch records use **NSO file offsets**: the offset in the module file *including its 0x100-byte header*. The module offset (the offset in the decompressed text, rodata and data segments laid out at their memory offsets) is `file offset - 0x100`. This is the Atmosphere IPS convention and the one pchtxt tools use with `@flag offset_shift 0x100` (disassembler addresses start at 0 for the first byte of `.text`, so the shift of 0x100 turns them into file offsets). A record that falls in the header, outside every segment, in `.bss`, or across two segments is a **miss**: nothing is written and the apply report lists it.

A patch targets one module by **module build id** (the 32-byte id in the NSO header at offset 0x40): `main`, `rtld`, `sdk` and every `subsdk*` have their own. `napple mods build-id <file.nso>` prints it.

### pchtxt

```
// free comment lines and trailing comments after a record
@nsobid-0123456789ABCDEF0123456789ABCDEF     module build id (1..64 hex digits, missing digits are zero)
@flag offset_shift 0x100                     added to every following address

// Widescreen 21:9 [author]                  the comment line right before @enabled names the block
@enabled                                     default on; @disabled = default off
00001234 1F2003D5                            ADDRESS VALUE (hex bytes, written in file order, no 0x needed)
00001238 "text"                              or a quoted string: \n \t \\ \" \xHH escapes, no terminating NUL is added
@stop                                        ignore the rest of the file
```

* Several `@nsobid-` sections (several modules or game versions) may live in one file; a record belongs to the last `@nsobid-` and the last `@enabled`/`@disabled` block.
* Records outside a block, a block before any `@nsobid-`, odd-length hex values, empty values, addresses past 4 GiB and over a million records are errors. Unknown `@directives` and flags are warnings and ignored.
* The block names are what the UI and `--enable`/`--disable` use: `patches/fix.pchtxt#Widescreen 21:9 [author]`.
* JSON form (`napple mods convert a.pchtxt a.json`): `{"format":"napple-patch-json-1","builds":[{"buildId":"...64 hex...","patches":[{"name","enabled","records":[{"offset":<file offset>,"data":"hex"}]}]}]}`. The shift is folded into the offsets, so pchtxt -> JSON -> pchtxt keeps the meaning, not the comments or the shift.

### IPS and IPS32

`patches/<BUILDID>.ips`, the file name is the module build id (Atmosphere style, trailing zeros may be dropped). Header `PATCH` (3-byte offsets, `EOF` marker) or `IPS32` (4-byte offsets, `EEOF`), plain records, RLE records (size 0, then a 2-byte run length and the byte), the optional truncate length after the marker. An offset that happens to spell `EOF` is a record unless it is really the end of the file. Truncation is parsed but **never applied to a module** (it is reported as a miss), since changing a module's size would move every address. IPS has no per-block switch: the whole file is one patch.

## Cheats

`cheats/<BUILDID16>.txt`, named by the first 16 hex digits (8 bytes) of the main module's build id (Atmosphere layout). Text format: `[Name]` starts a cheat (up to 64 bytes), `{Name}` a master code, following lines hold 8-digit hex words separated by spaces; `//` lines are comments.

### Cheat opcode subset

Napple decodes and runs the subset below (compatible with Atmosphere code types 0, 1 and 2). Anything else decodes as "unsupported", the decoder stops at that word and `validate` warns, so a cheat file with extra codes is still usable up to that point.

| Words | Meaning |
|---|---|
| `0TMR00AA AAAAAAAA VVVVVVVV (VVVVVVVV)` | **Store**: write value `V` (`T` bytes: 1, 2, 4 or 8; the 8-byte form has a fourth word) at address `A` (40 bits: low byte of the first word then the second word) in region `M`. `R` (offset register) must be 0. |
| `1TMC00AA AAAAAAAA VVVVVVVV (VVVVVVVV)` | **Begin conditional**: run the following ops only if the value at `A` compares to `V` with `C`: 1 `>`, 2 `>=`, 3 `<`, 4 `<=`, 5 `==`, 6 `!=`. |
| `21000000` | **Else** of the innermost conditional. |
| `20000000` | **End conditional**. Conditionals nest; an unmatched end/else or a missing end is an error in the decoder. |

Regions `M`: 0 main (module memory), 1 heap, 2 alias, 3 aslr base. Values are little endian. The interpreter (`cheats::run`) works on a `CheatMemory` trait: the runtime supplies the real memory, which is a Mac follow-up. Planned next (not yet decoded): loops, offset registers, key-press conditions, arithmetic.

## RomFS overlay

Everything under `files/` is a romfs file: `files/ui/title.bin` replaces `/ui/title.bin`. A marker `files/ui/title.bin.delete` (content ignored) hides that path; a marker on a directory path hides everything below it. With several mods enabled they are layered in the user's order, the later mod wins, and a delete only affects what lower mods provided. If a mod contains both a file and its `.delete` marker, the marker wins. Conflicts between mods are reported so the UI can show them. Path rules are the archive path rules above.

## config.json

```json
{ "renderScale": 1.5, "aspect": "21:9", "fpsMode": "60", "fixes": { "skipIntroLogos": true } }
```

`renderScale` 0.25..8.0; `aspect` `native` or `W:H`; `fpsMode` `default`, `30`, `60` or `unlocked`; `fixes` is a map of named boolean toggles (`[A-Za-z0-9_-]`, up to 64 characters) whose meaning is defined per title. Several mods merge key by key, later mods winning. Unknown keys are warnings.

## Shader replacements

`shaders/<program-hash>.msl`: a Metal Shading Language source for the shader program with the given hash (16 to 64 hex digits). Only validated today: UTF-8, at most 1 MiB, `#include <metal_stdlib>`, balanced brackets, at least one `vertex`, `fragment` or `kernel` entry point. The gfx hook that loads them is a later Mac step; shaders are never compiled or run on Linux.

## Hash manifest and identity

`manifest.sha256` lists `<sha256>  <path>` for every file except itself, sorted by path. `validate` checks that every listed file matches and that no file is unlisted. `pack` is deterministic: the same folder always gives the same bytes (sorted entries, fixed timestamps, mode 0644). Two identities are shown by `inspect`: the **package hash** (SHA-256 of the `.napmod` file) and the **content hash** (SHA-256 of the manifest text, the same for a folder and the package built from it unless `pack` had to add `components`).

## Trust model

* **Unsigned by default.** The `signature` field is reserved for a detached signature in a later version and cannot be set in format 1; nothing is verified against a key today, so the author and hash shown are claims, not proof. Install mods from people you trust.
* **Capabilities are shown before anything is enabled**: *code patches*, *cheats*, *romfs files*, *config*, *shaders*, counted from the files (a `components` list that disagrees is an error). `napple mods inspect` prints them with the author, license, package and content hashes.
* **Code patches, cheats and shaders default to off.** Enabling a mod does not enable its code patches: `Selection { enabled, code_patches, cheats, shaders, overrides }` has them all off by default, and the apply hook skips (and reports) everything not enabled. Individual pchtxt blocks can be switched with overrides.
* **Never auto-download.** Napple has no mod store and fetches nothing; you place the file yourself. (Hooks for a catalogue, if one is ever added, must also show this summary and ask first.)
* **Mods cannot reach outside themselves.** Archive and overlay paths are normalised and traversal is refused when the package is read, not only when it is applied; the library reads files by exact name from the listing.
* **No game data.** Mods must not contain copyrighted game data. `validate` *errors* on key files (`prod.keys`, `title.keys`, `*.keys` names), and *warns* on container and module dumps (`.nsp .xci .nca .xcz .nsz .nso .nro .elf`, `romfs.bin`, `main`, `rtld`...), on a single file over 256 MiB, on an overlay over 1 GiB or 5000 files, on patch records over 1 MiB and on more than 4 MiB of patch bytes. The size and type policy is a heuristic, not a guarantee: the author is responsible for what they publish.

## Validation reference

`napple mods validate <mod> [--json]` exits 1 when there are errors. Errors: unreadable or unsafe archive entries, an invalid `mod.json` (fields above), components that disagree with the files, any component that does not parse (pchtxt, IPS, cheats, config, shaders, thumbnail), a hash manifest that is missing from a package or does not match, key files, a set `signature`. Warnings: unknown keys, empty author, license or `buildIds`, case collisions, unsupported cheat opcodes, records inside the NSO header, unknown pchtxt directives, dump-like content (above) and an empty mod.
