# 2026-10-24 guest-handle: AC 3.0.x "request on handle 0" abort (branch claude/guest-handle-2026-10-24-plan10)

## Root cause
AC 3.0.1/3.0.3 (NVN 55.15 SDK) talk to sm over TIPC. Its GetServiceHandle (sdk+0x159070) sends the request, then reads the reply at fixed positions
(move handle = TLS word 3, result = word 4) as in a success reply, without looking at the special header. Our refusal for services outside the NPDM list
(`vi:m`, `vi:s`, `pctl:s/a/r`) was header + result word only, so word 4 still held the request's bytes (high half of the name): result 0, handle = junk/0.
The SDK treated vi:m/vi:s as available, `nn::vi::Initialize` (sdk+0x447b30) sent GetDisplayService (cmd 2) on handle 0, got 0xe401, SetTerminateResult + Break at sdk+0xc80730.
The harmless handle-0 control requests after pctl refusals were the same bug.

## Fix
`runtime/src/ipc.c`: `sm_tipc_error_layout()` puts a move handle 0 into TIPC sm error replies (refused / not-registered), keeping the success layout.
Test: `runtime/tests/test_ipc.c` (sm over TIPC error reply).

## Further missing commands hit once boot continued (all stubs)
ISelfController 44 CreateManagedDisplaySeparableLayer (layers 1,2), hid 1002/1003/1004, acc:u0 160, aoc:u 50, IApplicationFunctions 210 CreateMovieMaker (IMovieMaker 4/5 noop).

## Verified (scripted headless runs only)
AC 3.0.3 (ff1d1c05670db602) and 3.0.1 (8f2cb7a9774959c8): 45 s, exit 0, 0 CRASH. Afterwards the game reads RomFS (IStorage::Read) and the main thread parks in SleepThread
(main+0x37b8b54) with no further IPC: next problem is a stall, cause not investigated.
AC v0 (7fc1baff976aeca4): exit 70 at 0.65 s, guest Break 0x7d202 from sdk+0x12b50c0 (looks fs path related; fs.c not mine).

## LAS flake (tools/flaky_boot.sh, 20 s each, my build)
LAS 20 runs: 19 exit 0, run 19 exit 70: `FAIL: guest Break (reason 0, result 0x35f202) from sdk+0xd5f6c4` at 0.41 s (fs module 2 desc 6905), right after the 4th
"fs: mounted host directory .../01006BB00C6F0000/save/space1/account/4e617070" and SetTerminateResult 0x35f202. SMO 20 runs: 20/20 exit 0.
So the flake is real and in the save-data mount path (fs.c, not touched here); cause not investigated. Full log: side-session-notes/guest-handle-las-flake-run19.log.
