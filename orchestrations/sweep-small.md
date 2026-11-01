---
name: sweep-small
batch: 2026-11-01-r1
branch: claude/sweep-small-2026-11-01-r1
base: orchestrate/2026-11-01-r1
env: mac
model: claude-sonnet-5-5
depends_on: []
status: launched
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_afc5486b-71f2-45b6-b9f1-22a1a94901ac
launched_at: 2026-11-01
---

# sweep-small: close seven small partial items in audio, input, content and bcat

## Goal
Turn these %Analysis items from partial to impl, each with a test, or verified with a real title where possible (+0.39 pts total):
- `audio/performance metrics`: audren performance buffer layouts per revision (REV5..REV11+ entry/detail/header sizes), filled with plausible timings.
- `audio/audin (microphone)`: the Mac capture path (`audin_mic.c`) default-on with the permission flow and a silent fallback when denied.
- `input/GameCube / Palma / NES styles`: npad style lifo layouts and button bit positions (GC triggers, Palma, NES/SNES/N64 Lark/Lucia bits).
- `content/NSZ / XCZ (zstd)`: ingest a real NSZ end to end (make one from a Testing Material NSP if no NSZ exists: write a minimal NCZ section compressor in a test helper, or use `nsz` only if already installed; never download tools).
- `content/NCA2 / NCA0`, `content/personalized tickets`: retail-format validation and the container.rs wiring for personalized titlekeys (RSA-2048 titlekey decrypt with the console key path, tested with synthetic vectors).
- `fs_runtime/bcat storage`: bcat delivery cache storage layout checked and tested.

## Owned paths
`audio/` (except `audio/src/engine.c` debug hook: keep it), `input/`, `crates/napple-core/src/infrastructure/{ncz.rs,nca.rs,ticket.rs,ticket/}`, `crates/napple-build/src/container.rs`,
`runtime/src/services/bcat.c`, `bcat_core.[ch]`, tests next to them, `docs/archive/changes/2026-11-01-claude-sweep-small-2026-11-01-r1.md`.

## Do not touch
`gfx/`, `runtime/src/` other than the bcat files, other `runtime/src/services/*`, `frontend/`, `studio/`, `orchestrations/`.

## Starting points
`docs/architecture/audio-input-frontend.md`, `docs/architecture/runtime-services.md` (audren, hid, bcat rows), newest `docs/archive/changes/` notes for b3/b6 (npad styles, audren perf, NSZ/XCZ),
`.claude/skills/percent-analysis/verified.json` entries for these items (they say what is missing).

## Acceptance
1. Each item above: unit test passing on the Mac (`make -C audio test`, `make -C input test`, `cargo test -p napple-core`, `cargo test -p napple-build`, runtime service test for bcat).
2. audren performance: a headless SMO or MK8D boot (60 s) shows no audren perf warning; log line proving the perf buffer was written.
3. audin: with mic permission denied/absent, `audin` still returns silence without failing (test); note in the hand-off whether a real capture run was possible.
4. NSZ: `napple build --data-only` on an NSZ produces the same module hashes as the NSP (verified).

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data, locks, local-only, never ask).

## Result
