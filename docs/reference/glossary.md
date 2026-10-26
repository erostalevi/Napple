# Glossary

Terms used in the code and documentation, with the meaning they have in this project. Where a term is implemented in the repository, the file is given (paths relative to the `Napple/` project root).
Architecture pages: [overview](../architecture/overview.md), [module-format](../architecture/module-format.md), [rewrite-format](../architecture/rewrite-format.md),
[aarch32](../architecture/aarch32.md), [gfx](../architecture/gfx.md), [runtime-services](../architecture/runtime-services.md).

## A

**a32jit**
The ahead-of-time recompiler for AArch32 titles. `runtime/src/a32jit/a32lift.c` lifts ARM and Thumb code to AArch64 assembly that is built into `modules/a32jit.dylib` with `a32jit.manifest`.
The dispatcher `runtime/src/a32jit/a32jit.c` loads it; the name is historical, nothing is generated at run time.

**A64**
The AArch64 instruction set, which both the Switch's Tegra X1 and Apple Silicon execute. Napple runs 64-bit guest code natively and rewrites only the instructions macOS forbids.
Site detection is in `crates/napple-core/src/analysis/a64.rs`, the rewriter in `crates/napple-rewrite/src/a64.rs`.

**AArch32**
The 32-bit ARM instruction set (ARM and Thumb-2). Some titles (for example Mario Kart 8 Deluxe) are 32-bit ELF32 modules, which Apple Silicon cannot run. They take a separate path: NTB2 tables,
an interpreter in `runtime/src/a32/` and an optional ahead-of-time recompiler in `runtime/src/a32jit/`. See [aarch32](../architecture/aarch32.md).

**AOT (ahead-of-time)**
Work done at build time rather than at run time. The A64 rewrite, the AArch32 recompiler and the module tables are all produced by `napple build` before the game starts. There is no JIT
anywhere in Napple, which keeps iOS possible ([ios](../architecture/ios.md)).

## B

**BKTR**
The patch format of update NCAs: a sparse overlay on the base game's RomFS and ExeFS. `napple build` decodes it over the base RomFS (`crates/napple-build/src/bktr.rs`,
`crates/napple-core/src/infrastructure/nca.rs`). With `--update-romfs` the result is a full patched `romfs.bin`, a delta layer, or an in-place map.

**build id**
The 32-byte ModuleId in an NSO header (at offset 0x40). Napple uses its first 16 hex digits (lowercase) as the name of a version's data directory and as the build key in the compatibility
matrix; `knownSignatures/<titleId>/<buildId>.json` accepts 16 to 64 hex digits. Each update has its own build id. See [compat-format](../architecture/compat-format.md).

## C

**CMIF**
The command interface format of Horizon IPC: a request carries the magic `SFCI`, a command id and raw data; the reply carries `SFCO` and a result code. Plain sessions and domains are framed by
`runtime/src/ipc.c`. See also IPC and TIPC.

**CNMT**
The content meta file inside a meta NCA. It lists the contents of a title (program, control, data NCAs), its kind (application, patch, add-on content) and version. Parsed in
`crates/napple-core/src/infrastructure/cnmt.rs`; it is how `napple plan` groups base game, updates and DLC.

**compat matrix**
The table of games by pipeline stage (`ingest`, `build-plan`, `nso-load`, `site-scan`, `rewrite`, `rewrite-verify`, `runtime-boot`, `video`, `audio`, `input`, `saves`, `title-screen`, `ingame`)
with a status (`untested`, `pass`, `partial`, `fail`, `blocked`) and evidence per build. Stored in `compat/matrix.json`, rendered to `COMPATIBILITY.md`, maintained by `napple compat`
(`crates/napple-compat`). The runtime writes its stage through `--summary` ([compat-format](../architecture/compat-format.md)).

## D

**data directory**
The runtime's input: `<titleid>/<buildid16>/` with `manifest.json`, `main.npdm`, `modules/<name>.patched.bin` and `<name>.tables`, and optionally `romfs.bin`. Produced by `napple build`
(inside an app bundle, or alone with `--data-only`). Layout in [module-format](../architecture/module-format.md).

**DLC**
Downloadable add-on content, a separate title with id `...1xxx` whose RomFS is served by the `aoc:u` service (`runtime/src/services/aoc_core.c`). Selected with `--dlc` at build time.

## E

**ExeFS**
The partition of a program NCA that holds the executable modules (`main`, `rtld`, `sdk`, `subsdk0`...) and `main.npdm`. `napple build --dump-exefs` extracts it.

## F

