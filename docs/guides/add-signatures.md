# Add per-build signatures (`knownSignatures/`)

## Goal
Record, for one build of one game, where a function, pointer, struct, patch site or service hook is, as a *masked instruction pattern* that survives a rebuild, so the next person does not have to find it again.
Signatures are data files checked by a strict validator that rejects anything that looks like game content. Background: `knownSignatures/README.md`, `knownSignatures/schema.json`, and the design note
[docs/archive/changes/2026-10-20-signatures.md](../archive/changes/2026-10-20-signatures.md).

Status of consumers: in this tree only the `napple sigs` command reads `knownSignatures/` (`grep -rn knownSignatures crates runtime` finds `napple-sigs` and a comment in `napple-rewrite/src/survey.rs` saying per-game knowledge stays data-only).
The rewriter and the runtime do not consume them yet; the files are records and inputs for tools.

## What must never be committed
Game bytes beyond a short masked pattern: no dumps, no strings, no file paths, no keys, no firmware, no personal data, no NSO/NSP/XCI, no decompiler output. A pattern is at most 64 bytes with relocatable operands masked out.
The validator **rejects, it does not trim** (so that trimming cannot hide a leak): pattern longer than 64 bytes, fewer than 8 pinned (compared) bytes, bits set under the wildcard mask, a run of more than 8 printable ASCII bytes among the pinned bytes,
a key that is not the pattern hash, unknown fields, text outside an identifier character set, hex runs of more than 24 characters, high-entropy strings of 32 characters or more, paths or e-mail-like text,
struct layouts or ranges out of bounds, files over 1 MiB or with more than 5000 entries (`crates/napple-sigs/src/validate.rs`: `MAX_PATTERN_BYTES` 64 in `domain.rs`, `MAX_FILE_BYTES`, `MAX_ENTRIES`, `MAX_RANGE`, `MAX_ASCII_RUN`, `MAX_HEX_RUN`, `ENTROPY_LIMIT`).
Even a masked 64-byte pattern is a slice of game code: it is policy-limited, not provably free of copyright, so reviewers still read every pull request (design note, "Risks").

## Where the files live and how a build id keys them
```
knownSignatures/<TITLEID>/<BUILDID>.json      one file per binary build of a title
knownSignatures/schema.json                   JSON Schema (documentation; the Rust validator is authoritative and stricter)
knownSignatures/_examples/                    synthetic examples: ignored by lookups, validated by a unit test
```
- `TITLEID`: 16 upper-case hex digits (`TitleId::parse` upper-cases). Use the application (base) title id.
- `BUILDID`: the NSO header build id (the 32-byte ModuleId at offset 0x40 of the NSO) as lower-case hex, **16 to 64 hex digits** (`BuildId::parse`; `Nso::build_id_hex` gives all 64). Only the first 16 digits are the
  `<buildid16>` directory name of a runtime data directory; the signature file should use the full id you have, because the path is `<TITLEID>/<BUILDID>.json` and the file's `build_id` and `title_id` fields must equal the path
  (`DirRepository::load` refuses a file whose ids do not match). `napple compat show <titleid>` prints the build ids the matrix knows.
- Inside a file each signature is keyed by its **pattern hash**: lower-case hex of the first 16 bytes of `sha256("napple-sig/1\0" || u32le(len) || pattern || mask)` (32 hex characters). Because relocatable operands are masked, the same code in
  another build hashes to the same key, which is how a signature from build A is *carried* to build B.

## Entry format (one value of `signatures`)
Required: `kind` (`function`, `pointer`, `struct`, `patch-site`, `service-hook`), `module` (`main`, `sdk`, `subsdk0`, ...), `offset_start` (inclusive) and `offset_end` (exclusive) relative to the module image base (range non-empty, at most 0x100000, end below 2^32),
`pattern` and `mask` (hex, same length, 1 to 64 bytes; the pattern has the masked bits zeroed; mask 1 bits are compared), `resolution` (`action` = `annotate`, `hook`, `stub`, `rewrite` or `skip`; `hook` needs `params.target`, `stub` needs `params.value`,
`rewrite` needs `params.recipe`; at most 8 params of at most 64 characters), `provenance` (`local`, `rea`, `community`), `confidence` (`low`, `medium`, `high`; the tool never assigns `high` itself).
Optional: `name` (identifier characters, at most 200), and `layout` (`size` plus `fields[{name, offset, size, type}]`, required for `kind: struct`, every field inside the struct size).
File header: `{"schema_version": 1, "title_id": ..., "build_id": ..., "signatures": {...}}`. Which operands are wildcarded: B/BL targets, ADR/ADRP, literal loads, conditional/compare/test branches and the imm12 that follows an ADRP
(`Pattern::from_a64` in `domain.rs`).

