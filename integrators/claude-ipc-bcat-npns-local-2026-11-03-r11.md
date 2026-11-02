# Integrator note: claude-ipc-bcat-npns-local-2026-11-03-r11

Branch `claude/ipc-bcat-npns-local-2026-11-03-r11`. Details: `docs/archive/changes/2026-11-03-claude-ipc-bcat-npns-local-2026-11-03-r11.md`.

## Rows converted (12, IPC_STUB -> IPC_IMPL)
IBcatService: 20410, 20500, 30210, 30300, 90202, 90300, 90301. npns:u: 103, 104, 111, 151, 153.
Files touched: `bcat.c`, `bcat_core.[ch]`, `push.c`, `test_svc_bcat.c`, `test_svc_push.c`, the change note, this note. Fixtures unchanged.

## Tests run on Linux (tools/linux-check, gcc)
- `run-tests.sh test_svc_bcat.c test_svc_push.c`: bcat 221 checks, push 44 checks, 0 failed; also clean under ASan+UBSan.
- `check-syntax.sh`: bcat.c / push.c / bcat_core.c pass (one pre-existing `not_found` unused-function warning in bcat.c, not from this change).
- Full `run-tests.sh` over all service tests was started as a regression check; see the result recorded in the branch description if it finished.

## Unbuilt (never compiled on macOS)
All of the above C; commit subjects carry `unbuilt:` for the bcat commit. Portable C only, no new includes beyond `<stdio.h>`/`<string.h>`.

## Mac build/test
```bash
make -C runtime GFX=napple test test-services test-ipc-reference
```
`test-ipc-reference` was not run (Linux cannot build it); fixtures were not changed, so it should only see STUB->IMPL status changes.

## Risks
- Argument/record layouts (timer `u64 id + u32 seconds`, push log record 0x18, usage 0x10, npns stats 0x10, JID in out buffer) are unverified guesses; a guest with a different layout reads wrong values or may hit `ipc_in` range failures on short input.
- 30300 reads its optional id array via `ipc_buf_ptr`; not exercised against the real IPC layer.
- 90300/90301 no longer return the no-network error offline.
- Receive (npns 3/4) does not pop the queue yet; the queue is observable via statistics/events only.
- Global state in `bcat_core.c` / `push.c` is unsynchronised (assumes the single service thread, as the existing provider code does).