**firmware**
The Switch system software, as a zip or directory of NCAs that the user supplies. Napple needs it only at build time, to extract the shared system fonts (`crates/napple-build/src/firmware.rs`,
`crates/napple-build/src/sysdata.rs`). The fonts are served to games by the `pl:u` service (`runtime/src/services/pl.c`). Without firmware a build succeeds and records a compatibility note.

## G

**gate**
The host-call entry that replaces a `svc` or timer-counter read in rewritten code. The rewritten block loads the gate address from the key page and calls it; the host identifies the call by
its return address in `<module>.gates.tsv` ([rewrite-format](../architecture/rewrite-format.md)). The runtime side is `runtime/src/gate.S` and the svc handlers in `runtime/src/kernel.c`.

## H

**HFS0**
The hashed file system inside XCI partitions (`update`, `normal`, `secure`, `logo`). Parsed in `crates/napple-core/src/infrastructure/xci.rs`.

**HLE (high-level emulation)**
Implementing a Horizon service or system function in the host instead of running Nintendo's code. Napple answers IPC services in `runtime/src/services/*.c`, the kernel svcs in
`runtime/src/kernel.c`, and the NVN graphics API in `gfx/`. It is not an instruction-level emulator for 64-bit titles.

**Horizon**
The Switch's operating system. Napple reimplements the parts of its kernel interface (svcs), service interface (IPC) and loader that games use.

## I

**in-place data mode**
A development mode of `napple build --data-only --update-romfs in-place`. Instead of copying a patched RomFS (many GB for large updates), it writes a `romfs.map` that lets the runtime read and decrypt the
RomFS straight from the game files (`crates/napple-build/src/romfs_map.rs`, `runtime/src/services/romfs_map.c`). The map holds content keys and host paths and must never be shipped.

**IPC**
Inter-process communication between guest and Horizon services. A guest sends a request on a session handle with `svc SendSyncRequest`; the runtime's in-process server
(`runtime/src/ipc.c`, `runtime/src/dispatch.c`) routes it to a service object. The wire formats are CMIF and TIPC.

**IVFC**
The hash tree format that wraps a RomFS inside an NCA. `napple build` reads the last (data) level and writes it as `romfs.bin` (`crates/napple-core/src/infrastructure/nca.rs`).

## K

**key page**
A 16 KiB page in each rewritten module image that the loader fills with per-thread slot offsets. Rewritten code finds the virtual `x18`, `tpidr_el0`, `tpidrro_el0` and the gate address through it
(`KEY_x_IDX` entries `K18`, `KTLS`, `KTLR`, `KGATE`). Described in [rewrite-format](../architecture/rewrite-format.md); filled by `runtime/src/module.c`.

**knownSignatures**
The directory `knownSignatures/<titleId>/<buildId>.json` of per-build knowledge: addresses, short masked instruction patterns (at most 64 bytes), names, hashes and struct layouts, never game
bytes. The validator in `crates/napple-sigs/src/validate.rs` rejects anything else. See `knownSignatures/README.md` and `napple sigs`.

## M

**Maxwell**
The GPU architecture of the Switch's Tegra X1. Games ship shaders as Maxwell machine code; `gfx/src/shader/maxwell_translate.c` translates them to MSL at run time.

**MSL (Metal Shading Language)**
Apple's GPU shader language. The translator produces MSL source which `gfx/src/mtl_shader.m` compiles into Metal pipelines and caches.

## N

**NACP**
The control data of a title (names, publisher, display version). The default app name comes from it (`crates/napple-core/src/infrastructure/nacp.rs`).

**NCA**
Nintendo Content Archive: an encrypted container of one piece of title content (program, control, data, meta). Opened with `prod.keys` and title keys in
`crates/napple-core/src/infrastructure/nca.rs`.

**NCZ**
An NCA stored with its body decrypted and zstd-compressed, as found in NSZ and XCZ files. Decoded by `crates/napple-core/src/infrastructure/ncz.rs`, which presents the original NCA bytes again.

**NPDM**
The process metadata file `main.npdm`: the service access list, allowed svcs, main thread priority, core and stack size, and the 32-bit/64-bit flag. The runtime enforces the access list in `sm`
(`runtime/src/npdm.c`); `napple compat npdm` prints it (`crates/napple-core/src/infrastructure/npdm.rs`).

**NRO**
A relocatable executable module loaded at run time by the game through the `ldr:ro` service (`runtime/src/ro.c`, `runtime/src/nro.c`). `napple build` rewrites each `.nro` in the RomFS into the
version's `nro/` directory (`crates/napple-build/src/nro.rs`).

**NSO**
The Nintendo static object: the compressed, relocatable executable format of the modules in an ExeFS (`main`, `rtld`, `sdk`, `subsdk*`). Parsed in `crates/napple-core/src/infrastructure/nso.rs`. Its header holds the build id.