## Files to touch
| Path | What |
|---|---|
| `knownSignatures/<TITLEID>/<BUILDID>.json` | The new data file (create the directory). |
| `crates/napple-sigs/src/validate.rs`, `domain.rs`, `resolver.rs`, `repo.rs`, `share.rs`, `cli.rs`, `reverser.rs`, `exec.rs` | Only when you change the format or the tool, not when adding data. A format change also needs `knownSignatures/schema.json`, `knownSignatures/README.md` and the example in `_examples/`. |
| `knownSignatures/_examples/0100FFFF00000000/` | Synthetic example file: do not add real titles here. |

## Steps (adding data for a build)
1. **Work locally.** `napple sigs` is local-only by default: nothing leaves the machine except `sync` (a read-only clone of the public repository into `work/sigcache`) and `share --yes`.
   State lives under `--work` (default `./work`, git-ignored): `work/sigs/pending`, `work/sigs/shared`, `work/sigcache`.
2. **See what is known.** `napple sigs lookup <TITLEID> <BUILDID>` lists the signatures from the layers `pending`, `shared`, `knownSignatures/` (`--known`) and the remote cache, and names other builds of the title when this one is unknown.
3. **Match against the module.** `napple sigs match <module.nso|patched.bin> --title <TITLEID> [--build <id>] [--name main]`, or straight from the container
   `napple sigs match --game <xci|nsp> -k <prod.keys> [--module main]`: prints `hit` lines (with `AMBIGUOUS` or `(outside recorded range)` flags), `unknown` lines (signature not found in this module) and counts.
4. **Resolve new items.** `napple sigs resolve <module> --title <TITLEID> [--build <id>] [--no-rea] OFFSET[:KIND[:NAME[:SIZE]]]...`
   (or `--game <file> -k <prod.keys> --module main` instead of the module path). For each offset the tool derives the shortest masked pattern (12 to 64 bytes, at least 8 pinned) that occurs exactly once in the module text,
   then reports one of: `known` (already in this build), `carried` (found in another build of the title: new local entry, confidence capped at medium), `learned` (the optional Reverser, REA/Ghidra, returned the function bounds:
   provenance `rea`, confidence `medium`), `local` (no reverser result; you gave a SIZE; confidence `low`), or `UNRESOLVED` (for example no unique pattern of at most 64 bytes exists there). Every new entry is validated and written to `work/sigs/pending`.
   The REA/Ghidra setup is in [tooling.md](../reference/tooling.md); without it use `--no-rea` and give sizes.
5. **Review.** `napple sigs pending` lists them. Open the JSON in `work/sigs/pending/<TITLEID>/<BUILDID>.json` and read it as a reviewer would: names, patterns, nothing else.
6. **Validate.** `napple sigs validate <file.json>` prints `OK: <title>/<build> with N signature(s)` or one `REJECTED:` line per problem (non-zero exit).
7. **Contribute.** Either copy the file into `knownSignatures/<TITLEID>/<BUILDID>.json` and open a pull request, or let the tool do it: `napple sigs share --dry-run` prints the exact files, the branch name,
   the neutral commit identity (`Napple Signatures <signatures@napple.invalid>`) and the planned `gh`/`git` commands; `napple sigs share --yes` forks, commits and opens the pull request, then moves the entries to `work/sigs/shared`.
   The pull request is opened by the GitHub account `gh` is logged in with; GitHub knows that account. (The design note records that the live `--yes` path was implemented but not exercised against a real fork.)

