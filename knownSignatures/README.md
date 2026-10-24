# knownSignatures

Per-build knowledge that lets Napple rewrite a game without re-analysing it: where a function, pointer, struct,
patch site or service hook is, and what the rewriter/runtime should do with it.

```
knownSignatures/<titleId>/<buildId>.json     one file per binary build id of a title (NSO header build id)
knownSignatures/schema.json                  JSON Schema (documentation; the Rust validator is authoritative)
knownSignatures/_examples/                   synthetic examples (ignored by lookups, validated by the tests)
```

## What is allowed in here

Only addresses, **short masked instruction patterns (max 64 bytes)**, names, hashes and struct layouts.
Never game bytes beyond that, no strings, no dumps, no paths, no personal data, no keys.
`napple sigs validate <file>` REJECTS (it does not trim) files that:
- have a pattern longer than 64 bytes, fewer than 8 fully pinned bytes, bits set under the wildcard mask, or a
  run of printable ASCII (string data);
- have a key that is not the pattern hash;
- carry unknown fields, text outside an identifier character set, long hex runs, high-entropy strings, paths
  or e-mail-like text, or struct layouts/offset ranges out of bounds;
- are larger than 1 MiB or have more than 5000 entries.

## Entry fields

| field | meaning |
|---|---|
| key | pattern hash: first 16 bytes (hex) of sha256(`napple-sig/1\0` \|\| u32le(len) \|\| pattern \|\| mask) |
| kind | `function`, `pointer`, `struct`, `patch-site`, `service-hook` |
| module | `main`, `sdk`, `subsdk0`, ... |
| offset_start / offset_end | range relative to the module image base (start inclusive, end exclusive) |
| pattern / mask | the shortest masked pattern that is unique in the module. Relocatable operands (B/BL targets, ADR/ADRP, literal loads, conditional/compare/test branches, and the imm12 that follows an ADRP) are wildcarded so the same pattern survives rebuilds |
| name | symbol name if known (optional) |
| layout | struct size and fields (required for `struct`) |
| resolution | `annotate`, `hook` (`params.target`), `stub` (`params.value`), `rewrite` (`params.recipe`), `skip` |
| provenance | `local` (made on a user's machine), `rea` (found by REA/Ghidra), `community` (reviewed in this repo) |
| confidence | `low`, `medium`, `high`; the tool never assigns `high` automatically |

## Using and contributing

```
napple sigs lookup <titleId> <buildId>      what is known for a build
napple sigs match  <module> --title ID      hits / unknowns of a module against the known signatures
napple sigs resolve ...                      miss -> REA -> validate -> stored locally as pending-share
napple sigs pending                          what is waiting to be shared
napple sigs share --dry-run                  show EXACTLY what would be sent
napple sigs share --yes                      fork + pull request (neutral commit identity); default is local only
```
Contributions arrive as pull requests; reviewers should still check each file for accidental game content.
