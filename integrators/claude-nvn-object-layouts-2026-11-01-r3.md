---
branch: claude/nvn-object-layouts-2026-11-01-r3
orchestration: nvn-object-layouts
base: 774a6ec
head: 87dee72 (code at aee2856; the integrator-file commit follows)
env: mac
date: 2026-11-01
areas: [gfx]
status: ready
---

# claude/nvn-object-layouts-2026-11-01-r3: guest-visible NVN objects within the smallest size the titles give them

## Summary
- **Minimum sizes:** `gfx/src/obj.h` has `NVN_MIN_<type>` for all 30 guest-visible NVN object types: the smallest size measured across SMO 3.5.1, MK8D 0.12.16 (32-bit), LAS 8.2.0 and ACNH 20.5.4, i.e. the minimum of nvn.h's reserved size and the smallest credible neighbour gap.
  - `FITS(T, H)` now asserts `sizeof(T) <= NVN_MIN_H <= sizeof(H)`.
  - The evidence table and method are in `docs/architecture/gfx.md` section 14 (acceptance 1).
- **Program objects:** ACNH's NVNprogram is at most 0x80 bytes. A live program, magic intact, has an NVNbuffer at +0x80 and a memory pool at +0xb0, and r2's layout still zeroed +0x80..0x88, the start of that buffer.
  - `prog_t` is now 0x28 bytes (magic, nstage, hash[0..3]). The never-read `NVNshaderData sd[6]` is removed.
  - All stage hashes are kept host-side by program address, as in r2. The object mirrors stages 0..3 for programs copied by value.
- **Every other layout already fit** its measured minimum. No other side table was needed (acceptance 2).
- **Measurement tool:** `NAPPLE_GFX_OBJTRACE=1` (`gfx/src/api_objtrace.c`, with hooks in each Initialize, Finalize and SetDefaults) measures object sizes in a running title. It is off by default.
- **Canary test:** `gfx/tests/t_obj_canary.c` places a canary after `NVN_MIN` bytes and checks that it survives SetDefaults or Initialize plus all 328 entry points of the 30 families, Finalize last (acceptance 3).
  - The call lists come from `gfx/tests/t_nvn_canary_gen.py`.
  - With the minimums halved, the test fails on the expected calls, so it does catch overruns.

## Files
`git diff --stat 774a6ec..HEAD`:
- `gfx/src/obj.h`, `gfx/src/api_{cmd,device,memory,program,state,texture,window}.c`
- new `gfx/src/api_objtrace.c`
- `gfx/tests/t_nvn_ext_prog.c`, new `gfx/tests/t_obj_canary.c` and `gfx/tests/t_nvn_canary_gen.py`
- `docs/architecture/gfx.md` (one appended section, 14), `docs/archive/changes/2026-11-01-claude-nvn-object-layouts-2026-11-01-r3.md`, this file

`gfx/Makefile` is untouched: the wildcard picks up the new source and test. No `mtl_*`, `shader/` or `runtime/` changes, and no field that `mtl_*` reads moved (`tex_t.s`, `win_t.*`, `sync_t`, `view_t` are unchanged). Prototypes are unchanged, so the a32 table needs no regeneration.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Acc. 1: table of every guest-visible struct vs the smallest size in the four titles, with method | verified | `docs/architecture/gfx.md` section 14. Traced 90 s boots: `NappleWork/logs/objlay-{acnh,mk8d,las}-trace2`, `objlay-smo-trace3` (`grep objtrace run.log`) |
| Acc. 2: every host layout fits its minimum; host-only state moved | verified | `FITS` against `NVN_MIN_*` compiles for all 30. `prog_t` 0x98 -> 0x28 bytes (hashes host-side). No other layout needed moving |
| Acc. 3: `t_obj_canary` | tested | 30 families, 328 calls, canary intact. Halved-minimum sanity copy fails on QueueBuilder, Buffer, TextureBuilder, View, Texture, SamplerBuilder, Sampler, pools, ... |
| Acc. 4: `make -C gfx test` | tested | ALL TESTS PASSED. The SKIPs are pre-existing KNOWN items (t_clear_ds, t_clear_tex, t_feedback) |
| Acc. 4: four-title boots, ACNH 90 s | verified | `objlay-{acnh,mk8d,smo,las}-final`, 90 s each, untraced: exit 0, 0 skipped draws, 0 unimplemented calls, no warning absent from r2's runs (`gfxnvn-r2-*`). No "program hash" or "unknown name" line. Screenshots normal |

