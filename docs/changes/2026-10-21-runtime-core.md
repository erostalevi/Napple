# Runtime core: game-agnostic native runtime (2026-10-21)

## Objective
A runtime (`runtime/`, C/ObjC/AArch64 asm, `make -C runtime` -> `runtime/build/napple-runtime`) that loads any rewritten Switch
game described by a data directory (module tables read from files at run time), implements the Horizon kernel surface and the IPC
service framework, and boots the guest up to graphics device initialization, behind a replaceable gfx interface (`runtime/src/gfx.h`).
First targets: Link's Awakening (01006BB00C6F0000) and Super Mario Odyssey (0100000000010000).

## Files
- `runtime/src`: `main.c` (config, guest start), `module.c` (tables, mapping, relocation, host overrides, module permission state),
  `dispatch.c` + `gate.S` (svc/counter gates), `hle_stack.c`/`hle_thunks.S` (host-stack thunks, per-import trap stubs),
  `thread.c`/`thread_enter.S` (per-thread TSD slots, TLR), `threads.c` (thread/mutex/condvar/arbiter/events, virtual cores),
  `kernel.c` (handle table, svc dispatch, GetInfo, Break...), `memory.c` (guest address space, heap, alias, MapMemory),
  `ipc.c/.h` (HIPC/CMIF with domains, `sm:`, service registry), `npdm.c`, `signals.c`, `log.c` (log, JSON-lines trace, compat summary),
  `json.c`, `frontend.m/.h` (Cocoa window + CAMetalLayer), `gfx.h`, `gfx_stub.c`, `gfx_glue.c`, `services/*.c`.
- `crates/napple-build`: `napple build --dump-exefs` and `--dump-romfs` (PROGRESS lines per docs/studio-protocol.md).
- `tools/`: `prepare_game.sh`, `prepare_tables.py`, `boot_test.sh`, `make_empty_romfs.py`, `refpy/` (reference rewriter, local jump-table change).
- `THIRD_PARTY_NOTICES.md`, `docs/runtime-services.md`, `docs/runtime-modules.md`.

## Logic and decisions
- Services at IPC level, not SDK-symbol level: independent of SDK version; unknown services/commands fail loudly (summary + trace).
- Tables file (NTB1) instead of generated headers; load order main, subsdk*, sdk; imports bind to host overrides, then module exports,
  else a per-import trap that names the symbol when called (zero unresolved on both games).
- Guest memory below 2^39 in one reservation sized from the biggest free hole (macOS 27 leaves 7-22 GiB), alias region shrinks from 64 GiB.
- MapMemory keeps copy semantics (source may be heap, alias, module data or the thread stack, as the SDK probes with a module page).
- Domains are shared between cloned sessions (the SDK converts then clones).
- Service access follows the NPDM list; refused services return an error so the SDK falls back.
- Compat output: `runtime-boot.json` per docs/compat-format.md with unsupported svcs, missing/stubbed/implemented IPC, missing NVN, crash.

## Flow
```
napple-runtime <data-dir> -> manifest + npdm -> reserve guest space -> map+relocate modules -> gate index -> services
 -> boot thread: nn::init::Start -> svc/IPC (host) ... -> nvnBootstrapLoader -> gfx back end (stub ends run at first NVN call)
```

## Risks / assumptions
- Virtual cores follow Horizon rules but without preemption timeslices; games that spin without svc may stall.
- `GetInfo`, resource limits, applet command ids come from public documentation; each is exercised only as games reach it.
- The reference rewriter is a stopgap; jump-table bound scanning is heuristic (entries validated against the function).
- Environment: the sandbox storage went dataless/low-disk during this work (see report); romfs.bin could not be produced for either game.

## Tests
Boot runs (headless) of both games reach service initialization and `fsp-srv` RomFS mount; see `tools/boot_test.sh` and
`work/logs`. Recommended: unit tests for the IPC framing (domain in/out), handle table staleness, thread primitives.
