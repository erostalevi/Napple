# Contributor guides

How-to pages for the common changes. Each has a goal, the files to touch, steps, a worked example from existing code, how to test and the pitfalls found so far.
Background on how the pieces fit: [architecture](../architecture/overview.md); per-subsystem entry points: [subsystems](../reference/subsystems.md); known problems by symptom: [symptoms](../symptoms.md).

| Guide | One line |
|---|---|
| [add-ipc-service.md](add-ipc-service.md) | Add an HLE IPC service, or a command of an existing one (CMIF and TIPC, `sm` registration, `*_core.c` logic, unit tests). |
| [add-svc.md](add-svc.md) | Add or extend a Horizon svc (`svc_register`, register layout, AArch32 table, NPDM svc mask, host tests). |
| [add-nvn-function.md](add-nvn-function.md) | Add an NVN API function (`names.txt`, `NVN()` macro, command recording versus immediate, replay in `mtl_replay.m`, `gfx/tests`). |
| [add-maxwell-shader-op.md](add-maxwell-shader-op.md) | Add a Maxwell instruction to the shader translator and test that the generated MSL is right. |
| [add-game-support.md](add-game-support.md) | Bring up a new game: `napple build`, headless boot, reading the failure, title page, compatibility matrix. |
| [add-signatures.md](add-signatures.md) | Add per-build signatures to `knownSignatures/`: format, `napple sigs`, what must never be committed. |
| [debugging.md](debugging.md) | Crash and hang triage: `CRASH` output, tracing, screenshots, perf log, scripted input, the four reference titles. |
| [testing.md](testing.md) | Every test suite, its command, and whether it runs on Linux or only on macOS. |

Related references: [environment variables](../reference/environment-variables.md), [command line](../reference/cli.md), [glossary](../reference/glossary.md).