## Unbuilt / untested list
- None unbuilt.
- The `objtrace` path has run only with `NAPPLE_GFX_OBJTRACE=1` in the traced boots; it is not exercised by tests.

## Findings
- **ACNH program size.** The r2 fix assumed a neighbour at +0x88 at the latest; it is at +0x80. The r2 layout zeroed 8 bytes of the following NVNbuffer (its magic and pad) whenever a program was initialized, or SetShaders ran, after that buffer: a latent corruption. It is fixed by the 0x28-byte `prog_t`.
- **Exact fits at nvn.h's size.** No slack, but every reading is at or above that size:
  - `samp_t` 0x30 (titles show at most 0x78);
  - `dpool_t` 0x20 (three titles put the sampler pool exactly 0x20 after the texture pool);
  - queue and window builders 0x40, `view_t` 0x10, the state objects.
- **MK8D textures.** MK8D runs as a 32-bit title, so its stack temporaries cannot be recognised. It sets up about 219,000 textures in 90 s, and its small texture gaps are dead temporaries. `NVN_MIN_NVNtexture` (0x80) rests on ACNH (0x88 to a pool, 22,498 times), SMO and LAS (0xe0), and nvn.h.
  - Our `tex_t` is 0x58, so a 32-bit title would need an object under 0x58 to be affected; none is known.
- **Builders, views and state objects.** They have no lifetime, and their "recent objects" pairing mixes stack frames, so only array strides count as evidence: vertex attrib 4, vertex stream 8, color, channel mask and polygon 4. The other builder minimums are nvn.h's, and nothing contradicts them.
- **One SMO crash, outside gfx.** One traced SMO run (`objlay-smo-trace2`) crashed at 0.84 s inside the title's SDK (SIGBUS, fault on uncommitted guest memory, pc sdk+0x203bcc). The traced rerun, an untraced 60 s run and the final 90 s run were clean, so it does not reproduce. It may be worth a look by the memory / kernel owners.

## How to integrate
- No ordering constraints.
- Conflict hotspots: one-token `OBJTRACE(...)` insertions at the start of the Initialize, Finalize and SetDefaults lines of `api_*.c`. If another branch rewrote one of those lines, keep its body and re-add the hook (or drop it; the hook is diagnostics only). `obj.h`: the `FITS` macro and `prog_t` line.
- Build: `make -C gfx -j4 test`, then `make -C gfx lib adapter && make -C runtime`.

## How to test
1. `make -C gfx -j4 test` -> ALL TESTS PASSED; `t_obj_canary: ok`, `t_nvn_ext_prog: ok`.
2. ACNH, MK8D, SMO and LAS headless 90 s: exit 0, 0 skipped draws, and `grep -c 'program hash' run.log` -> 0.
3. Optional evidence refresh: `NAPPLE_GFX_OBJTRACE=1` on a boot. `grep 'objtrace close' run.log` lists any neighbour inside nvn.h's reserved size. A gap smaller than our host layout for a credible object (live, off the stack, repeated) means a layout must shrink.

## Fix policy hints
- **`t_obj_canary` fails on a new entry point:** a real overrun; move the field host-side, the way `api_program.c` does with its table. A crash in the test on a new entry point's dummy arguments: adjust its argument mapping in `t_nvn_canary_gen.py` and regenerate.
- **New title evidence of a smaller object:** lower `NVN_MIN_*` (the `FITS` assert then names the layout to shrink) and add the evidence to gfx.md section 14.

## Needs from other areas
- None for layouts.
- Optional, for the runtime/a32 owner: a hook giving gfx the 32-bit guest stack range would let `objtrace` recognise MK8D's temporaries.
