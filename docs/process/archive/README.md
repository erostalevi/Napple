# Process archive

Hand-off files of the unattended parallel rounds, moved here once their branch landed (`/napple-integrate`). Each round has an orchestration file per
work item (`orchestrations/<name>.md`: goal, owned paths, acceptance, written by `/napple-orchestrate`) and the integrator file its session wrote
(`integrators/<branch>.md`: what was done, evidence, how to integrate and test). The workflow is in the [process README](../README.md); the
matching change notes are listed in the [change archive](../../archive/changes/README.md#unattended-rounds-2026-10-27-b7-to-2026-11-02-r9).

They are kept as written: paths, line numbers and statuses are those of the time, and links inside them are not checked.

## Round 2026-11-01-r1

| Item | Orchestration | Integrator file | Goal |
|---|---|---|---|
| gfx-msaa-views | [gfx-msaa-views.md](orchestrations/gfx-msaa-views.md) | [claude-gfx-msaa-views-2026-11-01-r1.md](integrators/claude-gfx-msaa-views-2026-11-01-r1.md) | MSAA views, copy-backed texture views, sample shading, depth resolve, sparse |
| gfx-stages-live | [gfx-stages-live.md](orchestrations/gfx-stages-live.md) | [claude-gfx-stages-live-2026-11-01-r1.md](integrators/claude-gfx-stages-live-2026-11-01-r1.md) | geometry / tessellation / transform feedback against the real titles |
| media-live | [media-live.md](orchestrations/media-live.md) | [claude-media-live-2026-11-01-r1.md](integrators/claude-media-live-2026-11-01-r1.md) | verify video, movie audio and album JPEG on real titles |
| sched-applets | [sched-applets.md](orchestrations/sched-applets.md) | [claude-sched-applets-2026-11-01-r1.md](integrators/claude-sched-applets-2026-11-01-r1.md) | Horizon scheduling fixes, swkbd + offline web on real titles (ACNH race still open) |
| sweep-small | [sweep-small.md](orchestrations/sweep-small.md) | [claude-sweep-small-2026-11-01-r1.md](integrators/claude-sweep-small-2026-11-01-r1.md) | close seven small partial items (audren perf, audin mic, retro pad styles, NSZ, tickets, NCA2/0, bcat storage) |

## Round 2026-11-01-r2

| Item | Orchestration | Integrator file | Goal |
|---|---|---|---|
| gfx-nvn-api | [gfx-nvn-api.md](orchestrations/gfx-nvn-api.md) | [claude-gfx-nvn-api-2026-11-01-r2.md](integrators/claude-gfx-nvn-api-2026-11-01-r2.md) | the 14 missing NVN entry points, refused MSAA branches, ACNH program hash |
| gfx-shader-views | [gfx-shader-views.md](orchestrations/gfx-shader-views.md) | [claude-gfx-shader-views-2026-11-01-r2.md](integrators/claude-gfx-shader-views-2026-11-01-r2.md) | full shader corpus of the four titles, integer textures, ACNH cube view, sparse remap, MK8D archive |
| ipc-depth-hid | [ipc-depth-hid.md](orchestrations/ipc-depth-hid.md) | [claude-ipc-depth-hid-2026-11-01-r2.md](integrators/claude-ipc-depth-hid-2026-11-01-r2.md) | IPC reference lists + runner, and the missing rows for hid, audio and the system-device services |
| ipc-depth-sys | [ipc-depth-sys.md](orchestrations/ipc-depth-sys.md) | [claude-ipc-depth-sys-2026-11-01-r2.md](integrators/claude-ipc-depth-sys-2026-11-01-r2.md) | IPC reference fixtures and missing command rows for the system / offline services |
| sweep-media-r2 | [sweep-media-r2.md](orchestrations/sweep-media-r2.md) | [claude-sweep-media-r2-2026-11-01-r2.md](integrators/claude-sweep-media-r2-2026-11-01-r2.md) | movie AAC / VP8 / ARGB, clean guest exit, npad device types, LAS golden route, doc rows |

## Round 2026-11-01-r3

| Item | Orchestration | Integrator file | Goal |
|---|---|---|---|
| acnh-race-atomics | [acnh-race-atomics.md](orchestrations/acnh-race-atomics.md) | [claude-acnh-race-atomics-2026-11-01-r3.md](integrators/claude-acnh-race-atomics-2026-11-01-r3.md) | ACNH base start crash fixed (guest spinlocks need acquire ordering on Apple cores) |
| am-depth-applets | [am-depth-applets.md](orchestrations/am-depth-applets.md) | [claude-am-depth-applets-2026-11-01-r3.md](integrators/claude-am-depth-applets-2026-11-01-r3.md) | am IPC reference + rows, offline web kinds, applets on titles, frame-based input scripts |
| gfx-replay-gaps | [gfx-replay-gaps.md](orchestrations/gfx-replay-gaps.md) | [claude-gfx-replay-gaps-2026-11-01-r3.md](integrators/claude-gfx-replay-gaps-2026-11-01-r3.md) | D24 readback, separate texture/sampler binds, integer 3D/cube/MS views |
| ipc-local-stubs | [ipc-local-stubs.md](orchestrations/ipc-local-stubs.md) | [claude-ipc-local-stubs-2026-11-01-r3.md](integrators/claude-ipc-local-stubs-2026-11-01-r3.md) | 66 local documented-default IPC stubs become real behaviour; per-stream audio gain |
| nvn-object-layouts | [nvn-object-layouts.md](orchestrations/nvn-object-layouts.md) | [claude-nvn-object-layouts-2026-11-01-r3.md](integrators/claude-nvn-object-layouts-2026-11-01-r3.md) | guest-visible NVN objects within the smallest size the titles give them |

## Round 2026-11-02-r4

| Item | Orchestration | Integrator file | Goal |
|---|---|---|---|
| am-stubs-legal | [am-stubs-legal.md](orchestrations/am-stubs-legal.md) | [claude-am-stubs-legal-2026-11-02-r4.md](integrators/claude-am-stubs-legal-2026-11-02-r4.md) | title legal pages, am stub rows made real, offline network-connection applet |
| core-ordering-kernel | [core-ordering-kernel.md](orchestrations/core-ordering-kernel.md) | [claude-core-ordering-kernel-2026-11-02-r4.md](integrators/claude-core-ordering-kernel-2026-11-02-r4.md) | acquire/release for 32-bit exclusives, svcGetInfo 19/27, clean ExitProcess summary, docs links |
| gfx-views-raster | [gfx-views-raster.md](orchestrations/gfx-views-raster.md) | [claude-gfx-views-raster-2026-11-02-r4.md](integrators/claude-gfx-views-raster-2026-11-02-r4.md) | stencil cube views, remaining integer views, separate-texture slot masks, viewport swizzles, provoking vertex |
| ipc-reference-audit | [ipc-reference-audit.md](orchestrations/ipc-reference-audit.md) | [claude-ipc-reference-audit-2026-11-02-r4.md](integrators/claude-ipc-reference-audit-2026-11-02-r4.md) | misnumbered IPC rows follow switchbrew, nifm answers like an offline console, 17 more stubs real |

## Round 2026-11-02-r5

| Item | Orchestration | Integrator file | Goal |
|---|---|---|---|
| leftovers-a32-viewport-docs | [leftovers-a32-viewport-docs.md](orchestrations/leftovers-a32-viewport-docs.md) | [claude-leftovers-a32-viewport-docs-2026-11-02-r5.md](integrators/claude-leftovers-a32-viewport-docs-2026-11-02-r5.md) | interpreter lda/stl seq_cst + alignment fault, per-viewport swizzles, docs orphans |
| nifm-netconnect | [nifm-netconnect.md](orchestrations/nifm-netconnect.md) | [claude-nifm-netconnect-2026-11-02-r5.md](integrators/claude-nifm-netconnect-2026-11-02-r5.md) | nifm Cancel semantics + the real NetConnect output (MK8D Online Play shows the console's flow) |

## Round 2026-11-02-r6

| Item | Orchestration | Integrator file | Goal |
|---|---|---|---|
| net-host-provider | [net-host-provider.md](orchestrations/net-host-provider.md) | [claude-net-host-provider-2026-11-02-r6.md](integrators/claude-net-host-provider-2026-11-02-r6.md) | opt-in macOS host network provider (`NAPPLE_NET=host`): sockets, DNS with the vendor's domains refused, TLS, nifm, LDN over the LAN |
| error-applet-output | [error-applet-output.md](orchestrations/error-applet-output.md) | [claude-error-applet-output-2026-11-02-r6.md](integrators/claude-error-applet-output-2026-11-02-r6.md) | empty applet channels answer 0x480, error applet modes decoded (MK8D Online Play with a network returns to its menu) |
| gfx-multi-viewport | [gfx-multi-viewport.md](orchestrations/gfx-multi-viewport.md) | [claude-gfx-multi-viewport-2026-11-02-r6.md](integrators/claude-gfx-multi-viewport-2026-11-02-r6.md) | 16 viewports / scissors / depth ranges on the encoder, viewport index and layer outputs, layered passes on demand |

## Round 2026-11-02-r7

| Item | Orchestration | Integrator file | Goal |
|---|---|---|---|
| napple-infra | [napple-infra.md](orchestrations/napple-infra.md) | [claude-napple-infra-2026-11-02-r7.md](integrators/claude-napple-infra-2026-11-02-r7.md) | local test stack + AWS auto-deploy of the Napple game servers (`pretend.napple.lat`) and the `napple.lat` DNS |
| net-rooms | [net-rooms.md](orchestrations/net-rooms.md) | [claude-net-rooms-2026-11-02-r7.md](integrators/claude-net-rooms-2026-11-02-r7.md) | `napple-room` relay server + runtime client: virtual LAN and LDN between Napple instances over the internet (`NAPPLE_ROOM`) |
| mk8d-lan-session | [mk8d-lan-session.md](orchestrations/mk8d-lan-session.md) | [claude-mk8d-lan-session-2026-11-02-r7.md](integrators/claude-mk8d-lan-session-2026-11-02-r7.md) | MK8D 2618-0006 fixed (bsd errno numbering), nifm IpConfigInfo, unique pseudo device id; two MK8D instances meet in LAN Play through napple-room |
| napple-online | [napple-online.md](orchestrations/napple-online.md) | [claude-napple-online-2026-11-02-r7.md](integrators/claude-napple-online-2026-11-02-r7.md) | Pretendo-style Napple online service (accounts/tokens, NEX over WebSocket) + runtime glue; MK8D reaches its online menu |
| online-pia | [online-pia.md](orchestrations/online-pia.md) | [claude-online-pia-2026-11-02-r7.md](integrators/claude-online-pia-2026-11-02-r7.md) | SessionKeepFailed fixed (verbose-gathering join notifications), Pia NAT check server + glue; two MK8D instances matched into one gathering |
| online-race | (follow-up by message, no orchestration file) | [claude-online-race-2026-11-02-r7.md](integrators/claude-online-race-2026-11-02-r7.md) | nifm IP in network order, station URLs with type=3 and the NAT-check port; the joiner reaches NAT traversal with the host (race lobby still open: 2618-0502) |

## Round 2026-11-02-r8

| Item | Orchestration | Integrator file | Goal |
|---|---|---|---|
| napple-account | [napple-account.md](orchestrations/napple-account.md) | [claude-napple-account-2026-11-02-r8.md](integrators/claude-napple-account-2026-11-02-r8.md) | the Napple Account server: sign-in, devices, JWT/JWKS, E2E save-key exchange, profiles with reserved names, cloud-save metadata (no billing) |
| cloud-saves-client | [cloud-saves-client.md](orchestrations/cloud-saves-client.md) | [claude-cloud-saves-client-2026-11-02-r8.md](integrators/claude-cloud-saves-client-2026-11-02-r8.md) | end-to-end encrypted cloud saves: napple-cloud crate, runtime hooks, `napple account` / `napple saves` CLI |

## Round 2026-11-02-r9

| Item | Orchestration | Integrator file | Goal |
|---|---|---|---|
| napple-social-cloud | [napple-social-cloud.md](orchestrations/napple-social-cloud.md) | [claude-napple-social-cloud-2026-11-02-r9.md](integrators/claude-napple-social-cloud-2026-11-02-r9.md) | friends, presence and invites service for Napple players (REST + WebSocket) |
