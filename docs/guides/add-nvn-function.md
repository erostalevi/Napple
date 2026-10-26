# Add an NVN API function

## Goal
Implement an NVN entry point that a game calls and `libnapple_gfx` does not yet answer. Today such a call logs
`[napple-gfx] warn: unimplemented NVN entry point called: <name>` once, returns 0 and goes on (or aborts when `settings.unimplemented_abort` is set), and the
count appears in the `unimplemented calls` field of the `[napple-gfx] stats:` line and in the `diag:` lines printed at exit.
Background: [gfx.md](../architecture/gfx.md) (object model, interface, threading) and [subsystems.md](../reference/subsystems.md#graphics-nvn-to-metal).

## How an NVN function is found and called
- The guest never runs the SDK's NVN driver. It imports `nvnBootstrapLoader`, which the runtime overrides with the host function `hle_bootstrap` from
  `napple_gfx_hle_table()` (`gfx/src/core.c`; the adapter `gfx/adapter/gfx_napple.c: gfx_hle_table` registers it as a host symbol). The bootstrap loader and
  `nvnDeviceGetProcAddress` (`gfx/src/api_device.c`) both end in `nvn_lookup(name)`.
- `nvn_lookup` (`core.c`) hashes the name against `nvn_names[]`, which is **generated at build time from `gfx/src/nvnapi/names.txt`** by `gfx/tools/gen_nvn_tables.sh`
  (one line per API name; the same script writes one assembly stub per name, `nvn_unimpl_thunk[]`, that calls `nvn_unimpl_report` and returns 0).
- Implementations register themselves: the `NVN(name, ret, args...)` macro in `gfx/src/internal.h` defines a static function `h_<name>` and puts a
  `{name, fn}` record in the Mach-O section `__DATA,napple_nvn`. At first lookup `names_init` reads that section and fills `g_impl[]`. A name with an
  implementation returns it; otherwise the lookup returns the unimplemented stub. The result is wrapped for the guest by `gfx_wrap_callable`.
- The `NVN` macro also checks the signature at compile time: `static const NVNT_<name> chk_<name> = h_<name>;` needs a typedef `NVNT_<name>` from
  `gfx/src/nvnapi/nvn_protos.h`. For entry points newer than the bundled header the typedefs are hand-maintained in `gfx/src/nvnapi/nvn_protos_ext.h` (included at the end of `nvn_protos.h`).
  `NVN_NOPROTO` exists in `internal.h` for an entry without any prototype (no function uses it today). `NVNboolean` parameters are `uint32_t` in the typedefs.

## Files to touch
| Path | What |
|---|---|
| `gfx/src/nvnapi/names.txt` | The name must be in this list, or `names_init` logs `NVN implementation <name> is not in the name table` and ignores it. (523 names today; `g_impl[]` and the counters in `core.c` hold 1024.) |
| `gfx/src/nvnapi/nvn_protos.h` / `nvn_protos_ext.h` | The `NVNT_<name>` function-pointer typedef. Add to `nvn_protos_ext.h` by hand. |
| `gfx/src/nvnapi/nvn.h` | Types and enums of the NVN API (read-only reference for argument types). |
| `gfx/src/api_device.c` | Device, queue, sync, events, `nvnDeviceGetProcAddress`, `nvnDeviceGetInteger`. |
| `gfx/src/api_memory.c` | Memory pools, buffers. |
| `gfx/src/api_texture.c` | Textures, samplers, pools, builders. |
| `gfx/src/api_program.c` | Programs and shaders (`nvnProgramSetShaders`). |
| `gfx/src/api_state.c` | State objects (blend, color, depth/stencil, polygon, multisample, vertex): setters and getters on guest-sized structs. |
| `gfx/src/api_cmd.c` | Command buffers: **recording** of every `nvnCommandBuffer*` call, recordings, handles. |
| `gfx/src/api_window.c` | Windows, presentation. |
| `gfx/src/obj.h` | Opcodes `OP_*`, payload structs `p_*`, the state structs (`blend_st`...) with `FITS(T, NVNxxx)` size assertions, `cmdrec`. |
| `gfx/src/mtl_replay.m` | Replay of recorded commands onto Metal: `exec_op` and the `do_*` helpers. |
| `gfx/tests/` | `t_*.c` / `t_*.m` programs and the shared helper `nvn_test.h`. |

## Steps
1. **Decide the kind of function.**
   - *Object getter or setter* (state, builder, sampler): operates on the guest's struct and records nothing. Put it in the matching `api_*.c`, modelled on
     `api_state.c: nvnBlendStateSetBlendTarget` / `nvnBlendStateGetBlendTarget` (`ST(type, ptr)` / `CST(type, ptr)` cast the guest object to the packed struct from `obj.h`).
     If you add fields, keep `FITS(struct, NVNxxx)` true: the guest reserved a fixed size.
   - *Immediate call* (device, queue, memory pool, texture builder, sync): does its work on the calling thread; Metal objects are created behind it (`api_device.c`, `api_memory.c`, `api_texture.c`).
   - *Command-buffer call* (`nvnCommandBuffer...`): **records**, never touches Metal. The work happens later in the replay thread.
   - *Accepted and ignored*: write `{ (void)c; ... }` and log once with `LOG_ONCE(NAPPLE_LOG_WARN, ...)` if a game could depend on it (`api_cmd.c: nvnCommandBufferSetViewportSwizzles` is such a no-op).
2. **Add the name** to `names.txt` and, if it has no typedef yet, the typedef to `nvn_protos_ext.h` (pattern: `typedef void (*NVNT_nvnCommandBufferSetColorReductionEnable)(NVNcommandBuffer *c, uint32_t e);`).
3. **Write the handler** with the `NVN(...)` macro. Mask booleans with `B(x)` (`internal.h`): Switch callers do not zero-extend `NVNboolean`, only the low 8 bits are valid.
4. **For a recorded command:**
   1. Add an opcode before `OP_COUNT` in the enum in `obj.h` and, if it carries data, a `p_<name>` payload struct. Payloads hold raw values, addresses and handles only,
      never pointers into guest-owned objects (recordings outlive their command buffer).
   2. Record it in `api_cmd.c`: `REC(c, p_type, OP_X)` for a fixed payload, `REC_N(c, p_type, OP_X, extra_bytes)` for a payload followed by an array, `rec_op(c, OP_X, n)` for raw bytes.
      `rec_op` is static in `api_cmd.c`, so recorded calls live there. Outside `BeginRecording`/`EndRecording`, or on a null/finalized buffer, `rec_op` returns a scratch area and warns once, so a stray call is harmless.
   3. Replay it: add a `case OP_X:` to the `switch (h->op)` in `exec_op` (`mtl_replay.m`). State setters update the replay state `S` (viewports in `S.vp`, scissors `S.sc`, blend `S.blend[...]`...) and set
      `g_pk_dirty = 1` when the pipeline key changes; draws, clears and copies call a `do_*` function (`do_draw_arrays`, `do_clear_color`, `do_copy_b2b`). Anything that ends a render pass calls `pass_end()` first.
5. **Test it** (below), then update the graphics notes if the change alters behaviour games can see.

## Worked example
A recorded command with an array payload, modelled on `SetViewports` (record) and `OP_VIEWPORTS` (replay):

```c
/* gfx/src/api_cmd.c */
NVN(nvnCommandBufferSetViewports, void, NVNcommandBuffer *c, int first, int n, const float *v) {
    if (n < 0) n = 0; if (n > 16) n = 16;
    p_range *p = REC_N(c, p_range, OP_VIEWPORTS, (size_t)n * 16);   /* p_range { u32 first, n } then n x 4 floats */
    p->first = (uint32_t)first; p->n = (uint32_t)n;
    memcpy(p + 1, v, (size_t)n * 16);
}
/* gfx/src/mtl_replay.m, in exec_op() */
case OP_VIEWPORTS: { const p_range *r = (const p_range *)pl; const float *f = (const float *)(r + 1);
    for (uint32_t i = 0; i < r->n && r->first + i < 16; i++) memcpy(S.vp[r->first + i], f + 4 * i, 16); break; }
```
A scalar record (`api_cmd.c: nvnCommandBufferSetPrimitiveRestart` writes `{enable, index}` into `rec_op(c, OP_PRIM_RESTART, 8)`; replay `case OP_PRIM_RESTART` sets
`S.prim_restart_en` and `S.prim_restart_idx`) and a draw (`nvnCommandBufferDrawArrays` fills a `p_draw_arrays`; replay calls `do_draw_arrays`) are next to them. A
state object round trip is `api_state.c: nvnBlendStateSetBlendTarget` plus `nvnBlendStateGetBlendTarget`, bound by `api_cmd.c: nvnCommandBufferBindBlendState` (copies the `blend_st` into an `OP_BIND_BLEND` record).
`nvnCommandBufferSetViewports` is already in `names.txt`; a new function needs its own line there.

## How to test
- **Linux, no Metal:** `sh gfx/tests/t_getters_linux.sh` compiles `tests/t_getters_linux.c`, which `#include`s `src/api_state.c` and `src/api_texture.c` directly and calls the static `h_nvn*` handlers
  (set then get round trips; the script defines `NVN_IMPL_SECTION` so an ELF host accepts the section and leaves unresolved symbols alone). Measured on a Linux box: `33 checks, 0 failed`.
  This is the right test for any new state/builder getter or setter. Only those two API files are built this way; the repository has no Linux build of the other `api_*.c` files (a recorded command cannot be replayed without Metal).
- **macOS:** `make -C gfx test` builds `libnapple_gfx.a` and every `tests/t_*.c` / `t_*.m` (the Makefile takes them by wildcard) and runs them with `MTL_DEBUG_LAYER=1`; `make -C gfx tests` only builds them;
  `make -C gfx lib adapter` builds the library and the runtime adapter. A new test is just a new file `tests/t_<name>.c`.
- **Shape of a Metal test** (`tests/t_nvnapi.c`, `t_state.c`): `napple_gfx_config_default(&cfg); cfg.headless_width = 32; cfg.headless_height = 32; napple_gfx_init(&cfg);`, then call entry points through
  `L(nvnName)(...)` (the macro in `tests/nvn_test.h` looks them up with `napple_gfx_nvn_bootstrap_loader`, so a missing name or a missing `names.txt` line shows up as a NULL call), record into a command buffer,
  `nvnQueueSubmitCommands`, `nvnQueueFinish`, copy a texture to a buffer in a pool and compare pixels. `CHECK(cond, ...)` counts failures. `nvn_test.h` also has a tiny Maxwell assembler (`ins_*`) and `build_shader` so no game data is needed.
- **Prove the call is reached by name:** in a test, `L(nvnYourFunction)` must be non-NULL and not the unimplemented stub; the init log prints `NVN: <n> of <total> entry points implemented`, and after a run
  `napple_gfx_diag_unimplemented()` (`diag:` lines at exit) must not list your name.
- **Games:** run a title that calls it. Useful env (all exist in `gfx/src`, full list in [environment-variables.md](../reference/environment-variables.md)): `NAPPLE_NVNTRACE=1` prints the op number of each recorded command
  (`[nvntrace] cb <ptr> op <n>`), `NAPPLE_GFX_CMDTRACE=1` logs recording lifetimes, `NAPPLE_GFX_LOG=<level>` sets the gfx log level, `NAPPLE_DRAWLOG*` and `NAPPLE_SCREENSHOT` show what was drawn ([debugging.md](debugging.md)).

## Common pitfalls
- **Not in `names.txt`.** The handler compiles, the section entry exists, and the lookup still never returns it (or logs `not in the name table`). Add the line.
- **Missing or wrong typedef.** The compile fails with an incompatible pointer on `chk_<name>`: that is the signature check doing its job; fix the typedef in `nvn_protos_ext.h`, not the cast.
- **Unextended booleans** (`B(x)`): a game passing a dirty upper half turned conditional state on or off at random.
- **Command handles outlive command buffers.** A game can `EndRecording`, finalize the buffer, hand its memory to another one and keep calling the handle (Super Mario Odyssey, Mario Kart 8 Deluxe).
  Recordings are host copies kept in a graveyard under a byte budget (`NAPPLE_REC_BUDGET_MB`, default 1024); releasing them by idle time caused exploded geometry
  ([symptoms.md](../symptoms.md), Rendering). Never store a pointer to guest memory in a payload that replay reads later without checking it.
- **Command buffers are touched by several threads.** The recording lists are under one lock (`g_recs_mu`); null and finalized buffers must be tolerated (`cbh_get`/`CBH`), as a crash seen one run in three in
  Animal Crossing showed ([symptoms.md](../symptoms.md), Boot, crashes).
- **Guest garbage.** Games pass odd values once (a pool-relative shader address `0xe00` in `nvnProgramSetShaders`). Validate and reject instead of dereferencing.
- **Replay state is per-`exec_op`.** Setting `S.*` without `g_pk_dirty = 1` when the value feeds the pipeline key leaves a stale pipeline; ending the pass is needed before copies that cannot happen inside a render pass.
- **Window origin and depth mode change coordinates.** Viewport, scissor and fragment coordinate follow `nvnDeviceSetWindowOriginMode`; do not negate Y in the vertex shader ([gfx.md](../architecture/gfx.md), section 10).
- **Memory.** Writing into guest memory from the host needs `guest_write_prepare` (gfx dirty tracking write-protects pages that hold textures); a plain `memcpy` into such a page faults or hits `EFAULT`.
- **Version check.** A game compares `nvnDeviceGetInteger(API_MAJOR/MINOR)` with its build constant; it comes from `game.json` (`nvn_major`/`nvn_minor`) or is detected in `main.c: detect_nvn_version`. A mismatch silently skips graphics start-up.
