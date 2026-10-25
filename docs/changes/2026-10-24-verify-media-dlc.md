# verify-media-dlc (plan7): scripted checks for H.264, DLC and the flaky guest Break

All runs: Mac, headless, scripted (no one played). Runtime built from this branch (base 272aeda) in `$TMPDIR`; `~/Desktop/NappleLATEST` untouched.
Game data: main checkout `Napple/work/games` (LAS 01006BB00C6F0000, SMO 0100000000010000, built 2026-10-22). Default save root unless noted.

## 1. SMO / H.264 (VideoToolbox)
- SMO never creates a `movie::Decoder` in any scripted path I tried (log has zero `movie:` lines): idle 150 s on the title menu; Action Guide (LS_DOWN x3, A); New Game
  (A every 4 s for 110 s, fresh save root with `system/` copied in) which reaches the in-engine opening cutscene (screenshots at frames 3300 and 4500), still no movie.
  So no scripted path in SMO reaches a movie in the first ~2 minutes; SMO's early flow looks to use in-engine cutscenes only. SMO modules do link the movie library (subsdk0), so a later scene may.
- LAS does create decoders at ~0.5 s: type 3 (VP9, texture mode 2, format 3, runs on the host, 1080p, ~0.6-0.9 ms/frame) and a second one, **type 5, format 1, buffer mode 1**.
  That type-5 object is the only unknown decoder any title makes in the first seconds. Default run: "left to the library".
- With `NAPPLE_MOVIE_H264_TYPE=5` the VideoToolbox path IS reached on LAS:
  `movie: H.264 decoder 0x... on the host (VideoToolbox), mode 1 (buffer), output format 1` then
  `output format 1 not implemented on the host (NV12 = 4 only)` then `decoder ... destroyed after 0 frames`; the game keeps running (exit 0, VP9 movie unaffected).
  So the hook, the type override and the VT decoder creation work against a real guest; **no H.264 frame was decoded** (format 1 is refused, and type 5 may not be AVC at all:
  it is created next to the VP9 decoder, so it may be an audio or auxiliary decoder). Evidence logs in the shared notes folder (`las-h264-0.log`, `las-h264-5.log`).
- Fix needed (not mine, runtime/movie.c): confirm the real AVC DecoderType value (a title that actually plays an H.264 movie is needed; TOTK/other titles use VP9) and decide what format 1 means.

## 2. Animal Crossing, updates and DLC
- Command: `napple build <3 nsp> -k prod.keys --title-keys ... --firmware firmware.zip --updates all --dlc all --data-only -o $TMPDIR/vmd/ac` (2m15s, 26 GB, 3 build dirs, one per version).
- **There is no DLC in `Testing Material/AnimalCrossing+Updates+DLC`.** The three files are the base (`01006F8002326000` v0) and two updates (`01006F8002326800` v2097152 = 3.0.1 and v2228224 = 3.0.3).
  Plan log: `2 update(s), 0 DLC selected` / `3 version(s), 0 DLC`. The `[01006F8002326800]` file looks like DLC by its id but is the update title id (base + 0x800). So `aoc:u` cannot be shown with real content; this needs an AOC `.nsp`.
- Boot, 45 s headless, each build dir: **none boots.**
  - 3.0.3 (`ff1d1c05670db602`) and 3.0.1 (`8f2cb7a9774959c8`): exit 70 at 0.43 s, `ipc: TIPC request (cmd 0) is not implemented` / `FAIL: TIPC not implemented`, right after `boot: loader callback: modules linked`, before any aoc/fs call. `NAPPLE_IPC_LENIENT=1` does not help (`ipc.c` fails on type >= 16 unconditionally, `ipc.c:314`).
  - v0 (`7fc1baff976aeca4`): SIGSEGV fault=0x20 pc=main+0x19a6250 at startup (exit 139).
  - So no `aoc:u` / fs cmd 30 lines were produced. The unit-level coverage of those (cleanup-small's fs cmd 30 test) is the only DLC evidence.
- Fix needed (runtime/ipc.c, not mine): a TIPC (tiny IPC, command type = 16 + id) dispatch path, starting with the `sm:` commands the SDK sends at init. AC's SDK is a newer libnx/NX SDK that uses TIPC.

## 3. "guest Break ... sdk+0xd5f6c4, exit 70" flakiness
- 5 runs x 12 s on each of LAS and SMO with the branch build and with `~/Desktop/NappleLATEST/bin/napple-runtime`: **20/20 exit 0**, no FAIL/CRASH line (twice: once on an older base, once on this base).
- Not reproduced, so I cannot call it flaky or build-specific from these data. Observations:
  - The reported failure timing (0.43 s) equals the AC TIPC failure timing, i.e. the early-boot class. The binary identity matters less than the **game data**: the report may have come from a game built with a different (Studio) build than `work/games`.
  - An empty `--save-root` (no `system/fonts`) crashes both games at ~0.47 s with `CRASH signal=11 fault=0x38 pc=main+0x111e000` after `pl: no shared fonts ...`.
    That is exactly the earlier "unreproducible boot-crash" signature (fault 0x38, main+0x111e000): its cause is a missing `<save-root>/system/fonts` (fonts-fallback owns the fix in services/pl.c). Worth checking that the exit-70 run also used a save root without fonts.

## Files
`Napple/tools/flaky_boot.sh` (new; N sequential headless boots of one runtime on one data dir, prints exit code and first FAIL/CRASH line; `SAVE_ROOT=` to override), this note.
