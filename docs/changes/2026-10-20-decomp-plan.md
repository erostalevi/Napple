# Decomp stage: build plan (base/update/DLC), NSO loader, rewrite-site scanner

## Objective
`napple plan` / `napple decomp`: take any mix of base game, update and DLC packs, show what can go into the
final build (versions and DLC are selectable at recomp time), then load every module of each selected version
and report the A64 sites that must be rewritten for macOS/iOS.

## Files
- `infrastructure/cnmt.rs`: CNMT parser (Application / Patch / AddOnContent / Delta, content records)
- `infrastructure/nso.rs`: NSO0 loader (LZ4 segments, SHA-256 segment hashes, build id)
- `analysis/a64.rs`: scanner for `svc`, TPIDR_EL0/TPIDRRO_EL0 (MRS/MSR), CNTVCT/CNTPCT/CNTFRQ, other system
  registers, and x18/w18 operands (yaxpeax-arm decode after a cheap register-field prefilter)
- `application/package.rs`: `scan` (one Pack per content meta), `BuildPlan` (base, updates sorted by version, DLC,
  warnings for duplicates/foreign packs/missing base), selection (`--updates all|latest|none|v,v`, `--dlc all|none|id,id`)
- `application/decomp.rs`: per-version module analysis, JSON report (counts, ids; no game bytes)
- `application/ingest.rs`: BKTR RomFS no longer aborts ingest (`romfs_note`)
- `napple-cli`: `plan` and `decomp` commands

## Decisions
- The plan lists every version/DLC with size so the recomp step (and later Studio) can choose what to embed;
  the runtime toggle/picker (goal 12) needs all chosen layers embedded.
- Reports never contain game bytes; `work/` is git-ignored.

## Validated on real data (Link's Awakening v0 NSP, keys 22.1.0)
CNMT parsed (Application, v0, 6.27 GB). All 5 NSO modules decompressed with segment hashes verified; build ids
read; sites found: main 483 TPIDR + 4858 x18; sdk 61 svc, 56 TPIDRRO, 32 CNTPCT, 11228 x18; whole run ~0.2 s.

## Risks / pending
- Scanner counts are not yet a rewrite: x18 counting is textual on the decoded instruction, and words in data
  inside text (literal pools) can give false hits. Per-site verification (execute original vs rewritten) is milestone 2.
- Updates and DLC are only synthetic-tested (grouping, CNMT). BKTR RomFS patching, update ExeFS and DLC mounting
  need real packs (milestone 1b); NSZ not implemented.
- Not implemented yet: rewriter, runtime, Studio UI, signature DB, REA integration.

## Tests
`cargo test`: 18 passing (new: NSO plain/LZ4/tamper/garbage, CNMT kinds + hostile tables, plan grouping/sorting/
filtering/duplicates, A64 encodings). Real-data runs: `napple plan` and `napple decomp` on the NSP above.
