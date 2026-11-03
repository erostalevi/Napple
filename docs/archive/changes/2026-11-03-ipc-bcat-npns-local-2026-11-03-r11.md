# IPC depth: bcat and npns:u local-state rows (r11)

Converts 12 documented-default `IPC_STUB` rows to `IPC_IMPL` with host-side state and a unit test each. No network is involved; the
NullProvider/offline behaviour of the existing rows is unchanged.

## IBcatService (`bcat.c`, `bcat_core.[ch]`)
State lives in `bcat_core.c` (timer table of 16, subscription flags, stop latch, push log ring of 16, a generation counter bumped on every change).

| id | name | behaviour |
|---|---|---|
| 30210 | SetDeliveryTaskTimer | upserts an application timer; seconds 0 removes it; subscribes; full table -> invalid-argument result |
| 20410 | SetSystemApplicationDeliveryTaskTimer | same, separate system-timer namespace |
| 90202 | ClearDeliveryTaskSubscriptionStatus | clears the subscription flag of that key's timers, timers stay |
| 30300 | RegisterSystemApplicationDeliveryTasks | registers system tasks (optional u64 id array, else task 0) |
| 20500 | StopBackgroundService | drops all timers, latches stopped; a later timer set restarts |
| 90300 | GetPushNotificationLog | out array of 0x18-byte records + s32 count; empty by default |
| 90301 | GetDeliveryCacheStorageUsage | 0x10 bytes {s64 bytes, s32 dirs, s32 files} summed over the cache provider (zero without one) |

Decision: 90300/90301 previously answered the no-network result; they now succeed because the data is local (an empty log / empty cache offline).

## npns:u (`push.c`)
Local state: connection state (0 default), notification queue (16, empty), received/dropped counters, JID string (empty). Events from 5, 7 and 153 are
remembered and signalled by the host hooks (`npns_local_enqueue`, `_set_state`).
103 GetState and 151 GetStateWithHandover read the state; 104 GetStatistics {u64 received, u64 dropped}; 111 GetJid fills the out buffer with the local JID
(empty success when unset); 153 GetDropEventWithHandover fires when the full queue drops its oldest entry. Rows 5 and 7 (already IMPL) now fire
on the same state; with nothing queued they still never fire. Receive (3/4) is unchanged and does not yet pop the queue.

## Unverified
Argument/record layouts above are the author's choices (ids and names are from `tests/ipc_reference`, unchanged); sizes marked [unverified] in the sources.
Fixtures were not edited (ids and names matched).
