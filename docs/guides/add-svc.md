# Add or extend a Horizon SVC

## Goal
Make a Horizon supervisor call (svc) that the guest issues answer correctly in the runtime, instead of stopping it with
`unimplemented svc 0x<nn> (<Name>) at <module>+0x<offset>`, or extend what an existing svc supports (a `GetInfo` id, a new mode of `WaitForAddress`...).
Background: the svc list and its status are in [runtime-services.md](../architecture/runtime-services.md) ("Kernel (svc)" and the 2026-10-24 additions);
the gate that carries the svc into the host is in [rewrite-format.md](../architecture/rewrite-format.md#host-gate-svc-and-counters).

## How an svc reaches your code
1. `napple build` rewrites every `svc #n` in the guest's A64 modules into a *gate* (a call into the runtime). The runtime looks the gate up by its return address
   (`runtime_dispatch` in `runtime/src/dispatch.c`) and calls `svc_dispatch(num, regs, module, offset)` (`runtime/src/kernel.c`). No per-svc table is needed
   in the rewriter: any immediate becomes a gate.
2. `svc_dispatch` calls `threads_svc_enter` and `threads_checkpoint` (virtual cores, priority preemption), then the handler registered for `num`
   (`handlers[256]`, filled by `svc_register(num, "Name", fn)`), then `threads_svc_exit`.
3. A handler is `void fn(uint64_t *regs)`. `regs[0..8]` are x0..x8 in and out, in the raw **kernel** register convention (x0 holds the result code on return,
   additional outputs come back in x1...). `regs[29]`, `regs[30]` are fp and lr of the guest. Use `R_*` result codes from `runtime/src/napple.h`
   (`R_INVALID_HANDLE`, `R_INVALID_ADDRESS`, `R_NOT_IMPLEMENTED`...).
4. No handler for `num`: `summary_svc_unsupported` (listed in `unsupported_svcs` of the summary), a `svc_unsupported` trace event, the guest call
   stack, and `runtime_fail` (status 70). With `NAPPLE_SVC_LENIENT=1` the call returns `R_NOT_IMPLEMENTED` (0x4201) and the run goes on, to enumerate more gaps.
5. AArch32 (NTB2) titles enter through `runtime/src/a32/a32_proc.c`: `a32_svc_impl` converts the A32 registers to the same `regs[]` layout using the
   `svc_sigs[]` table and then calls the same `svc_dispatch`.

## Files to touch
| Path | What |
|---|---|
| `runtime/src/kernel.c` | Generic svcs (`svc_kernel_register`: process id, Break, GetInfo, resource limits, ...), the handle table, `known_names[256]` (names used in diagnostics), `svc_close_handle`. |
| `runtime/src/threads.c` (`svc_threads_register`) | Threads, events, mutex/condition variable, address arbiter, waits, sleep. |
| `runtime/src/memory.c` (`svc_memory_register`) | Heap, MapMemory, QueryMemory, physical memory. It also calls `svc_kcode_register`. |
| `runtime/src/ipc.c` (`svc_ipc_register`) | Sessions and IPC svcs, shared and transfer memory. It also calls `svc_ksession_register`. |
| `runtime/src/ksession.c`, `kmsg.c` | Guest-to-guest sessions and ports (0x20, 0x40-0x44, 0x70-0x72). |
| `runtime/src/kcode.c` | Code memory (0x4B, 0x4C): a small self-contained svc file. Model for a new file. |
| `runtime/src/a32/a32_proc.c` | `svc_sigs[]`: the C-prototype signature of each svc for AArch32 titles (see below). |
| `runtime/src/main.c` | Calls the `svc_*_register` functions in order at start-up (`svc_kernel_register(); svc_threads_register(); svc_memory_register(); svc_ipc_register();`). A new file needs its register function called from one of these. |
| `runtime/tests/host/test_*.c` and `runtime/Makefile` | Host test and its rule (see "How to test"). |
| `docs/architecture/runtime-services.md` | Update the svc list. |

## Steps
1. **Get the register layout right.** Take the number, the inputs and outputs from the public Horizon svc documentation, not from the libnx C wrapper: the handler sees
   the kernel layout. Existing handlers show it: `svc_get_thread_priority` in `threads.c` reads the handle from `r[1]` and returns the priority in `r[1]`;
   `svc_get_resource_limit_value` in `kernel.c` reads the resource id from `r[2]` and returns the value in `r[1]`.
2. **Write the handler** next to related ones, or in a new file modelled on `kcode.c` (pure argument checks, then a call into the object code).
   - Validate like the kernel and return the matching `R_*` code; handles come from `handle_get(h, OBJ_xxx)`, `handle_new(type, obj)` (`kernel.c`), and
     `HANDLE_CURRENT_THREAD` / `HANDLE_CURRENT_PROCESS` are the pseudo handles (`napple.h`).
   - Guest pointers go through `guest_ptr(addr, size)`; a NULL result means "not guest memory" (`R_INVALID_POINTER`).
   - If the call can block, give the virtual core up while waiting. Inside `threads.c` use its static `core_put` / `core_take` (see `svc_sleep_thread`);
     elsewhere use `thread_core_away(&token)` and `thread_core_back(&token)` (`napple.h`). Otherwise the three application cores can all be held by sleeping threads.
   - A new kernel object type: add it to `ObjType` in `napple.h` and a `*_close_object` hook that `svc_close_handle` calls (as `kcode_close_object` does).
3. **Register it:** `svc_register(0x.., "Name", fn)` in the register function of the file. Add the name to `known_names[]` in `kernel.c` if it is not there
   (it is only used for messages and `svc_name`).
4. **AArch32:** add `{0x.., "Name", "<sig>"}` to `svc_sigs[]` in `a32_proc.c`. The signature has one letter per C parameter: `I` 32-bit value, `P` pointer (the host
   handler gets the base added), `L` 64-bit value, `o` 32-bit out value, `O` 64-bit out value, `a` out address, `R`/`S` return a 32/64-bit value instead of a result.
   Without an entry the title stops with `AArch32 svc 0x.. has no register layout yet`.
5. **Test it** (below), update `runtime-services.md`, and re-run a title that issues the call.

## The svc mask in the NPDM
The kernel capability descriptors in `main.npdm` are parsed in `npdm_load` (`runtime/src/npdm.c`) into `g_npdm.svc_mask[6]` (192 bits) and `nsvc_allowed`; the start-up log prints
`<n> svcs allowed`. In the code read for this guide the mask is used in exactly one place: `GetInfo` id 26 (`IsSvcPermitted`, `kernel.c: svc_get_info`), which answers from it
(true when no NPDM was loaded). **`svc_dispatch` does not enforce it**: a game calling an svc outside its mask reaches the handler anyway. If you add an svc, nothing has to be
added to the mask; the cross-game table of allowed svcs in `COMPATIBILITY.md` comes from `napple compat run`.

## Worked example
Modelled on `runtime/src/kernel.c: svc_get_resource_limit_value` / `resource_limit` and its registration in `svc_kernel_register`. The svc, its number's use and its body here
are placeholders to show the shape (0x4A `SetUnsafeLimit` is in `known_names[]` but has no handler in the tree today); the real semantics must come from the public documentation.

```c
/* kernel.c, next to the other generic svcs */
static void svc_set_unsafe_limit(uint64_t *r) {            /* x0 = size in; x0 = result out */
    if (r[0] & 0xfff) { r[0] = R_INVALID_SIZE; return; }   /* validate like the kernel */
    r[0] = 0;                                              /* success */
}
/* in svc_kernel_register(): */
svc_register(0x4A, "SetUnsafeLimit", svc_set_unsafe_limit);
```
A handler with a handle argument, an error code and a new object in one place: `runtime/src/kcode.c: svc_create_code_memory` (`r[0] = kcode_create(r[1], r[2], &h); r[1] = h;`) and
`svc_control_code_memory`; its register function `svc_kcode_register` is called from `memory.c: svc_memory_register`; its close hook is `kcode_close_object`.
An existing extension of an svc: the `GetInfo` cases in `kernel.c: svc_get_info` (add a `case` with the value; an unknown id logs `GetInfo id N ... not implemented`,
adds a summary note and returns `R_INVALID_ENUM`).

## How to test
Host tests run the real handlers on any thread through `svc_dispatch`; they are in `runtime/tests/host/`:
- `test_svc_async.c` is the smallest model: it calls `svc_kernel_register(); svc_threads_register();`, adopts the thread with `host_thread_attach(tlr)`, and calls
  `svc_dispatch(num, r, "test", 0x40)` with `uint64_t r[9]`, checking `r[0]` and outputs; a stand-in handler can be registered with `svc_register` to stub the neighbour.
- `test_kernel_ipc.c: test_code_memory` shows argument-validation checks for 0x4B/0x4C using `host_code` (`tests/host/host_stubs.h`); `test_kernel_sched.c` shows tests that use real host threads.
- Build rules live in `runtime/Makefile` (`HOST_TESTS`, one rule per test, and `HOST_TESTS += $(HOST_BUILD)/test_kernel_sched` as the pattern for a new test). Add a rule like
  `$(HOST_BUILD)/test_x: tests/host/test_x.c $(HOST_KERNEL) $(HOST_HDRS)` and append it to `HOST_TESTS`.
- Run: `make -C runtime test-host` (the Makefile calls it portable to "any POSIX host"). **Measured on a Linux box for this guide: it does not build there**, because
  `runtime/src/threads.c` (part of `HOST_KERNEL`) uses Mach APIs (`mach_port_t`, `thread_suspend`...) in the `NAPPLE_PCPROF` code; only `test_ios_dylib` builds. Treat `test-host` as a macOS target
  (`tools/ci_build.sh` runs it on macOS; `.github/workflows/tests.yml` runs `make -C runtime test test-services` and does not include `test-host`).
- Then run a title with `NAPPLE_SVC_TRACE=1` or `--svc-trace` (logs `0x<nn> <Name> from <module>+0x<offset> x0..x3`, and the result), and, to list every missing svc in one run, `NAPPLE_SVC_LENIENT=1`
  ([debugging.md](debugging.md)). For an AArch32 title `NAPPLE_A32_SVCTRACE=1` prints the A32 registers and result of each svc.

## Common pitfalls
- **Wrong register convention.** Writing the handler from the libnx prototype instead of the kernel ABI puts outputs in the wrong registers. Compare with a neighbour of the same shape in `kernel.c` or `threads.c`.
- **Cooperative cores.** Preemption only happens at svc entry (`core_checkpoint`); a handler that blocks without releasing its core starves other threads, and a guest thread that spins without any svc is never
  time-sliced ([runtime-services.md](../architecture/runtime-services.md), "Kernel scheduling note").
- **Pausing and context.** Several svcs are `partial` on purpose (0x32, 0x33, 0x36...): read their paragraph in `runtime-services.md` before changing semantics; the status words there (*implemented*, *partial*) must stay true.
- **Unknown values must not guess silently.** Unknown `GetInfo` ids and unsupported svcs are reported (summary note, `unsupported_svcs`) so the compatibility record says what a title needed. Keep that policy.
- **Address-arbiter results are checked by games.** AC needed `SignalToAddress` type 2 to write `value-2` and `WaitForAddress` type 3 ([titles/01006F8002326000.md](../titles/01006F8002326000.md) and `docs/archive/changes/2026-10-25-claude-ac-render-2026-10-25-titles.md`, item 8).
- **Do not assume the mask is enforced** (see above), and do not rely on `known_names[]` to register anything: it is names only.
- **AArch32 is a second front door.** An svc that works for 64-bit titles but has no `svc_sigs[]` row fails for MK8D 1.0; `QueryMemory` is special-cased in `a32_svc_impl` (40-byte `MemoryInfo`).
- **Code memory limits.** Code generated at run time runs natively without the AOT rewrite and must not contain `svc`, `x18` or `tpidr` accesses (`kcode.c` header).
