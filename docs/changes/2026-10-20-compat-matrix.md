# Compatibility matrix (napple-compat) and NPDM/NACP parsers

## Objective
A per-game, per-version matrix of pipeline stages (ingest, build-plan, nso-load, site-scan, rewrite, rewrite-verify,
runtime-boot, video, audio, input, saves, title-screen, ingame) with status Untested/Pass/Partial/Fail/Blocked,
reasons, evidence paths and runtime counters; plus the Horizon service lists each game requests (from main.npdm),
which tell the runtime author which services to implement first.

## Files
- `crates/napple-compat/src/{model,store,render,runner,lib}.rs`: data model, stage-file IO and merge, markdown renderer,
  `compat run` use case, CLI (`run`, `show`, `set`, `render`, `npdm`). Uses serde/serde_json (already in the local registry).
- `crates/napple-core/src/infrastructure/npdm.rs` (new): META/ACI0/ACID parser, SAC service list, kernel capabilities.
- `crates/napple-core/src/infrastructure/nacp.rs` (new): control.nacp parser (names, publisher, display version).
- `crates/napple-core/src/application/package.rs`: additive `Pack::control()` (reads control.nacp from the Control NCA).
- `crates/napple-core/src/infrastructure/mod.rs`: registers the two modules. No existing signature changed.
- `compat/matrix.json`, `COMPATIBILITY.md`: generated results (metadata only). `docs/compat-format.md`: file format.

## Logic
`compat run <folder> -k keys`: finds .nsp/.xci recursively (no symlink following), `scan` each, groups packs by
application id (one BuildPlan per game; a folder can hold many games), analyzes base + all updates: Program ExeFS ->
NSO parse (hashes verified) + A64 site scan (+ distinct SVC numbers decoded from the svc sites), main.npdm, control.nacp.
Version key = first 16 hex of the main NSO build id. Writes `work/compat/<TID>/<BID>/<stage>.json` for the four owned stages,
merges with the existing matrix, overlays every stage file found (newest `time` wins, so runtime results are picked up),
sorts and writes `compat/matrix.json` and `COMPATIBILITY.md`. Re-running is idempotent apart from timestamps; manual
notes and runtime stages are preserved. A container that cannot be parsed is recorded as ingest=Fail under the title id
taken from the "[TITLEID]" tag in its file name (build `unknown`).
NPDM: SAC entries (bit 7 = hosted service, low bits = length-1), KAC descriptors decoded by trailing-ones count
(thread info, SVC mask -> allowed SVC numbers, map/IO ranges, regions, interrupts, program type, kernel version,
handle table, debug flags).

## Decisions
- rewrite / rewrite-verify stay Untested: `napple-rewrite` exposes only a CLI stub `run`, no library API. A clearly
  marked hook `runner::rewrite_hook` returns None and is where the call goes once the API exists.
- Stage files are the interchange with the C runtime (simple JSON, atomic write, tolerant reader); matrix.json is the
  committed source of truth, so it works on a machine without `work/`.
- `build-plan` is per game and recorded on each version; counters are stored per stage and unioned for display.
- Symbols in the table are ASCII (OK ~ X B .).

## Findings on the two test games (v0 base, firmware 22.1.0 keys)
- SMO `0100000000010000`: NSOs rtld, main, subsdk0, sdk (4); main build `3ca12dfaaf9c82da`. LAS `01006BB00C6F0000`:
  rtld, main, subsdk0, subsdk1, sdk (5); main `ae16f71e002af8cb`. The NSO sets differ (LAS has an extra subsdk).
- Both: 64-bit, address space type 3, main thread priority 44 core 0, main stack 0x100000, system resource 0, 512 handles,
  threads priority 28..59 cores 0..2, allow_debug set, production ACID. SMO kernel version 6.0, LAS 9.1.
- SVC use: only rtld (4: 0x6, 0x26-0x28) and sdk contain `svc`; main and subsdk have none (all kernel calls go through
  sdk). sdk holds svc numbers outside the NPDM allow-list (SMO 106 distinct svc sites, many above 0x40; LAS 61), which
  are probably unused wrapper stubs; runtime should implement the allowed list first (SMO 47, LAS 50) and treat the rest as lazily failing.
- x18: heavy (SMO 15.5k sites, LAS 29k); TPIDR_EL0/TPIDRRO and CNTPCT sites appear in LAS main (483 TPIDR in main) and in sdk for both; SMO main has none.
- Services: 42 (SMO) and 44 (LAS) client services, no hosted services. LAS additionally needs `caps:u` and `nim:eca`.
  Common core to implement first: `sm` bootstrap, `appletOE`, `apm`, `fsp-srv`, `hid`, `nvdrv`, `vi:u`, `audren:u`, `audout:u`,
  `hwopus`, `time:u`, `set`, `lm`, `pl:u`, `acc:u0`, `aoc:u`; the rest (friend, ldn, nfc, nfp, ssl, bsd, nifm, htc*, prepo, bcat, mii, irs, pcm, banana,
  ntc, nsd) can be stubbed to "offline/unsupported".
- SMO FS permissions 0x4000000000000000, LAS 0x0 (raw FAC value).

## Risks / assumptions
- KAC decoding follows Switchbrew/Atmosphere (type = trailing ones, payload from bit ones+1); a first draft was off by one
  for types >= 10 and was caught against the real data (handle table 512 is a good sanity check). Kernel version
  numbers (6.0, 9.1) are decoded but not otherwise validated.
- The sdk SVC list is static text scanning (data in code pools can fake an `svc`); SMO's 106 may include false hits.
- Updates and DLC are only exercised with synthetic data in core; `compat run` was run on two base games only.
- NSZ files are skipped (not supported by core yet).

## Tests
`cargo test -p napple-compat` (11) and `cargo test -p napple-core` (+4: NPDM synthetic and hostile, NACP).
Real-data: `napple compat run` on both NSPs (0.4 s), `compat npdm`, and a manual run of `set` plus a C-style runtime stage file
through `render` in a temp dir. Recommended: a real update pack and an XCI once available; a unit test for `runner::run` needs a fixture container.
