# Round 3: LAS crash on leaving Marin's house, graphics backlog, SMO

Status: in progress (see "Not done"). Everything below was verified by headless scripted runs unless stated otherwise.

## 1. Objective
1. Fix the instant crash when the owner left Marin's house in Link's Awakening (LAS): `CRASH signal=10 (SIGBUS) ... esr=0x9200004f` = guest write to a read-only page.
2. Finish the graphics backlog: translator failures, draws skipped while pipelines compile, SMO black/corrupt 3D, controller applet, diagnosable unattended sessions.

## 2. TASK 1: the crash (root cause and fix)
### Evidence
- Fault page lay inside a 16 KiB host page that gfx had write-protected (`prot=0x701`, a region of 0xdc000 bytes read-only). The log shows the game creating hundreds of tiny memory pools
  (`MemoryPool ... mem=0x7a63231000 size=0x1000 flags=0x61`): 4 KiB aligned, 4 KiB - few MiB, one per texture. Host pages are 16 KiB, so several pools share every host page
  with each other and with unrelated heap data.
- Old `gfx/src/dirty.c` kept the protection state **per pool range** (a state array per range, `find()` returning the first live range that contains the address). Two ranges covering the same
  host page disagreed: pool A had protected the page (state PROT), the fault lookup found pool B first (state NONE) and treated the fault as "already resolved, retry"; after a few retries it
  declined and the runtime reported a guest crash. Other defects of the same design: removing one pool unprotected pages another pool still needed, a protect racing a pool removal could leave a page
  read-only with no live range (nobody would ever unprotect it), and range slots were reused while a fault handler could still hold the old state arrays.
- This is the "page-granularity mismatch 4 KiB guest vs 16 KiB host" hypothesis, combined with a protect-vs-remove race. Leaving the house finalizes the pools of the room (mass finalize), which
  is exactly when the window opens.

### Fix (gfx/src/dirty.c rewritten, same API plus two helpers)
- One global page table keyed by host page (16 KiB): `{gen, state, refs}` per page, chunks of 1024 pages allocated on demand in a lock-free directory (fault path: atomics + mprotect only).
- `refs` = number of registered pool ranges covering the page (`dirty_add_range(mem, size)` / `dirty_remove_range(mem, size)`; the remove now takes the size and must match the add). A page is protected only while `refs > 0`
  and the last `dirty_remove_range` makes it writable (waiting for a concurrent protect: the protect re-checks `refs` after claiming the page, a Dekker-style handshake with seq_cst atomics).
- State machine NONE -> BUSY -> PROT claimed by CAS, so two threads never run mprotect on one page concurrently.
- Fault handler: unprotects a PROT page (generation bumped first, so an uploader's stamp is always older); a page that is already writable (another thread unprotected it between the fault and the handler) is retried;
  a page that keeps faulting with `refs > 0` is forced writable instead of crashing (logged with `NAPPLE_GFX_DIRTY_DIAG=1`); a page never registered is "not ours" (real crash path unchanged).
- `dirty_invalidate()` after `mach_vm_remap` / `mmap` in `nvnMemoryPoolMapVirtual` (the replaced pages are writable; neighbours sharing a boundary page lose their protection state).
- `api_memory.c`: `dirty_add_range` only when the Metal pool was created, remove under the same condition (balanced refs).
- Diagnostics: `NAPPLE_GFX_DIRTY_DIAG=1` logs add/remove/forced-writable; `dirty_describe(addr)` returns the page state (for crash reports).

### Regression test
`gfx/tests/t_dirty.c` (in `make -C gfx test`): deterministic cases (three 4 KiB pools sharing one host page, removal order, overlapping ranges, host-write prepare) plus a multi-threaded stress (4 writer threads
on a real SIGSEGV/SIGBUS handler, 2 uploader threads doing protect-then-read and checking that a clean `dirty_since` never hides a write, 2 threads adding/removing 4 KiB-aligned pools): millions of writes, ~10^5 faults resolved, 0 declined.

### Verification in the game
Scripted headless run from the owner's save (copy in `work/g3/saveroot`): title -> Autosave "Marin and Tarin's House" -> dialogue -> walk south out of the house -> village (screenshots `work/g3/r3-*.png`,
contact sheet `work/g3/sheet3.png`): no crash across the house -> village transition. (The old binary could not be re-run to reproduce the crash because the code was replaced in place; the unit test encodes the failing pattern.)

## 3. Other changes
The round ended before this section was written. The items below were reconstructed afterwards (cloud session, 2026-10-23) from the code; they
were **not** re-verified in a game run, and the evidence paths of the original round are unknown.

- Translator (`gfx/src/shader/maxwell_translate.c`), the three LAS failures listed in round 2:
  - HSETP2/HSET2 immediate (`0x7e00`/`0x7c00`) and constant-buffer (`0x7e80`/`0x7c80`) forms (bezel_pbr ps; layout checked against those shaders per the code comment).
  - Indexed attribute load (AL2P + IPA with the index flag, decal_uber ps): fragment varyings mirrored in a local array.
  - Bindless TEX.B handles read from user uniform buffers (`bindless_index`, slots `8192 + n`), not only the c2 texture table (water_uber ps).
- Draws no longer skipped while a pipeline compiles (`gfx/src/mtl_shader.m`): the first use builds synchronously and a draw waits for a pipeline
  being built elsewhere. Every pipeline key is persisted to `<cache>/pipelines.bin` and rebuilt in the background once its programs are registered
  ("prewarm"); cached MSL is compiled once at start-up. `NAPPLE_GFX_PIPE_SKIP=1` restores the old skip behaviour, `NAPPLE_GFX_NO_PREWARM` disables prewarm.
- Controller support applet (id 0xc, `runtime/src/services/applet.c`): answered offline as a user confirmation with one virtual pad.

## 4. Files
- `gfx/src/dirty.c` (rewritten), `gfx/src/api_memory.c` (balanced add/remove), `gfx/tests/t_dirty.c` (new): task 1.
- `gfx/src/shader/maxwell_translate.c`, `gfx/src/mtl_shader.m`, `runtime/src/services/applet.c`: section 3.

## Not done
- **SMO black/corrupt 3D**: no root cause recorded and no code tied to it; the compat matrix still says video partial (glyphs missing, ~45 fps).
- **Translator failures**: the three LAS forms above have code but no recorded corpus result; re-run `t_xlate` on the LAS and SMO corpora and confirm 0 failures.
- **Unattended sessions** ("diagnosable unattended sessions" objective): nothing recorded.
- **Still unimplemented from round 2**: `OP_DRAW_TEXTURE` (ignored in `mtl_replay.m`), transform feedback, multi-draw-indirect-count,
  separate texture/sampler binding; render scale, settings, 60 fps.
- **Compat matrix** not refreshed since 2026-10-21: run `napple compat` for both games after the above.
- Found after the round (cloud session, see `2026-10-23-cloud-session.md`): the pipeline wait added in section 3 can hang when prewarm fills the GCD
  worker pool (proposed fix committed, untested), and the controller applet read the player counts from the wrong storage (proposed fix committed, untested).
