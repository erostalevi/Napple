# Contributor guides

How-to pages for the common changes. Each has a goal, the files to touch, steps, a worked example from existing code, how to test and the pitfalls found so far.
Background on how the pieces fit: [architecture](../architecture/overview.md); per-subsystem entry points: [subsystems](../reference/subsystems.md); known problems by symptom: [symptoms](../symptoms.md).

| Guide | One line |
|---|---|
| [add-ipc-service.md](add-ipc-service.md) | Add an HLE IPC service, or a command of an existing one (CMIF and TIPC, `sm` registration, `*_core.c` logic, unit tests). |
| [add-svc.md](add-svc.md) | Add or extend a Horizon svc (`svc_register`, register layout, AArch32 table, NPDM svc mask, host tests). |
| [add-nvn-function.md](add-nvn-function.md) | Add an NVN API function (`names.txt`, `NVN()` macro, command recording versus immediate, replay in `mtl_replay.m`, `gfx/tests`). |
| [add-maxwell-shader-op.md](add-maxwell-shader-op.md) | Add a Maxwell instruction to the shader translator and test that the generated MSL is right. |
| [gpu-title-scan.md](gpu-title-scan.md) | Scan a title's shader control sections for the GPU stages it uses and pick the fewest titles to cover them all. |
| [add-game-support.md](add-game-support.md) | Bring up a new game: `napple build`, headless boot, reading the failure, title page, compatibility matrix. |
| [add-signatures.md](add-signatures.md) | Add per-build signatures to `knownSignatures/`: format, `napple sigs`, what must never be committed. |
| [debugging.md](debugging.md) | Crash and hang triage: `CRASH` output, tracing, screenshots, perf log, scripted input, the four reference titles. |
| [network-providers.md](network-providers.md) | The `OnlineProvider` extension point: what bsd, nifm, sfdnsres, ssl, friend, nsd and prepo ask of it, the C ABI, loading a provider module, the NullProvider. |
| [online-rooms.md](online-rooms.md) | Napple rooms: local wireless (LDN) and LAN play between Napple instances over the internet through a `napple-room` server; running a server, joining, what is relayed, limits. |
| [napple-online.md](napple-online.md) | The Napple online service (`crates/napple-online`, `NAPPLE_ONLINE_SERVER`): Napple accounts and tokens, the NEX game server (PRUDP Lite over WebSocket), MK8D's online path, boundaries, deployment. |
| [napple-account.md](napple-account.md) | The Napple Account server (`crates/napple-account`): accounts (email + password, emailed codes, SES/SMTP mail), devices, JWT/JWKS, E2E save-key exchange, profiles and reserved names, cloud-save metadata, deployment. |
| [cloud-saves.md](cloud-saves.md) | End-to-end encrypted cloud saves with a Napple Account (`crates/napple-cloud`, `napple account` / `napple saves`): devices, approval and recovery, sync rules, conflicts, wire details, environment variables. |
| [napple-social.md](napple-social.md) | The Napple friends and presence service (`crates/napple-social`): API, WebSocket events, privacy, data model, running it. |
| [napple-online-client.md](napple-online-client.md) | The Mac-side Napple Online client (`crates/napple-cloud` online module + `runtime/src/online`): shared credentials, profile map, the overlay function table. |
| [online-e2e.md](online-e2e.md) | Napple Online end-to-end: the scripted 60-check run (sign-up, second device, cloud save, friends, tiers, delete) locally and live, overlay screenshots. |
| [hardware-simulators.md](hardware-simulators.md) | NFC / amiibo (virtual amiibo, picker contract, optional dumps) and the IR camera simulator, and how the in-game overlay wires them. |
| [studio.md](studio.md) | Napple Studio: the panes, building for a Mac, iPhone/iPad devices and Simulators, signing modes, bundle ids, the free/paid memory-limit rule, debug flags. |
| [ios-device-candidate.md](ios-device-candidate.md) | Which iPhone/iPad fits which games: Simulator profiling method, memory figures, picks. |
| [fuzzing.md](fuzzing.md) | Run the Rust cargo-fuzz targets, the seeded-mutation tests and the C runtime fuzz drivers; fix and record a finding ([findings](../reference/fuzz-findings.md)). |
| [testing.md](testing.md) | Every test suite, its command, and whether it runs on Linux or only on macOS. |
| [deploy-servers.md](deploy-servers.md) | Deploy the Napple game servers to AWS at `pretend.napple.lat` and the `napple.lat` DNS: one-time setup, the workflow, rollback, costs, the local test stack. |
| [testing-graphics.md](testing-graphics.md) | Golden-frame routes per title, the gfx warning and Metal validation gate, contact sheets, approving goldens. |

### For community contributors
| Guide | One line |
|---|---|
| [contributing-report-a-bug.md](contributing-report-a-bug.md) | Capture the machine details and a trimmed log for a bug report. |
| [contributing-document-a-title.md](contributing-document-a-title.md) | Run the scripted boot, write `docs/titles/<id>.md`, update the matrix and regenerate the support tables ([field reference](../reference/catalogue-howto.md)). |
| [contributing-report-a-graphics-bug.md](contributing-report-a-graphics-bug.md) | Capture frames and graphics logs, narrow a wrong-pixels bug down. |
| [contributing-write-a-mod.md](contributing-write-a-mod.md) | Rules and workflow for mods and game fixes (the format itself lives in the mod docs). |
| [contributing-community.md](contributing-community.md) | Labels, Discussions categories, conduct and security check, sign-off decision. |

Related references: [environment variables](../reference/environment-variables.md), [command line](../reference/cli.md), [glossary](../reference/glossary.md).
