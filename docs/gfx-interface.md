# gfx interface: what the runtime provides and calls (v1, draft published early)

Header: `gfx/include/napple_gfx.h` (pure C). Library: `gfx/libnapple_gfx.a` (`make -C gfx`). Design: `docs/gfx-design.md`.
Status of this document: the API is fixed enough to code against; details marked (TBD) can still change and will be
listed in `docs/changes/<date>-graphics.md`.

## 1. Lifecycle

```
runtime start
  napple_gfx_config_default(&cfg)      fill: metal_layer (or NULL headless), title_id, cache_dir, hooks, settings
  napple_gfx_init(&cfg)                creates MTLDevice/queue + gfx threads; 0 = ok
  napple_gfx_hle_table(&n)             register each {symbol, fn} as a host override of the guest import of that name
guest runs: nnMain -> nn::gfx -> nv::SetGraphicsAllocator / nv::InitializeGraphics / nvnBootstrapLoader("nvnDeviceGetProcAddress") ...
every guest present -> hooks.on_present(frame, swap_interval) -> runtime signals vsync event at napple_gfx_vsync_period_ns()
exit: napple_gfx_diag_dump(); napple_gfx_shutdown()
```

## 2. Symbols the runtime must redirect

`napple_gfx_hle_table()` returns the authoritative list. At the time of writing:

| guest symbol | meaning |
|---|---|
| `nvnBootstrapLoader` | returns the NVN entry points; the SDK's own NVN driver is never executed |
| (not overridden) `nv::InitializeGraphics`, `nv::SetGraphicsAllocator`, `nv::SetGraphicsDevtoolsAllocator` | run natively; overriding the allocators crashes SMO, and InitializeGraphics needs the runtime's nvdrv (see docs/runtime-services.md) |

Every function pointer gfx returns to the guest goes through `hooks.wrap_guest_callable(user, fn, name)`
(use it to run NVN calls on the guest thread's host stack, as the reference does with `runtime_hle_wrap_named`).
The guest calls NVN entry points with the plain AAPCS64 ABI; there are no stack arguments, up to 8 integer and 8 FP
register arguments. Boolean (`NVNboolean`) parameters are not extended by Switch callers: gfx masks to 8 bits.

## 3. Guest memory: what gfx needs from the runtime

1. **Identity mapping.** A GPU address passed by the game is a host virtual address. NVN memory pools wrap game memory
   (`nvnMemoryPoolInitialize(storage, size)`); gfx wraps the same pages in a zero-copy `MTLBuffer`
   (`newBufferWithBytesNoCopy`), so the pages must be page-aligned `mmap` memory owned by the process (16 KiB pages on
   Apple Silicon; the guest sees 4 KiB granularity, gfx rounds outward to 16 KiB).
2. **Virtual memory pools** (`NVN_MEMORY_POOL_FLAGS_VIRTUAL`, `nvnMemoryPoolMapVirtual`): gfx remaps physical pool pages
   (`mach_vm_remap`) so the virtual address range must be reserved by the guest/runtime as normal anonymous memory.
3. **Write-fault notifications (dirty tracking).** The reference marks pool pages that hold textures read-only after
   uploading them to the GPU; a guest write faults, the page is marked dirty and unprotected. Therefore:
   - the runtime's fault handler (Mach exception server or SIGSEGV/SIGBUS handler) must call
     `napple_gfx_handle_fault(addr, is_write, ucontext)` first for every access fault inside guest memory; on `1` it
     retries the instruction;
   - the runtime must call `napple_gfx_guest_write_prepare(addr, len)` before any *host-side* write into guest memory
     that a syscall performs on behalf of the guest (file reads, IPC out-buffers, decoders): a kernel `read()` into a
     protected page returns `EFAULT` instead of faulting through the handler;
   - the runtime must not itself `mprotect` ranges that were handed to nvn memory pools.
4. **No JIT / no x18 assumptions in gfx.** gfx code is plain host code. It never touches guest registers.

## 4. Threading

- The guest's graphics thread(s) call NVN entry points directly. gfx implements them on that thread, records, and may
  hand work to a private translation/replay thread (`submit` returns before the GPU work is built).
- Blocking entries use `hooks.core_away`/`core_back`.
- gfx never calls back into guest code, except through addresses the game passed itself (debug callbacks, allocators):
  `nv::SetGraphicsAllocator` callbacks are stored and may be invoked from the guest thread that is inside the NVN
  call. The runtime does not need to do anything for these.

## 5. Presentation, resolution and frame pacing

- `metal_layer`: a `CAMetalLayer*`. gfx sets pixel format, `framebufferOnly`, `displaySyncEnabled`, drawable size. NULL
  selects headless mode (offscreen texture, same code path, used by tests and `napple run --headless`).
- `settings.output_width/height`: custom output size; `internal_res_scale`: render-target scale for scene-sized targets
  (window-derived textures are scaled, fixed-size targets such as shadow maps are not); `upscaler`: none/bilinear/MetalFX.
- Pacing: the runtime owns the guest-visible vsync event. It must fire it every `napple_gfx_vsync_period_ns()`, **not**
  every game swap interval. gfx decides when to actually show frames: `NAPPLE_PACING_GAME` paces to
  `swap_interval/60 s`, `NAPPLE_PACING_FIXED` to `target_fps`. Speed-up artifacts happen when game logic is stepped by
  vsync counts: for a game that requests 30 Hz (swap interval 2), `game_fps_divisor_fix=1` keeps the guest's logical
  step at 2 vsyncs while gfx may present interpolated/duplicated frames at the higher output rate. (Games that step logic
  from the Horizon tick instead of vsync need no fix; see docs/gfx-design.md section "frame pacing".)
- `napple_gfx_screenshot(path)`: PNG of the next presented frame, for headless verification.

## 6. Diagnostics

`napple_gfx_diag_unimplemented()` returns distinct unimplemented NVN entry points that the guest called, with counts;
each is logged once by name at WARN as it happens. `settings.unimplemented_abort=1` aborts on the first one (CI).
`napple_gfx_diag_unknown_lookups()` lists names asked of `nvnDeviceGetProcAddress` that this build does not know.
`napple_gfx_get_stats()` gives frames, draws, pipelines compiled, shader cache hits, pending async compiles.

## 7. Integration status

Pending: runtime/ has no loader yet (empty directory when this was written). When it does, the glue is: init, apply
`napple_gfx_hle_table()` to the module import resolver, forward fault handler and write-prepare, drive vsync from
`on_present` and `napple_gfx_vsync_period_ns()`.

## 8. Updates since first publication

- `napple_gfx_config.shader_translator` is reserved (NULL): translation is in-process (see docs/gfx-design.md); no helper process, no .NET.
- `napple_gfx_settings.present_interval_override` added (0 = honour the game). The guest vsync event is virtual: fire it every
  `napple_gfx_vsync_period_ns()` (1/60 s by default), not at the monitor refresh.
- NVN API version: the game checks `nvnDeviceGetInteger(API_MAJOR/MINOR)` against the version it was built with, so the runtime must put the
  game's version in `nvn_major/nvn_minor` (per-game data; 0,0 = 55.15, right for Link's Awakening).
