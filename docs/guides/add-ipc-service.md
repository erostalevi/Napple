# Add an HLE IPC service, or a command to an existing one

## Goal
Make a Horizon service name (for example `lbl`, `psm`, `mii:u`) or a missing command of an existing service answer in the runtime, so that a game
that asks for it no longer stops with `IPC command <service>:<id> not implemented` or `IPC service "<name>" not implemented`.
Background: [runtime-services.md](../architecture/runtime-services.md) (policy, status per service), [subsystems.md](../reference/subsystems.md#ipc-and-hle-services).

## How a request reaches your code
1. The guest connects to `sm:` (svc `ConnectToNamedPort`, `svc_connect_to_named_port` in `runtime/src/ipc.c`) and sends `GetServiceHandle` (cmd 1,
   an 8-byte name) to the object made by `sm_open`. The handler is `sm_get_service` in `runtime/src/ipc.c`.
2. `sm_get_service` first asks `npdm_service_allowed(name)` (`runtime/src/npdm.c`): a name that is not in the title's NPDM service access list is
   refused with result `IPC_R_NOT_FOUND(21, 8)`, even if the service is registered. The SDK then falls back to another name (`pctl`, then `pctl:s`...).
3. Then it looks the name up in the registry filled by `ipc_register_service(name, open)`. The match is `strncmp(..., 8)` and the **last**
   registered match wins (the loop does not stop at the first). No match: the runtime logs `sm: service "<name>" is not implemented`, adds
   `missing service <name>` to the summary and stops (`FAIL:` line, exit status 70), or with `NAPPLE_IPC_LENIENT=1` answers `IPC_R_NOT_FOUND(21, 5)`.
4. `open()` returns an `IpcObject`; its session handle goes back to the guest. Later requests (`SendSyncRequest`, svc 0x21) are parsed in `dispatch()`
   (`runtime/src/ipc.c`) and reach `run_object()`, which searches the object's `IpcCmd` table linearly for the command id.
   A missing id is `unimplemented()`: log line, `missing ipc command <iface>:<id>` in the summary, `runtime_fail`.

## CMIF versus TIPC
Both reach the same `IpcCmd` handler with the same `IpcCall`; you do not write two handlers.
- **CMIF** (HIPC message types 4 and 6): a 16-byte `SFCI` header carries the command id; `c->in` points right after it. A session may be converted to a
  *domain* (control request 0), in which case sub-objects are object ids instead of session handles. `ipc_out_object()` hides the difference.
- **TIPC** (Horizon 12+, message type >= 16): `dispatch_tipc()` takes the command id as `type - 16`, `c->in` are the raw words after the
  descriptors (no header, no alignment), and the reply carries the result code as its first raw word (`respond_tipc()`). No domains, no control requests.
  Objects passed as arguments arrive as copy handles; objects you return leave as move handles.
- What differs for you: nothing in the handler, except `c->type >= 16` (16 is `T_TIPC_BASE`, private to `ipc.c`) and the layout of the `sm` error reply
  (`sm_tipc_error_layout`). The Animal Crossing 3.0.x SDK uses TIPC for `sm`. Both paths are covered by `runtime/tests/test_ipc.c`.

## Files to touch
Each path below exists today.

| Path | What |
|---|---|
| `runtime/src/services/<name>.c` | The glue: handlers, `IpcCmd` table, `<name>_open`, `service_register_<name>()`. Model: `runtime/src/services/lbl.c`. |
| `runtime/src/services/<name>_core.c` and `_core.h` | Optional, portable logic (state, clamping, tables) with no Mach, Cocoa or Metal include. Model: `lbl_core.c`, `lbl_core.h`. |
| `runtime/src/services/services.c` | Add `void service_register_<name>(void);` and a call in `services_init()`. This file is a list of register calls, not a table of commands. |
| `runtime/tests/test_svc_<name>.c` | The unit test. Model: `runtime/tests/test_svc_lbl.c`. Helpers in `runtime/tests/svc_test.h` and `runtime/tests/check.h`. |
| `runtime/src/services/svc_common.h` | Reusable handlers (`svc_noop`, `svc_ret_u8_0`, `svc_ret_u32_0`, `svc_ret_u64_0`, `svc_ret_event`, `svc_ret_event_signaled`, ...). |
| `runtime/src/services/hz_result.h` | `HZ_RESULT(module, desc)` and the result codes used so far (many are marked `[unverified]`). |
| `docs/architecture/runtime-services.md` | Add or update the row for the service (status word: implemented, partial, stub, unbuilt). |

No Makefile edit is needed: `runtime/Makefile` compiles `src/services/*.c`, `runtime/tests/services.mk` builds every `tests/test_svc_*.c`, and
`tools/linux-check/run-tests.sh` runs the same files and links every `src/services/*_core.c`.

## Steps
1. **Check that the game asks for it and may open it.** `napple compat npdm <game.nsp|xci> -k <prod.keys>` prints `services used` from the NPDM. A name
   that is not listed there is refused by `sm` before your code runs. The first run of the title tells you the command ids that are actually used:
   the log line `ipc: <iface>: command <id> not implemented (type ..., N argument bytes, buffers x.. a.. b.. w.. c.., N copy + N move handles)` gives the
   size of the arguments and the buffers (X/A input, B/C output). `NAPPLE_IPC_LENIENT=1` lets one run list several gaps.
2. **Write the portable core first** if there is state or arithmetic: a struct, an init function, setters/getters that clamp or validate (`lbl_core.c`).
3. **Write the glue.** One `static void h(IpcObject *self, IpcCall *c)` per command.
   - Read arguments with `IPC_IN(c, T, offset)` (offset into the raw argument bytes). An offset past the request stops the runtime with
     `IPC argument out of range`, so only read what the command really carries. Where the layout is uncertain, accept the call without reading (`lbl.c` does this).
   - Return values with `IPC_OUT(c, v)` (aligned to `min(size, 8)`, at most 0x100 bytes in total), `ipc_out_zero`, `ipc_out_copy_handle` (events, shared memory),
     `ipc_out_move_handle`, `ipc_out_object` (a sub-interface), or an error with `c->result = ...` / `ipc_fail`.
   - Buffers: `ipc_in_buf(c, n)` (map-alias A buffers first, then pointer X buffers) and `ipc_out_buf(c, n)` (B then C). Write output with `ipc_write_buf()`:
     it calls `guest_write_prepare` (needed for gfx write tracking) and adds the size descriptor for receive-list buffers. Read input through `ipc_buf_ptr()`.
4. **Fill the table.** `{id, "Name", handler, IPC_IMPL}` for real behaviour, `IPC_STUB` for a documented safe default. The flag only changes which list of the
   summary the command appears in (`implemented_ipc` or `stubbed_ipc`).
5. **Open and register.**
   ```c
   static IpcObject *demo_open(void) { return IPC_OBJECT("demo:u", demo_cmds, NULL, NULL); }
   void service_register_demo(void) { ipc_register_service("demo:u", demo_open); }
   ```
   `IPC_OBJECT(iface, table, state, destroy)` computes the table length. Register extra names for the same object with more `ipc_register_service` calls
   (`fgm` also registers `fgm:0` and `fgm:9`; `pl:u` and `pl:s` share `pl_open`).
6. **Hook it up** in `services/services.c` (declaration plus call in `services_init`).
7. **Write the test** (next section), run it, then update `runtime-services.md`.

To add a command to an existing service, only steps 3, 4 and 7 apply: add the handler and one row to the service's `IpcCmd` array, and one `svc_call` block to its test.

## Worked example
Modelled on `runtime/src/services/lbl.c` (functions `lbl_set_current`, `lbl_get_current`, table `lbl_cmds`, `lbl_open`, `service_register_lbl`) and on its core
`lbl_core.c: lbl_set_brightness`. The names `demo` and the ids are made up.

```c
/* runtime/src/services/demo.c */
#include "svc_common.h"
#include "demo_core.h"
static DemoState demo; static int demo_ready;
static DemoState *state(void) { if (!demo_ready) { demo_state_init(&demo); demo_ready = 1; } return &demo; }
static void demo_set(IpcObject *s, IpcCall *c) { (void)s; demo_set_level(state(), IPC_IN(c, float, 0)); }   /* in: f32 */
static void demo_get(IpcObject *s, IpcCall *c) { (void)s; float v = state()->level; IPC_OUT(c, v); }          /* out: f32 */
static const IpcCmd demo_cmds[] = {
    {0, "SetLevel", demo_set, IPC_IMPL}, {1, "GetLevel", demo_get, IPC_IMPL}, {2, "Save", svc_noop, IPC_STUB},
};
static IpcObject *demo_open(void) { return IPC_OBJECT("demo:u", demo_cmds, NULL, NULL); }
void service_register_demo(void) { ipc_register_service("demo:u", demo_open); }
```
A sub-interface (`ipc_out_object`) is in `runtime/src/services/lm.c: lm_open_logger`; a list written into an output buffer is in
`runtime/src/services/mii.c: list_into`; a result code is in `mii.c: db_find_index` (`c->result = HZ_RESULT(HZ_MOD_MII, 4)`).

## How to test
**Unit test shape** (`runtime/tests/test_svc_lbl.c`): the test `#include`s the service **source** (`#include "../src/services/lbl.c"`) so it can reach the static
handlers, then `#include "svc_test.h"`, opens the object with `lbl_open()`, and drives it with `svc_call(o, cmd, in, n, &c)`, checking `c.result`,
`c.out_size` and `SVC_OUT(&c, T, off)`. `svc_call_c` is for tests that prepare buffers or handles (`svc_set_in_buf`, `svc_set_out_buf`). End with the
`{"test":...,"checked":N,"failed":M}` line and `return g_failed != 0;`. Every command you add should also be checked to be *in the table*: `svc_call`
fails when the id is missing.

**How the test links the core.** `runtime/tests/services.mk` takes every `tests/test_svc_*.c`; `svc_included` greps the test for `services/<file>.c` and removes that
object from the link (the test already contains it). All other runtime objects, including every `*_core.c`, link normally. If your test includes two
service sources, both are dropped. On Linux `tools/linux-check/run-tests.sh` compiles the test plus all `src/services/*_core.c` plus `tools/linux-check/ipc_mock.c`
(a stand-in for `ipc.c`, `kernel.c`, `log.c`) with `-include tools/linux-check/compat.h`.

| Where | Command | What it proves |
|---|---|---|
| macOS | `make -C runtime test-services` | builds and runs every `test_svc_*` against the real runtime objects, plus `test_hid_depth` |
| macOS | `make -C runtime test` | `test_media`, `test_ipc` (CMIF, domains, TIPC, `sm`), `test_fs_journal`, `test_hid_service`, `test_timescale` |
| Linux | `sh tools/linux-check/run-tests.sh` (or `... runtime/tests/test_svc_demo.c`) | the test and the `*_core.c` logic against the mock |
| Linux | `sh tools/linux-check/check-syntax.sh runtime/src/services/demo.c` | `-fsyntax-only` with gcc and clang over stub headers |

Measured on a Linux box for this guide: `run-tests.sh` ran 29 of the 32 `test_svc_*.c` files with 0 failures; `test_svc_fs`, `test_svc_fs_dataid` (need `romfs_map.c`) and
`test_svc_time` (needs `host_symbols_register`) do not link against the mock. Passing the Linux run says the C is well formed and the core logic matches the test; it
says nothing about the real `ipc.c` or a game. A command id or layout written from memory stays *unverified* until a title uses it: say so in the header comment
of the file (`mii.c`, `hz_result.h` do) and in the `runtime-services.md` row.

Then boot a title that uses the service (see [add-game-support.md](add-game-support.md)) and read `ipc:` lines with `NAPPLE_TRACE_IPC=all`
([debugging.md](debugging.md)).

## Common pitfalls
- **An unknown command is fatal, not skipped.** `FAIL: IPC command <iface>:<id> not implemented`, exit status 70. Add it as an `IPC_STUB` with a safe answer
  rather than leaving it out, or use `NAPPLE_IPC_LENIENT=1` only to enumerate.
- **The NPDM list gates the name.** Registering a service the game's NPDM does not list changes nothing for that game. Conversely a game may probe several names
  (`pctl`, `pctl:s`, `pctl:r`, `pctl:a`) and expect the refusal of some.
- **A refusal over TIPC must keep the success layout** with a move handle 0 (`sm_tipc_error_layout`), otherwise the SDK reads a request word as "success, handle 0" and aborts
  with "request on handle 0" ([symptoms.md](../symptoms.md), Animal Crossing 3.0.x).
- **Return a handle where the real reply has one.** `IApplicationFunctions` cmd 210 once returned an object where the reply carries a copy handle to an event, and the guest spun on
  `WaitSynchronization(handle 0)` ([symptoms.md](../symptoms.md), Animal Crossing boot spin). Events: `svc_ret_event` (never signalled) or `svc_ret_event_signaled`.
  A never-signalled event that the game waits on without timeout blocks forever; an `appletOE` message event that was never de-asserted was once a service bug
  ([runtime-services.md](../architecture/runtime-services.md), "Kernel scheduling note").
- **Duplicate names override.** The last `ipc_register_service` with the same first 8 characters wins. `audin:u` is registered by `audio.c`; `audin.c` is kept for its unit test only
  (comment in `services_init`). The registry holds 128 services and names up to 11 characters (`Service.name[12]`), compared on 8.
- **Do not write into guest memory with `memcpy` from a handler** when `ipc_write_buf` can do it: a host-side write into a page that gfx write-protected makes a kernel `read()` fail
  with `EFAULT` instead of faulting through the handler ([gfx.md](../architecture/gfx.md), section 3).
- **The response is limited to 0x100 bytes** (`IpcCall.out`, `MSG_BYTES`); larger results go into an output buffer.
- **Keep trace aids out of the core and out of files the host tests link.** A debug helper referenced from runtime code but not linked by the host tests broke `make -C runtime`
  ([symptoms.md](../symptoms.md), Building).
- **Do not change a shared handler without a second look.** Both `ios-sim` and `mk8d-64` once added `time:u` cmd 300 and conflicted ([symptoms.md](../symptoms.md), Services and input);
  `grep` the table for the id before adding it.
- **Honest status.** The services added in `services-2026-10-24` were written on Linux and never run on a console; see the status words in `docs/archive/changes/2026-10-24-services-VERIFY.md`.
