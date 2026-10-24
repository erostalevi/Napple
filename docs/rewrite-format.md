# Rewriter output format (`napple rewrite`)

Produced by `crates/napple-rewrite`. Conventions (key page, slots, gate call) follow the reference
rewriter (`patch_nso.py`, MIT) so a loader written for that format can consume the output. The
runtime author should read this file; all offsets are module-relative (module base = 0 = start of `.text`).

## Files per module (`main`, `rtld`, `sdk`, `subsdk*`)

`napple rewrite <game> -k prod.keys -o OUT` writes `OUT/<module>.{patched.bin,fixups.tsv,gates.tsv,sites.tsv}`
for the base game; each selected update goes to `OUT/update-v<version>/`. Extra files appear only when
non-empty: `<module>.unhandled.tsv` (sites the rewriter could not handle: the module must not be run),
`<module>.warnings.txt`. The reference's `cuts.tsv`, `obs.tsv`, `params.tsv` (game-specific tables) are not produced.

## `<module>.patched.bin`

```
[0, text_end)                      code, with every site replaced by `b <block in patch area>`
[text_end, image_len)              rodata, data, bss exactly as the NSO maps them (bss = zeros)
[key_page_off, key_page_off+0x4000) key page, zero in the file; the loader fills it
[.., patch_off)                    zero
[patch_off, end)                   patch area (rewriter code), 64 KiB aligned start
```
The loader maps the whole file at the module base (bss and the rest zero-filled), applies the module's
original relocations (RELA/RELR/JMPREL; the rewriter changes none and moves no code), and makes
text + patch executable. Branches text -> patch must be within +-128 MiB (checked at rewrite time).

## `<module>.fixups.tsv`

Header-only file of `# NAME<TAB>value` lines: `PATCH_OFF`, `KEY_PAGE_OFF`, `KEY_PAGE_SIZE` (0x4000),
`TEXT_END`, `RO_OFF`, `DATA_OFF` (hex), and `KEY_<name>_IDX` (decimal) for `K18`=0, `KTLS`=1, `KTLR`=2,
`KGATE`=3, and reserved `KRES0`=4, `KRES1`=5, `KASPECT`=6, `KASPECTUI`=7 (unused by the generic rewriter).

## Key page and thread slots

Entry `KEY_x_IDX` of the key page (8-byte little-endian words at `key_page_off + 8*idx`) must hold
`slot_number * 8`, where `slot_number` is a per-thread slot (e.g. a pthread TSD key) the loader allocated.
Rewritten code computes the slot address as `tpidrro_el0 + key_page[idx]`. Slots per guest thread:

| key | slot holds |
|---|---|
| K18 | virtual `x18` (initial value 0 or the guest's choice) |
| KTLS | virtual `tpidr_el0` (guest thread pointer) |
| KTLR | virtual `tpidrro_el0` (Horizon ThreadLocalRegion pointer) |
| KGATE | address of the host gate function |

Assumption inherited from the reference: the low bits of Apple's `tpidrro_el0` are zero (it is the TSD
base). If hardware shows otherwise, the loader must compensate; the rewriter does not mask.

## Host gate (svc and counters)

`svc #n`, `mrs xN, cntpct_el0|cntvct_el0|cntfrq_el0` become a block that
`sub sp,sp,#144; stp x29,x30,[sp,#-16]!`, loads the gate address from KGATE into x30 (x29 is restored to the
guest value before the call), `blr x30`, then `ldp x29,x30,[sp],#16; add sp,sp,#144; b site+4`.
The host looks the call up by its **return address** (x30 on entry = module offset of the `ldp`) in `gates.tsv`:

`<ret_off><TAB><kind><TAB><arg><TAB><orig_off>` with `kind` in `svc|cntpct|cntvct|cntfrq`; `arg` = svc immediate
(decimal) or destination register number. All guest registers are live in the registers at entry
(x0-x30 except that x30 holds the return address and x29 the guest x29 with the pair saved on the stack at
`[sp]`); the host writes results into x0-x7 (svc) or the destination register (counters, never x18/x29/x30:
those sites are reported unhandled). Guest sp seen by the host is 160 bytes lower than at the site.
`cntfrq` is an addition to the reference (Switch 19.2 MHz vs Apple 24 MHz).

## `<module>.sites.tsv`

Tab separated, one row per rewritten site: `addr<TAB>label` (labels `x18 svc tpidr_r tpidr_w tpidrro_r cntpct
cntvct cntfrq`), `lo<TAB>W<TAB>hi` exclusive window `[lo,hi)` (head word at `lo` branches to the copy),
`addr<TAB>C<TAB>ret` `blr x18` call, `addr<TAB>J<TAB>base<TAB>count` `br x18` jump table (targets `base+4*i`,
`i<count`; an additional `x18` row for the same address exists). Diagnostic; the runtime does not need it.

## What the rewriter does (summary)

* x18 read/write: spill two scratch regs at `[sp,#-144]`, load slot, run the instruction with x18 -> scratch, store back, restore.
  `-144` is just below the 128-byte red zone (clang/AArch64 leaf functions keep live locals there; Link's Awakening main does).
* `cbz/tbz x18`, `adr/adrp x18`, `ldr x18,<literal>`, `blr x18` (x16/x17 are dead into a call; no spill), `br x18` (landing pads, shared per table), ldxr/stxr windows copied whole.
* `mrs/msr tpidr_el0`, `mrs tpidrro_el0`: slot access; `svc`/counters: host gate.
* **Runtime contract**: signal handlers for guest threads must run on an alternate stack (`sigaltstack`); the
  rewritten code treats `[sp-160, sp-128)` as scratch and a signal frame there would corrupt it.

Failure policy: a site with no safe template is left untouched and listed in `unhandled.tsv`; the CLI exits non-zero.