- Shader global memory (LDG/STG, used by SSBO access) is addressed through one buffer over the guest heap: a `gmem` region
  (base, size) will be added to the config once the runtime fixes its heap layout (TBD; until then SSBO-heavy compute shaders are skipped).
- Status: API layer (device, queue, sync, events, pools, buffers, textures, samplers, programs, state objects, command recording,
  windows) is written; the Metal replay layer is in progress. See docs/changes/2026-10-21-graphics.md.

## 9. Integration (done on the graphics side, verified up to the guest's first nvdrv call)

- `make -C gfx lib adapter` builds `gfx/libnapple_gfx.a` and `gfx/build/obj/adapter/gfx_napple.o`, which implements runtime/src/gfx.h on top of
  napple_gfx.h (hooks mapped: `core_away` token, `wrap_guest_callable`, `on_present`).
- `make -C runtime GFX=napple` links them instead of `gfx_stub.c` (the only change made under runtime/: a conditional block in its Makefile).
- Tried on Link's Awakening (`runtime/build/napple-runtime work/games/01006BB00C6F0000/<build> --headless`): gfx initializes
  (`Metal device`, `NVN: 510 of 523 entry points implemented`), the guest then aborts earlier than NVN with
  `IPC service "nvdrv" not implemented` (caller sdk+0x3d4314). That is a runtime service (nvdrv/nvmemp/nvgem stubs, or overriding the nn::vi native
  window path) and is the next blocker for graphics bring-up; gfx cannot be exercised by a real guest until it exists.
- Reaching NVN with a real game also needs: `nvn_major/nvn_minor` per game, the heap window (`gmem_base/gmem_size`) if SSBO compute is used, and the
  runtime's fault handler forwarding to `gfx_handle_fault` (runtime/src/signals.c already has the hook point).

## 10. Integration findings (2026-10-22)
- The adapter builds its override table lazily: the runtime registers overrides before `gfx_init`.
- Window origin: "lower-left" (the NVN default) changes only window coordinates (viewport, scissor, fragment coordinate y = H - y_top via the rscale table); NDC +y renders to the top row in both modes. The former vertex-shader Y negation produced upside-down images in real games and was removed.
- `CopyTextureToTexture` with a different size or format is done by a draw (format conversion and scaling); depth/MSAA/3D variants are still not implemented.
- Command handles outlive their command buffer (graveyard of 1024 recordings), required by Super Mario Odyssey.
- Headless runs pace presents at the swap interval so the guest runs at real speed.
- New host API: `napple_gfx_texture_info`, `napple_gfx_texture_write_rgba` (video decoders).
- Per-game: `nvn_major/nvn_minor` from `game.json`.
