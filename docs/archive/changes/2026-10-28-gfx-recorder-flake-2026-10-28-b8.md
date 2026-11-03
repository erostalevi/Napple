# 2026-10-28 gfx-recorder-flake (batch 2026-10-28-b8, Mac)

The ACNH start crash (10-30 % of headless base starts, ~5 % on the update) was host heap corruption in the NVN command recorder (`gfx/src/api_cmd.c`), caused by
the guest using one stack-allocated `NVNcommandBuffer` from two threads at once. The recorder is now safe against that misuse; the guest-side race itself
(why two threads record the same object at once) is a runtime matter and is reported, not fixed.

## Root cause
Traced with the new `NAPPLE_GFX_CBTRACE=1` (below). In ACNH base (`7fc1baff976aeca4`):
- `main+0x1ae75c0` records one object's command list. It builds a 0x1c0-byte context **on its own stack**, binds it into a shared per-object slot
  (`obj->array[bufidx]`, 0x260-byte elements, `bufidx = obj[+0x2e5] & 3`), then calls its Begin/End wrappers (`main+0x19cddb0`, `main+0x19cdee0`), which
  reach the command buffer through the slot: `slot->[0x30]` (the bound context) `->[0xc0]` = context + 0x110.
- Two guest threads ran this for the same object and buffer index at the same moment: the guest main thread (boot pthread stack, core 0) and worker 0x57
  (core 1) in one run, workers 0x57 and 0x58 (cores 1 and 2) in another. The later bind wins, so one thread's Begin/End and commands land on the **other
  thread's stack-allocated command buffer**, which its owner meanwhile finalizes, initialises again (a tight Init/Begin/End/Finalize loop, ~1 µs per round)
  and finally overwrites with ordinary stack frames (the dumps show the object holding stack garbage: magic `0xf000f`, "host pointer" = an address next to it).
- What that did to the old recorder (host part pointer stored in the guest object, `owner` check):
  - a second thread's `EndRecording`/`rec_op` used a host part the owner freed in `Finalize` at the same time (use after free; the documented
    `memcpy(data, h->buf, h->len)` crash under MallocScribble);
  - two threads `realloc`ed `h->buf` and updated `len`/`children` together (double free, torn lengths);
  - a command recorded on a finalized or never-initialised object went to a **64-byte static scratch**, and one recorded outside Begin/End to a
    **256-byte field of the host part**, whatever its size: `UpdateUniformBuffer` (up to 1 MiB) and `CallCommands` (8 bytes per handle) overflowed both
    into neighbouring globals / heap blocks. This matches the soak report's failures ("pointer being freed was not allocated", wild `memmove`, xzone
    freelist `SIGTRAP`, all attributed to `gfx_cmdrec_live+...`, the nearest exported symbol before the recorder's static functions).
- Timing: the default 10 ms timer slice makes the overlap likelier: 60 x 5 s base starts gave 2 anomaly runs (1 crash) with the default slice and 0 of
  60 with `NAPPLE_SLICE_MS=0`, in line with the earlier observation. It is not same-core slicing: the two threads were on different emulated cores.

## Is the command buffer shared by design?
No. Each thread builds its own context, and so its own command buffer, on its own stack; nothing in the guest code intends two threads to use one.
The sharing comes from the per-object slot both threads bind their context into. It is not stack reuse either: the traces show both threads alive and
inside the same Begin..End window at the same moment, one of them working on the other's live stack object.

NVN gives no thread safety for a command buffer object: like the other NVN objects it is not internally synchronized, and the application must make
sure only one thread at a time records into it. Hardware semantics therefore need no lock in the driver. On a console the game's own scheduling must keep
these two recordings of one object/buffer index apart; under Napple they overlap. That is a runtime-side divergence (thread scheduling or a guest
synchronization primitive), to be found by a runtime session.

The recorder's new lock exists only so that a misbehaving guest cannot corrupt the host heap; it gives the guest no ordering guarantees. A list recorded
during such an overlap can still hold the wrong thread's commands, exactly as it would on hardware with the same race.

