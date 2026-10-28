# Making and sharing a mod

This page walks through a code fix. It uses the synthetic example in [examples/synthetic-fix](examples/synthetic-fix/mod.json); replace its made-up ids with your game's. Read the [format](format.md) page for the exact rules.

## 1. Find the build id

Patches are keyed by the **module build id**: the 32-byte id in the NSO header (offset 0x40) of each module. The `main` module's id also names the game version.

* From the game's ExeFS: `napple build --dump-exefs <game> -k <prod.keys> -o exefs/`, then `napple mods build-id exefs/main` (and `exefs/subsdk0`, ... for the other modules). The dump contains game code: keep it out of your mod and out of git.
* From a build report: `napple decomp <game files> -k <prod.keys> --json` lists every module with its full `build_id`.
* The runtime data directory of a built game is named after the first 16 hex digits of the `main` build id (`work/games/<titleid>/<buildid16>/`).

Put the `main` ids of the versions you tested in `mod.json` `"buildIds"`. A patch made for one version does not fit another: addresses move.

## 2. Write the patch

Pick the format you like; both end up as the same thing.

**pchtxt** (readable, can carry several versions and named, switchable blocks):

```
@nsobid-0123456789ABCDEF0123456789ABCDEF
@flag offset_shift 0x100

// Skip intro check [yourname]
@enabled
00000040 00008052        // mov w0, #0
00000100 C0035FD6        // ret
```

The addresses are the ones your disassembler shows (`.text` starts at 0); `offset_shift 0x100` turns them into NSO file offsets. Values are bytes in file order: `00008052` is `mov w0, #0` (little-endian `52800000`). Strings: `00001010 "Patched"`. Mark risky or optional blocks `@disabled`.

**IPS** (compact, what many existing fixes use): name the file `patches/<BUILDID>.ips`; the offsets are file offsets already (header included). You can convert in either direction:

```
napple mods convert fix.pchtxt FEDCBA9876543210FEDCBA9876543210.ips     # one build id per IPS file
napple mods convert 0123...IPS.ips fix.pchtxt --build-id 0123456789ABCDEF0123456789ABCDEF
napple mods convert fix.pchtxt fix.json                                  # editable JSON form
```

Keep patches small. Only write the bytes you change; a mod must never contain the original game's code or data.

## 3. Cheats, files, settings (all optional)

* Cheats: `cheats/<first 16 hex digits of the main build id>.txt` in the Atmosphere text format; only the [opcode table](format.md#cheat-opcode-table) runs (a cheat using any other code type is skipped as a whole).
* Replacement files: put them under `files/` at the romfs path (`files/ui/title.bin` replaces `/ui/title.bin`). Add `files/path.delete` to hide a file. Use only files you made yourself.
* Settings: `config.json` with `renderScale`, `aspect`, `fpsMode`, `fixes`.

## 4. Describe it

`mod.json` needs `id`, `name`, `version`, `titleId`, `buildIds`, `author`, `license` and a plain description of what changes and any known problems. Say what the patch does to the game and what it needs (for example "needs update 1.2.0").

## 5. Check, test, pack

```
napple mods validate my-mod/                # errors must be 0; read the warnings
napple mods apply my-mod/ --image module.bin --build-id <BUILDID> --enable-code --dry-run
napple mods pack my-mod/ -o my-mod-1.0.0.napmod
napple mods inspect my-mod-1.0.0.napmod     # what your users will see
```

`apply --dry-run` prints a **hit** for every record written (offset, segment), a **MISS** for every record that did not land (outside the module, in the header, across two segments), overlaps between records, and what was skipped because it is not enabled, with the module's hash before and after. A fix that shows a miss is wrong for that build. `module.bin` is a flat image of the module's text, rodata and data at their memory offsets; `--layout <text_end>,<ro_end>` tells the tool where the segments split (it defaults to everything being text). Until the game integration lands (see [Integration](integration.md)) the way to test a fix on a real game is to run the game with the patched build; the dry run is how you check offsets and that the patch applies to exactly the bytes you expect.

Test on every build id you list, boot the game headless for a minute, and then play the part you changed.

## 6. Publish

Share the `.napmod` file anywhere you like (a release page, a forum post). Put the package hash from `napple mods inspect` next to the download link so people can check it. Do not include game data, keys or firmware; do not promise that a mod is safe, say what it contains. Napple never downloads mods; people install them on purpose and see your author, license, hashes and capabilities first.

Updating: raise `version`, keep `id`. Do not reuse a version number for different content.