## Worked example
Run on a Linux box for this guide with a **synthetic** 256-byte file of made-up instruction words (not game code), so the output can be reproduced without any game:
```
$ napple sigs resolve synth.bin --title 0100FFFF00000000 --build 00112233445566778899aabbccddeeff --no-rea --work w2 0x40:function:synthetic_fn:0x20
0x40  local         571808a981f154bea6060bb591943274  (low confidence; no reverser configured; stored as pending-share)
nothing was sent anywhere. Review with `napple sigs pending`; share with `napple sigs share`.
$ napple sigs pending --work w2
0100FFFF00000000/00112233445566778899aabbccddeeff: 1 signature(s)
571808a981f154bea6060bb591943274  Function     synth    0x40..0x60  Local/Low  synthetic_fn
$ napple sigs validate w2/sigs/pending/0100FFFF00000000/00112233445566778899aabbccddeeff.json
OK: 0100FFFF00000000/00112233445566778899aabbccddeeff with 1 signature(s)
$ napple sigs match synth.bin --title 0100FFFF00000000 --build 00112233445566778899aabbccddeeff --work w2
hit      571808a981f154bea6060bb591943274  Function     0x40  synthetic_fn
1 hit(s), 0 unknown, 0 for other modules
```
(`synth` is the module name taken from the file name; pass `--name main` for a raw `main` image.) The committed model of every kind is
`knownSignatures/_examples/0100FFFF00000000/00112233445566778899aabbccddeeff00112233.json`: a `struct` with a `layout` and `annotate`, a `service-hook` with `resolution {action: hook, params: {target: hle_nn_os_get_tick}}` and a mask that wildcards a `BL`,
and a `patch-site` in module `sdk` from provenance `rea`. `napple sigs lookup 0100FFFF00000000 00112233445566778899aabbccddeeff00112233 --known knownSignatures/_examples` prints its three entries.

## How to test
- `cargo test -p napple-sigs` (offline, no game, runs on Linux: 40 tests in this tree) covers hash stability, validator rejections, store round trips, layered precedence, the resolver, the REA adapter against a fake `rea`, the share plan and
  that the committed `_examples` validate (`validate.rs: committed_examples_validate`). `cargo test --workspace --locked` is what CI runs on macOS (`.github/workflows/tests.yml`, job `rust`).
- **Real files under `knownSignatures/<TITLEID>/` are not covered by a CI test** (the unit test reads only `_examples`; repository loads revalidate at run time). Before a pull request run
  `napple sigs validate` on every file you add, and `napple sigs lookup <TITLEID> <BUILDID>` from the repository root to see that the layered store loads it (a file with a wrong path or mismatched ids is reported as an error).
- If you change the format: update `schema.json`, the README table, the `_examples` file and the tests together.

## Common pitfalls
- **Wrong key.** Editing a pattern or mask by hand changes its hash; the validator rejects "key is not the pattern hash". Let the tool write the file.
- **Weak or string-like patterns.** Fewer than 8 pinned bytes, or a run of printable ASCII, is rejected (it would identify nothing or leak a string). Choose a different start offset or accept `UNRESOLVED`.
- **Ambiguity.** `match` flags `AMBIGUOUS` when the pattern occurs more than once; `resolve` only accepts a pattern that is unique in the module text.
- **Wrong build id.** The build id is the NSO ModuleId, which differs between the base game and each update; the update's `main` has its own id. Do not copy the 16-digit data-directory name as if it were the whole id.
- **Pointer and struct kinds** cannot be derived automatically yet; they need a hand-written entry with the right kind and (for structs) a layout (design note, "Risks").
- **Names.** REA/Ghidra cannot name stripped NSO functions; automatic names (`FUN_...`, `entry`) are discarded. Names must come from a human or from another build's signature.
- **Paths and secrets in `--work`.** The work directory may hold REA input slices (`work/rea-in`, deleted right after) and a clone for sharing (`work/sigshare`); keep `work/` out of git (it is ignored) and never copy files from it other than the reviewed JSON.
- **Confidence.** Do not edit a file to set `confidence: high` unless a reviewer has verified it; the tool never does.