## Fix (gfx/src/api_cmd.c, gfx/src/obj.h)
- `cmd_t.hid` (was `cmd_t.h`, a host pointer): slot number + 1 in the low word, generation in the high word. A lock-free chunked slot table maps slots to
  host parts; host parts are never freed (a finalized part goes to a free list and is reused by the next `Initialize` with a new generation). A guest
  object that is stale, byte-copied, finalized under another thread or overwritten with garbage resolves to "no host part", never to freed memory.
- Every change of the recording state (`buf`, `len`, `children`, `rec_*`, command memory) runs under the part's `os_unfair_lock`; the lookup is checked
  again under the lock. `EndRecording` copies and moves the children under it; `CallCommands` appends children under it and retains nothing when nothing
  is being recorded.
- The caller fills a command's payload after `rec_op` returns, so a growing buffer is not `realloc`ed but copied, and the old one is freed 2 s later
  (`grave_put`); payloads are zeroed when reserved, so a record copied by another thread's `EndRecording` before it is filled reads as an empty record
  (`CallCommands` count 0), never as garbage pointers.
- Commands that are not recorded (no host part, or outside Begin/End) go to a per-thread sink as large as the command (`cmd_sink`).
- `Initialize` on an object that still has a live host part retires that part first (it used to leak with its child references).
- Diagnostics: `NAPPLE_GFX_CBTRACE=1` keeps a 64 K-entry ring of Initialize/Finalize/Begin/End events (object, host thread, guest thread id and core via
  `dlsym` of the runtime's `threads_current_id`/`threads_current_core`, pthread stack range, a 6-frame backtrace) and prints, for the first 8 anomalies
  (call on a dead object, Begin while recording, End without Begin, End from another thread), every event of that object plus the caller's frame chain.

## Also fixed
- `gfx/tests/t_stages.c` kept shader control sections in `g_ctl[8]` but builds more than 8 stages: it overran into the driver's globals (it happened to
  hit nothing important before; with the new slot table it zeroed `g_cbh_n` and 7 recordings failed). Sized to the pool's shader slots, with a bound check.

## Verified here (Mac)
All runs headless through `tools/dev/gamelock.py`, `NAPPLE_AUDIO_DEVICE=0`, ACNH input script `20:A:0.3,30:A:0.3,40:A:0.3,50:A:0.3`, 30 s each, runtime built from this branch (`make -C runtime GFX=napple`).

| Run | Result |
|---|---|
| `make -C gfx test` | all pass (incl. new `t_cmdshare`, fixed `t_stages`) |
| `t_cmdshare` under `MallocScribble=1 MallocPreScribble=1`, old recorder (base `api_cmd.c`/`obj.h`) | crashes 3 of 3 (SIGTRAP / SIGSEGV) |
| same, new recorder | passes; ~360 k lists recorded by two threads sharing one object in 1.5 s |
| ACNH update+DLC `ff1d1c05670db602`, 40 starts | **0 failures** |
| ACNH base `7fc1baff976aeca4` + `ff1d1c05670db602`, 10 starts each under MallocScribble | **0 failures** each (the documented 16/16 repro did not reproduce on this base or on NappleLATEST before the fix either: 0 of 7) |
| ACNH base `7fc1baff976aeca4`, 40 starts | **3 failures, all guest faults, no host heap crash**: `main+0x1b61d3c` (null+0x28, the soak report's known guest crash), `main+0x19d04d4` (null+0x18), `main+0x667178` (null). None of the three logged a recorder warning. |
| 40 x 5 s base starts with `NAPPLE_GFX_CBTRACE=1` on the fixed build | 1 overlap detected and survived, 0 crashes (before the fix: 2 of 60 with 1 host crash) |

| ACNH base, 20 starts with `NAPPLE_SLICE_MS=0` | 0 failures |

So the host side is fixed. The base build still fails about 7 % of starts in guest code; that is the guest race (or another one) and needs the runtime fix.

## Not done / remaining
- The guest-side race (runtime): why two threads record the same object and buffer index at once. See "Needs" in the integrator file.
- Other gfx guest objects were checked for the same pattern: `sync_t.cell` points into the guest window object (guest memory, as on hardware);
  pools, buffers, textures keep host pointers but are long-lived objects that ACNH does not put on stacks. Left as they are.

## Files
`gfx/src/api_cmd.c`, `gfx/src/obj.h`, `gfx/tests/t_cmdshare.c` (new), `gfx/tests/t_stages.c`, `docs/todo.md`.
