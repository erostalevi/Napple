# Support matrix

Per-title status per platform. **Generated** by `tools/support/gen_support.py` from `compat/matrix.json`, the [title notes](../titles/README.md) and the iOS column of their table; do not edit by hand (`python3 -I tools/support/gen_support.py` rewrites it, `--check` fails when it is stale). Metadata only: no game bytes, keys or firmware.

Last `napple compat run`: 2026-10-21T02:18:02Z. Platforms: macOS is the goal now; iOS is deferred (its column is the last Simulator result recorded in the title notes). Windows and Linux are not goals.

Legend (macOS stages, from the matrix): `OK` pass, `~` partial, `X` fail, `B` blocked, `.` untested, `n/t` title not in the matrix (no scripted stage result; read the note).

## macOS

| Title | Id | Boot | Graphics | Audio | Input | Saves | Performance | Known issues | Note |
|---|---|:-:|:-:|:-:|:-:|:-:|---|---|---|
| Super Mario Odyssey | `0100000000010000` | OK | ~ | ~ | ~ | ~ | Live play: steady 60 fps, GPU 6-12 ms, 4 of 51 five-second windows stalled (worst 990 ms at the scene change, 656 pipelines built in one window). | graphics: boot UI and Assist Mode screens render; audio: audren REV3: 0 underruns/overruns over 70 s on Core Audio; input: A and Plus reach the game through the shared HID path (Assist Mode select, demo skip); +4 more in the note | [note](../titles/0100000000010000.md) |
| Mario Kart 8 Deluxe | `0100152000022000` | n/t | n/t | n/t | n/t | n/t | not recorded | The ~45 fps ceiling is gfx: guest blocked ~21 ms per present in SyncWait, per-submit commit to complete ~21 ms, present GPU <= 8 ms, replay CPU 0.7 ms (LAS runs 58 fps with the same code).; Resolved 2026-10-26 (gfx-correctness): stale command handles (lists now live as long as their command memory), the compressed render-target shadow is copied back, a vertex stream outside every pool binds zeros (and ...; iOS Simulator: the G-buffer pass needs 48 bytes of colour attachment, the Simulator allows 32 (base, AArch32); +3 more in the note | [note](../titles/0100152000022000.md) |
| Link's Awakening | `01006BB00C6F0000` | OK | OK | ~ | ~ | OK | 60 fps live windowed in a 3-6 minute play (perf-las-live), GPU 6-12 ms | audio: Core Audio output verified clean: 0 underruns/overruns over 95 s (frames requested = wall time x 48 kHz); input: keyboard verified end to end in the packaged app (real HID key events, window key, title L+R -> game); First-use pipeline/shader build stutter (170-1300 ms when entering a new area); +2 more in the note | [note](../titles/01006BB00C6F0000.md) |
| Animal Crossing: New Horizons | `01006F8002326000` | n/t | n/t | n/t | n/t | n/t | ~30 fps windowed (240 s run), 1738 frames in 70 s headless with input. | iOS Simulator: 100 s, no crash, no first frame.; v0 was not re-run after the clock/olsc/prepo/pool-race changes (its data was deleted).; Thread 0x56 is created but never started in every boot (harmless). | [note](../titles/01006F8002326000.md) |

### State per title (macOS)

- **Super Mario Odyssey** (`0100000000010000`): plays on macOS (intro, menus, Cascade Kingdom); start menu on the iOS Simulator (a touch A did not visibly advance).
- **Mario Kart 8 Deluxe** (`0100152000022000`): v3.0.5 (update v1376256 + Booster Course Pass DLC) boots and races headless on the normal 64-bit path at 40-47 fps (title, menus, cup select, races). The base 1.0 build is AArch32 and runs through the lifted `a32jit.dylib` (written by `napple build`) at 52-60 fps in headless menus and the attract race; the runtime has no interpreter (2026-10-26, a32-full).
- **Link's Awakening** (`01006BB00C6F0000`): plays on macOS (the most mature title); reaches its title screen on the iOS Simulator.
- **Animal Crossing: New Horizons** (`01006F8002326000`): version 3.0.3 (+3 DLC) boots through the whole opening (Timmy's dialogue, the Nook Inc. counter, the name-entry keyboard) and shows its picture through the game's own window pass (2026-10-26: `nvnProgramSetShaders` with pool-offset code addresses is accepted, see the gfx-correctness note; the stopgap present is gone; Timmy and Tommy and other skinned meshes appeared once the translator read ...

## iOS (deferred)

| Title | Id | Last recorded iOS Simulator result |
|---|---|---|
| Super Mario Odyssey | `0100000000010000` | start menu; abort on A (48-byte colour attachments) |
| Mario Kart 8 Deluxe | `0100152000022000` | 3.0.5 aborts at ~2 s (48-byte colour attachments; time:u fixed) |
| Link's Awakening | `01006BB00C6F0000` | title screen (2026-10-26 re-run) |
| Animal Crossing: New Horizons | `01006F8002326000` | abort at ~2 s (48-byte colour attachments) |

See also [COMPATIBILITY.md](../../COMPATIBILITY.md) (the full matrix with stage evidence), the [coverage by feature](feature-support.md) and the machine-readable [support-data.json](support-data.json).
