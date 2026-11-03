# 2026-10-26 nx-rewriter: rewriter correctness and size picks from the nxnative review (R1, R2, R4, R6)

Env: Mac. Branch `claude/nx-rewriter-2026-10-26-nx`. Statuses: *tested* = unit/equivalence test passes, *verified* = measured on real game data here.

## R1 jump tables (`crates/napple-rewrite/src/jumptab.rs`)
- **Halfword tables padded to the largest entry** (`Shape::AdrHalfTable`): `ldrh` tables used to get 65 536 landing pads each. Now the table is read (adrp+add base, `cmp/b.hi` bound) and padded to max entry + 1; unreadable tables keep 65 536. *Tested* (Unicorn equivalence, entries {0,1,2,5} -> 6 pads) and *verified* on ACNH.
- **adr-relative offset tables** (`Shape::AdrRelative`: `adr x18,L; ldrsw E,[T,I,lsl #2]; add x18,x18,E; br x18`, target = L + entry). *Tested*.
- Shared helper `table_base_and_bound` / `targets_extent` now also back the existing signed-word shape (behaviour unchanged; LAS/SMO/MK8D-update outputs byte-identical).
- Not added (no known title needs them; nxnative documents them): copy-before-check index, far table base over 256 instructions, base moved through registers, byte tables far from the `br`. The function-span fallback still covers compiler switches.

**ACNH 3.0.3 (build ff1d1c05670db602, main text 66 MB), patch area, same machine:**

| module | before | after |
|---|---|---|
| main | 42 285 136 B | 7 893 256 B |
| sdk | 1 440 588 B | 669 744 B |
| subsdk0 | 407 476 B | 407 476 B |
Rewrite wall time 9.1 s -> 8.4 s for base+update (all modules, structural check on), RSS 3.8 -> 2.8 GB. Matches nxnative's 42 -> 7.9 MB. LAS, SMO and the MK8D 64-bit update produce byte-identical outputs before/after (`diff -r`). The ACNH images changed (smaller pads) and passed `check::verify`; booting them is NOT verified here (ac-render owns ACNH runtime).

## R2 exclusive windows (`windows.rs`, `check.rs`)
- **adrp inside a window** is now re-encoded for the copy's address (same target page, destination register substituted when it is x18); it was rejected as "cannot be relocated". Structural checker accepts exactly that. *Tested* with Unicorn equivalence.
- Already correct, now covered by tests: fall-through exit emitted first (`exclusive_windows`), out-of-line retry block re-entering the stxr (`out_of_line_retry_block_reenters_the_window`, equivalence), unconditional branch into a window refused (`branch_into_the_middle_of_a_window_is_refused`), a window never spans a second exclusive load (`find`), bl inside a window refused.
- Caveat: the equivalence harness replaces exclusives with plain loads/stores, so the monitor semantics themselves (job-system hang) are not exercised by Unicorn; the exit order is proven structurally and by successful-store path equivalence.

## R4 per-build workarounds (findings only)
nxnative keys cuts by NSO build id and by function fingerprints (`MATCH_CUTS`: unique unwind function by length + SHA-256 of a normalised body, callee bodies checked one level deep, relocation addresses normalised, original bytes re-verified, ambiguity = no cut). Our rule stays: per-game knowledge is data-only in `knownSignatures`; the rewriter has no build-id switch and none was added. If a title ever needs a cut, enrol it as a signature record with the same fingerprint discipline (ambiguous or changed code = red diagnostic, no patch). Not implemented, by design.

## R6 survey (`crates/napple-rewrite/src/survey.rs`)
`napple rewrite --survey <files> -k keys [--updates ...]` (no `-o`; `--survey-exact` also runs the rewriter for the exact patch size). Prints per program: NPDM facts (name, 32/64-bit, address space, system resource, stack, priority/core, ACID flags, SVC and service counts), per module A32/A64, build id, segment sizes, site kinds, relocation kinds, patch estimate; exits 1 with a list of blocking reasons (no base, no main, no main.npdm, unreadable NSO/layout, unsupported relocation kind, unhandled sites in exact mode). AArch32 modules (ELF32 dynamic section: DT_REL and no DT_RELA) are reported, not failed. `napple rewrite` itself now SKIPS AArch32 modules with a clear message instead of reporting ~300 bogus unhandled sites on MK8D's base. Estimate is 40 B per site (excludes jump pads; ACNH main is ~151 B/site because of them). Outputs for the four titles: see the notes folder (survey-*.txt). Survey of ACNH base+3.0.3 took 8.6 s.

Titles: LAS main 21 MB text 4 858 x18 + 483 tpidr sites; SMO and ACNH likewise in the files; MK8D base = 8 modules AArch32 (NPDM 32-bit, address_space 2), MK8D update 1.7.0 = AArch64 (address_space 3).

## Not done / unverified
- The full `napple build --data-only --update-romfs in-place` comparison of build times was not run (the rewrite stage is the only thing changed; its before/after is above via `napple rewrite`). Previous per-title numbers: LAS/SMO/MK8D-update patch sizes identical.
- LAS/SMO 45 s headless boots not run (rewriter output byte-identical, runtime untouched). ACNH boot with the smaller pads unverified.
- `make -C runtime test`, `make -C gfx test` not run (no runtime/gfx paths touched). `cargo test --workspace` green.