**NSP**
A PFS0 package of NCAs and tickets, the usual container for an eShop-style game, update or DLC. See PFS0.

**NSZ**
A compressed NSP whose NCAs are NCZ files (XCZ for XCI). Accepted as input like NSP.

**NTB1**
The module table file `<name>.tables` for AArch64 modules: symbols, relocations and gates, with magic `NTB1`. Written by `crates/napple-build/src/tables.rs`, read by the loader in
`runtime/src/module.c`. Layout in [module-format](../architecture/module-format.md).

**NTB2**
The same table format for AArch32 modules, with magic `NTB2` and the ARM relocation kinds. Written by `crates/napple-build/src/tables32.rs`, loaded by `runtime/src/a32/a32_module.c`.

**nvdrv**
The Horizon service that fronts the Tegra drivers (`/dev/nvmap`, `/dev/nvhost-ctrl`, GPU channels). Games use it for graphics and video decode memory. Implemented in part in
`runtime/src/services/nvdrv.c`; status in [runtime-services](../architecture/runtime-services.md).

**NVN**
The Switch's low-level graphics API. Napple implements the NVN function table on Metal in `gfx/src/api_*.c` (surface in `gfx/src/nvnapi/`), command recording in `gfx/src/api_cmd.c` and replay in
`gfx/src/mtl_replay.m`.

**nvnBootstrapLoader**
The single entry point the game's graphics library uses to obtain every NVN function pointer. Napple overrides it through the HLE table (`gfx/src/core.c`, exposed by `gfx/adapter/gfx_napple.c`), so the SDK's own
NVN driver is never executed.

## P

**PFS0**
Partition file system, the flat archive format of NSP files and of ExeFS. Parsed in `crates/napple-core/src/infrastructure/pfs.rs`.

**prod.keys**
The user's key file (production keys), supplied with `-k`. Napple reads it to decrypt containers and never prints key values or stores them in the repository
(`crates/napple-core/src/domain/keys.rs`). `napple build --check` validates it.

## R

**REA**
The reverse-engineering adapter: `crates/napple-sigs/src/reverser.rs` runs the `rea` CLI over Ghidra to find function boundaries when a signature is missing. It has a hard timeout, no shell, and
its inputs come only from the work directory. The binary path can be set with `NAPPLE_REA_BIN`; see [tooling](tooling.md).

**rewriter**
`crates/napple-rewrite`: the ahead-of-time pass that replaces the instructions macOS does not allow in guest code (`x18` use, thread-pointer registers, `svc`, timer counters) with branches into a
patch area. It writes `<module>.patched.bin` plus fixups, gates and sites files ([rewrite-format](../architecture/rewrite-format.md)). `napple rewrite` runs it; `napple build` calls it in process.

**RomFS**
The read-only file system of a title's data. Games read it through the `fsp-srv` service (`runtime/src/services/fs.c`) from `romfs.bin`, a delta layer or a `romfs.map`.

## S

**site**
A location in a module that the rewriter must change, such as an `x18` access, a `svc`, a counter read or an indirect jump table. Listed in `<module>.sites.tsv`. Unhandled sites make `napple rewrite` fail.

**sm**
The service manager. A guest connects to `sm:` and asks for service names; the runtime's `sm` (in `runtime/src/ipc.c`) enforces the NPDM access list and returns a session for implemented services.
An unknown service aborts the run unless `NAPPLE_IPC_LENIENT` is set.

**Studio**
Napple Studio, the SwiftUI macOS app in `studio/`. It runs the `napple` CLI as a subprocess (`plan`, `build`) and shows a library, plan and build progress; the protocol is in
[studio-protocol](../architecture/studio-protocol.md).

**SVC (supervisor call)**
A call from guest code into the Horizon kernel (memory, threads, synchronization, IPC). In rewritten code each `svc` goes through the gate; the handlers are in `runtime/src/kernel.c` and related
files, and AArch32 guests use the adapter in `runtime/src/a32/a32_proc.c`. An unimplemented svc aborts the run unless `NAPPLE_SVC_LENIENT` is set.

## T

**TIPC**
A compact alternative to CMIF used by some services; request types start at 16 in `runtime/src/ipc.c`, which frames it next to CMIF and builds the TIPC error reply for refused `sm` requests.

**title id**
The 64-bit (16 hex digit) identifier of a title. A base game ends in `0000`, its update in `0800`, DLC in `1xxx`. Napple uses the base (application) id for every version
and keys saves, input settings, logs and the matrix by it (`~/Library/Application Support/Napple/<titleId>`).

## X

**XCI**
The game card image format: a header and HFS0 partitions (`update`, `normal`, `secure`, `logo`) holding NCAs. Parsed in `crates/napple-core/src/infrastructure/xci.rs`.
