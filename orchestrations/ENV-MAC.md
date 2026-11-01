# Mac environment notes for the unattended batches (2026-11-01-r1, -r2, ...; read by every item)

This checkout is **flat**: the git root `/Users/neto/napple` IS the Napple tree (no outer `Documents/Napple`, no `Napple/` prefix). Ignore the skills' "outer folder" gotcha; run git at the root of your worktree.

## Rules for this batch (set by the user, who is away)
- **Never ask the user anything.** Decide, note the decision in your integrator file, keep going. If something is truly impossible, hand off with `status: blocked` and the reason.
- **Local only: do NOT `git push`** (overrides `/ura-napple-parallel` step 4.4). Commit on your branch; the integrator reads local branches.
- Another batch may still be running in other worktrees: never touch paths outside your file's Owned paths, even if they look stale.
- Hand-off message: when done, `SendMessage` to the orchestrator session named in your file: `<name> handed off: <branch> <head>`.
- 16 GB RAM / 10 cores shared by 5 sessions: no clean rebuilds of the whole tree, build only your areas, never leave a game process running.

## Test material (real files)
- Games, keys, firmware: `~/Desktop/switchfiles` (export `NAPPLE_TESTING_MATERIAL=~/Desktop/switchfiles`)
  - keys `~/Desktop/switchfiles/Keys 22.1.0/prod.keys` (+ `title.keys`), firmware `~/Desktop/switchfiles/Firmware.22.1.0.zip`
  - SMO `Super Mario Odyssey™ [0100000000010000][v0][Base].nsp`, LAS `The Legend of Zelda Link’s Awakening [01006BB00C6F0000][v0].nsp`,
    MK8D folder (base + v1376256 update + Booster DLC), ACNH folder (base + 3.0.1/3.0.3 updates + 3 DLCs)
- Built apps from Studio (do not modify): `~/Applications/Napple/*.app`.

## Shared work dir (title data is built ONCE for all sessions)
- `ln -s /Users/neto/NappleWork work` at your worktree root (already git-excluded). Data lives in `/Users/neto/NappleWork/games/<titleid>/<buildid>/`.
- Build title data only if `ls /Users/neto/NappleWork/games/<titleid>` is empty, and only under the build lock:
  ```bash
  cargo build --release -p napple-cli 2>/dev/null || cargo build --release   # target/release/napple
  lockf -k /Users/neto/NappleWork/.build.lock target/release/napple build "<nsp>" -k "$HOME/Desktop/switchfiles/Keys 22.1.0/prod.keys" \
     --firmware "$HOME/Desktop/switchfiles/Firmware.22.1.0.zip" --updates latest --dlc all --update-romfs in-place --data-only -o /Users/neto/NappleWork/games
  ```
  Re-check inside the lock (another session may have built it meanwhile). `--update-romfs in-place` keeps each title ~250 MB.
- Headless game runs (memory heavy) go through the run lock, one at a time machine-wide:
  `lockf -k /Users/neto/NappleWork/.run.lock tools/boot_test.sh <titleid> <seconds> [args]` (or the runtime binary directly with `--headless --seconds N`). Keep runs ≤ 120 s each. `tools/boot_test.sh` writes saves into the shared `work/saves`: prefer the runtime binary directly with a private copied `--save-root` (e.g. `$TMPDIR/<your-name>/saves`).
- Builds that need `$TMPDIR` (codesign) use your own subfolder: `$TMPDIR/<your-name>/`.

## Status words
verified (ran here on the Mac, with a real title where the acceptance says so) / tested / untested / proposed. Coverage only rises for *verified* work.
