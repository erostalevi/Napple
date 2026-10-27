# Titles

Status of the four Testing Material titles. Numbers are scripted/measured (headless unless said) on an Apple M-series Mac, never "played" unless a note says a person did.
Symptom lookup: [../symptoms.md](../symptoms.md). Status as of 2026-10-26; later branches may have moved it: check the [change archive](../archive/changes/README.md) and the integrator files.

| Title | Id | macOS | iOS Simulator | Note |
|---|---|---|---|---|
| Link's Awakening (LAS) | 01006BB00C6F0000 | plays, ~58 fps headless, 60 fps live | title screen (2026-10-26 re-run) | [01006BB00C6F0000](01006BB00C6F0000.md) |
| Super Mario Odyssey (SMO) | 0100000000010000 | plays, 60 fps live | start menu; abort on A (48-byte colour attachments) | [0100000000010000](0100000000010000.md) |
| Animal Crossing: New Horizons (ACNH) | 01006F8002326000 | boots through the opening and shows its picture through the game's own window pass, ~30 fps | abort at ~2 s (48-byte colour attachments) | [01006F8002326000](01006F8002326000.md) |
| Mario Kart 8 Deluxe (MK8D) | 0100152000022000 | update + DLC runs races at 40-47 fps | 3.0.5 aborts at ~2 s (48-byte colour attachments; time:u fixed) | [0100152000022000](0100152000022000.md) |

## Building test data without disk pain (all titles)
The in-place mode writes a `romfs.map` (extents of the NSP files plus AES-CTR parameters) instead of a RomFS copy: about 250 MB per title instead of 5-19 GB. Development only: the map holds keys and host paths, never ship it, and an iOS device bundle needs the real RomFS. Needs the `napple` CLI from a branch with `--update-romfs in-place` (branch `claude/mk8d-64-2026-10-25-titles` at the time of writing).
```bash
cargo build --release -p napple-cli
target/release/napple build <nsp files or folder> -k "/Users/neto/Documents/Napple/Testing Material/Keys 22.1.0/prod.keys" --firmware "/Users/neto/Documents/Napple/Testing Material/Firmware.22.1.0.zip" --updates latest --dlc all --data-only --update-romfs in-place -o <scratch dir>
```
Run headless (the data dir is `<scratch>/<title id>/<build id>`; take the newest version dir for an update):
```bash
runtime/build/napple-runtime <data dir> --headless --seconds 45 --save-root <tmp save dir>
```
Passing: exit 0, a `stats: frames N` line with N in the thousands, no `CRASH`. Useful env: `NAPPLE_PERF_LOG=1` (fps, GPU ms, RSS), `NAPPLE_INPUT_SCRIPT="15:A,19:A:0.3"` (seconds after HID is configured : button [: hold seconds]), `NAPPLE_SCREENSHOT` (see [../reference.md](../reference/subsystems.md)).
Games live in `/Users/neto/Documents/Napple/Testing Material` (gitignored). Delete scratch data when done.
