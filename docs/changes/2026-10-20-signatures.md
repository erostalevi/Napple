# Shared signatures: schema, stores, matcher, resolver, REA adapter, share flow

## Objective
Implement step [3 resolve] and section 6 of ARCHITECTURE.md: signatures live as data in
`knownSignatures/<titleId>/<buildId>.json`; a miss is solved by REA/Ghidra, validated and kept locally as
"pending-share"; sharing is opt-in, previewed byte for byte, and never happens by default.

## Files
- `knownSignatures/schema.json`, `knownSignatures/README.md`, `knownSignatures/_examples/0100FFFF00000000/<build>.json`
  (3 synthetic entries: service-hook, patch-site, struct; `_examples` is ignored by lookups, validated by a test).
- `crates/napple-sigs/src/`
  - `domain.rs`: `Signature`, `SignatureFile`, `Pattern` (bytes + bit mask), A64 operand masking, pattern hash, ids.
  - `validate.rs`: copyright/privacy validator.
  - `repo.rs`: `SignatureRepository` port; `DirRepository`, `LayeredRepository`, `GitCacheRepository`.
  - `reverser.rs`: `Reverser` port; `ReaCliReverser` (+ fake-`rea` test helper).
  - `resolver.rs`: `Module` (NSO/raw), `match_module`, `derive_pattern`, `resolve_item` pipeline.
  - `share.rs`: `build_plan`, `preview`, `execute` (gh/git).
  - `exec.rs`: process runner (no shell, env cleared, hard timeout incl. process group, output cap).
  - `cli.rs`: `napple sigs lookup|match|resolve|pending|share|validate|sync`.
- `Cargo.toml` of napple-sigs: serde, serde_json, sha2 0.10, dev: tempfile. `docs/tooling.md` (install record).
No other crate was modified.

## Logic
- **Pattern**: up to 16 A64 words. Relocatable operands are wildcarded at bit level (B/BL imm26, ADR/ADRP, LDR literal,
  B.cond, CBZ/CBNZ, TBZ/TBNZ, and the imm12 of ADD/LDR/STR that consumes a preceding ADRP). `pattern` has the masked
  bits zeroed; `mask` marks compared bits. **Hash** = first 128 bits of sha256("napple-sig/1\0", len, pattern, mask);
  it is the key of the entry, so the same code in another build (or after relinking) hashes the same.
- **derive_pattern**: shortest pattern (12..64 bytes, >= 8 pinned bytes) occurring exactly once in the module text.
- **resolve**: derive pattern -> (1) hash found in this build: `known`; (2) found in another build of the title:
  `carried` (new local entry for this build, confidence capped at medium); (3) Reverser (REA) gives function bounds
  from the item's offset -> `learned` (provenance `rea`, confidence `medium`); (4) if no Reverser result but the user
  gave a size: `local`, confidence `low`; else `UNRESOLVED`. Every new entry is validated, then stored in
  `work/sigs/pending`. Lookups see pending + shared + `knownSignatures/` + `work/sigcache` (merged, earlier wins).
- **match**: scans text (code kinds, 4-byte aligned) / all segments (other kinds); reports hits (flagging ambiguous or
  outside the recorded range), unknowns (signature absent from the module) and signatures for other modules.
- **share**: `--dry-run` or no flag = preview only; the preview prints target repo, branch (derived from content hash),
  neutral identity, PR text, the planned commands and the EXACT file contents. `--yes` runs: `gh auth status` ->
  `gh repo fork` -> shallow clone in `work/sigshare` (git config isolated: HOME sandbox, `GIT_CONFIG_GLOBAL=/dev/null`, fixed
  author/committer `Napple Signatures <signatures@napple.invalid>`, UTC-midnight timestamps) -> merge into any existing
  upstream file (upstream wins) -> commit -> push to the fork using gh's credential helper via `-c` (no global config
  change) -> `gh pr create`. On success pending files move to `work/sigs/shared`.
