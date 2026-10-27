# Document a title (for contributors)

## Goal
Add or improve the catalogue entry of a game you own: run the scripted headless boot, record what happened as a title page and as compatibility data, and open a pull request. You do not need to write C or Rust.
This is the contributor version of [add-game-support.md](add-game-support.md) (which also covers fixing what blocks a boot). Field-by-field reference: [catalogue field reference](../reference/catalogue-howto.md).

## Requirements
An Apple Silicon Mac, the Xcode command line tools, Rust and the build from [CONTRIBUTING.md](../../CONTRIBUTING.md#build), your own `prod.keys`, optionally the firmware, and a legally owned copy of the game (base, and update/DLC if you have them).
**Never commit or upload** any of those, or anything extracted from them ([legal rules](../../CONTRIBUTING.md#legal-rules-first)).

## Steps
1. **Claim it.** Open the *Document a title* issue form (or comment on an existing one) so nobody duplicates the work. Check [titles/README.md](../titles/README.md) and [COMPATIBILITY.md](../../COMPATIBILITY.md) first: the title may already have a page to improve.
2. **Build the data directory** (data-only, in-place RomFS to save disk):
   ```bash
   target/release/napple build <game files or folder> -k <prod.keys> --firmware <Firmware.zip> --updates latest --dlc all --data-only --update-romfs in-place -o work/games
   ```
   The last line is `RESULT <path>`; the data directory is `work/games/<TITLEID>/<buildid16>` (newest version for an updated game). Details and pitfalls: [add-game-support.md](add-game-support.md#steps).
3. **Scripted boot (45 s, headless).**
   ```bash
   mkdir -p /tmp/napple-save
   runtime/build/napple-runtime work/games/<TITLEID>/<buildid16> --headless --seconds 45 --save-root /tmp/napple-save \
       --summary work/compat/<TITLEID>/<buildid16>/runtime-boot.json 2> work/logs/<TITLEID>-boot.log; echo "exit $?"
   ```
   Passing: exit 0, a `[napple-gfx] stats: frames N` line, no `CRASH`, no `FAIL:`. `tools/boot_test.sh <titleid> 45` does the same and writes the stage file for you. To get past a title screen use scripted input: `NAPPLE_INPUT_SCRIPT="10:A,12:PLUS:0.3"` ([debugging.md](debugging.md)). Install real fonts into `/tmp/napple-save/system/fonts` for text (`napple build --dump-fonts`).
4. **Add the game to the matrix and record stages.**
   ```bash
   napple compat run <folder with the game> -k <prod.keys>     # first four stages, adds the game
   napple compat render                                        # merges work/compat/ stage files
   napple compat set <TITLEID> video partial "frames flow, one menu flickers" --evidence work/logs/<TITLEID>-boot.log
   ```
   Honest statuses only: a headless boot says nothing about audio or input quality ([status meanings](../reference/catalogue-howto.md#stage-results-compatmatrixjson)). Evidence paths are under `work/`, which is gitignored: put the useful log lines in the page instead.
5. **Write the page** `docs/titles/<TITLEID>.md` in the style of [the existing pages](../titles/01006BB00C6F0000.md): `# <Name> (<TITLEID>)`, a `**State:**` line saying what was measured and how, `## What it needs`, `## What is open`, `## Build and test` (exact commands, expected frames and exit), `## Numbers`.
   Use the project's status words: *verified*, *tested*, *unbuilt*, *untested*. Do not write that anyone "played" it unless a person did.
6. **Index it.** Add one row to the table in [titles/README.md](../titles/README.md) (the iOS column: `not run` is fine). Add a row to [symptoms.md](../symptoms.md) for every symptom you root-caused (four non-empty cells).
7. **Regenerate and check.**
   ```bash
   python3 -I tools/support/gen_support.py
   python3 tools/check_docs.py
   ```
   Commit the three regenerated files in `docs/reference/` with your change. `check_docs.py` fails on a broken link, an orphan page or a stale table.
8. **Pull request.** Sign off your commits (`git commit -s`), fill in the template, tick "Catalogue / title page / docs only". `compat/matrix.json` and `COMPATIBILITY.md` come from the tool; if they changed more than your title, say why.

## What a maintainer will check
Title id equals the file name; every claim has a command or a log line behind it; no game bytes, no key material, no absolute paths from your machine; status words are honest; the generated tables are fresh.

## Pitfalls
Updated games have several `<buildid16>` directories (the base boots the old version). Do not materialise 19 GB of RomFS. One game process at a time uses the GPU. More in [add-game-support.md](add-game-support.md#common-pitfalls).