- **REA adapter**: `rea function <slice> <addr> --target-format raw-binary --architecture arm64 --entry-point <addr>
  --provider ghidra --format json`. The slice (<= 64 KiB of text starting at the item) is written 0600 under
  `work/rea-in`, must canonicalise inside work/ (symlinks included), and is deleted right after. 10 min timeout,
  16 MiB output cap, env cleared except PATH/HOME/GHIDRA_INSTALL_DIR/JAVA_HOME. The parser rejects anything but a
  function whose body starts exactly at the requested entry; auto names (`FUN_`, `entry`, ...) are discarded.

## Decisions
- One-file-per-build, entries keyed by hash, `deny_unknown_fields` everywhere: anything unexpected is rejected rather than ignored.
- Validator rejects instead of trimming, because trimming could hide a leak. Thresholds: pattern <= 64 B, >= 8 pinned
  bytes, no printable-ASCII run > 8 pinned bytes, strings in an identifier alphabet, no hex run > 24, entropy <= 4.6 bits/char
  for strings >= 32, no paths or `@`, ranges <= 1 MiB, files <= 1 MiB / 5000 entries. Every repository load revalidates,
  so a hostile remote cache cannot inject data; symlinked files are never followed.
- Hexagonal: use cases depend on the `SignatureRepository` and `Reverser` traits only; process execution is isolated in `exec`.
- The remote read model is a git clone, network only on explicit `napple sigs sync`; reads and tests never touch it.
- Tool never assigns `high` confidence; `community` provenance is for reviewed upstream content.

## Real-data check (Link's Awakening v0, keys 22.1.0; module `main`, build ae16f71e...)
- `resolve` of a function start with REA: Ghidra 12.1.4 via REA returned the boundary (`0x1b8624..0x1b865c`), stored
  as `rea/medium` pending-share (13 s).
- Two more signatures recorded by hand-given size (local/low); an offset in generic mid-function code was correctly
  reported UNRESOLVED (no unique <= 64 byte pattern).
- `napple sigs match --game ... --module main`: 2 hits, 0 unknown, both inside their recorded range. `validate` OK.
  `share --dry-run` printed the preview, nothing sent. These artefacts were deleted afterwards (`work/` is git-ignored anyway).

## Risks / assumptions
- Even a masked 64-byte pattern is a slice of game code. It is policy-limited, not provably free of copyright; reviewers
  must still look at PRs. Lowering `MAX_PATTERN_BYTES` is a one-line change if counsel asks.
- The PR is opened by the GitHub account `gh` uses: commit metadata is neutral but the account is visible to GitHub.
- The live `--yes` path (fork, push, PR) is implemented but NOT exercised: `gh` is not authenticated and nothing may be
  pushed. Only the preview, plan, validation and a local-repo git clone (sync) are tested.
- REA only returns function boundaries from a known start; it cannot name stripped NSO functions, find service names or
  struct layouts. Those still need humans/other builds (`carried`). `pointer`/`struct` kinds cannot be derived
  automatically yet. No emulation-based verification (planned in ARCHITECTURE 6.2): only uniqueness + bounds checks.
- Pattern search is a linear scan (fine for 20 MB modules and tens of signatures; index if the DB grows).
- NSO modules other than main/rtld/sdk/subsdk* are not recognised; `--game` reads the first matching `--module`.
- Release build of napple-cli failed in this environment with a missing `libLLVM.dylib` (outside this crate; debug build and tests fine).

## Tests (`cargo test -p napple-sigs`: 40 pass, offline)
Masking/hash stability and bit-level matching; validator rejections (long pattern, wrong key, weak/ASCII pattern,
blobs, entropy, paths, unknown fields, ranges, ids, layouts, resolution params, size); store round-trip/merge/corrupt/
mismatch/symlink; layered precedence; git cache sync from a local `file://` repo; resolver pipeline (known, reverser,
fallback, carried, refused kinds); matcher hit/unknown/range; REA adapter with a fake `rea` script (parse, garbage,
exit code, timeout, outside-work, symlink, missing binary); process runner (no shell, env, timeout, output cap); share
plan/preview exactness, exclusion of invalid pending, empty plan, dry-run CLI leaves pending untouched; committed examples validate.
Recommended next: run `share --yes` against a throwaway fork with a throwaway account; fuzz the validator.
