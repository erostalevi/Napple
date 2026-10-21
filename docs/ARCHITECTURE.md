# Napple: architecture and technical plan (draft v0)

Status: proposal, nothing implemented yet. Written after reading both reference projects:

| Reference | Approach | Lesson |
|---|---|---|
| `LASRecomp` | Lift ARM64 -> C (`tools/lifter.py`, `gen.py`), compile locally with Zig, HLE kernel + NVN->Metal/Vulkan | Per-game lifter and many per-game HLE hooks (`runtime/hle/*`). Hard to be game-agnostic. |
| `MacNativeGames` | Switch and Apple Silicon are both ARM64: run the game's own code natively, rewrite only what macOS forbids (x18, TLS reg, `svc`, counter regs) ahead of time (`patch_nso.py`); HLE Horizon services; NVN->Metal 4 | Far more generic. Per-game cost is shrunk to patch tables and addresses. |

## 1. Core decision (needs confirmation)

**Recommended: "native A64 + ahead-of-time rewrite", not C lifting.**

- No lifter means no per-game lifting bugs; any ARM64 NSO is handled by the same pipeline.
- Works on iOS/tvOS later: no JIT is needed because rewriting happens ahead of time (build or first launch), not at run time.
- "Decompile/recompile" in the user-facing sense is kept: Napple outputs a standalone, locally signed `.app`.
- Static C lifting stays optional, only for games that need it, behind the same interface.

## 2. Pipeline

```
 user files: game (XCI/NSP/NSZ) + prod.keys + firmware
   -> [1 ingest]   container parse, NCA decrypt (user's keys), ExeFS/RomFS, version/hash ID
   -> [2 analyze]  NSO load, symbol/function discovery, find patch sites
   -> [3 resolve]  signature lookup: knownSignatures -> REA fallback -> optional share
   -> [4 rewrite]  AArch64 rewriter (x18, TPIDR, svc, counters, 4K->16K page issues)
   -> [5 package]  .app = runtime + per-game patch table (no game bytes), local ad-hoc sign
   -> [run]        loader + Horizon HLE + NVN->Metal + input/save/settings
```

## 3. Module layout (hexagonal / DDD)

```
domain/          pure rules: GameId, Signature, PatchSite, AuthBinding, no I/O
application/     use cases: IngestGame, AnalyzeBinary, ResolveSignatures,
                 BuildApp, AuthorizeSystem, ShareSignature
infrastructure/  adapters: containers/NCA crypto, A64 disassembler, rewriter,
                 git/GitHub client, Keychain, codesign
interfaces/      CLI (`napple build`), launcher UI, runtime entrypoints
runtime/         Horizon HLE (services), NVN->Metal, audio, input, FS
```

Business rules never import adapters. Ports (interfaces) for: `KeyStore`, `SignatureRepository`,
`BinaryAnalyzer`, `Reverser` (REA), `Signer`, `ContainerReader`.

## 4. Primitive-by-primitive plan

1. **Own the game.** Input is the user's encrypted XCI/NSP plus `prod.keys`; nothing is downloaded or bundled.
2. **Keys/firmware.** `ContainerReader` validates keys by decrypting known headers; firmware only for system data (shared fonts).
3. **Any game.** Per-game knowledge lives only in data (signatures, patch tables), never in code branches. Honest limit: HLE service and GPU coverage is a long tail; the first games will each expose missing services/NVN features. Plan for a compatibility matrix.
4. **Full functionality** except Nintendo-specific/online (NIM, friends, NEX, etc.: stubbed to "offline").
5. **Keys only at build time (revised by the owner).** Keys, firmware and the game are needed only to decompile/recompile.
   The output app is self-contained and runs anywhere, with no re-checks and no per-system auth (this also keeps iOS simple).
   Consequence: the app contains game-derived content, so it is for the builder's personal use only. Napple does not
   condone sharing; Studio and the README say so, and nothing game-derived is ever committed or uploaded. The
   earlier per-system auth design is dropped.
6. **Game-agnostic** means the runtime exposes one interface and per-game data is the only input (section 5).
7. **Thin customization layer:** saves in macOS FS (`~/Library/Application Support/Napple/<titleId>`), controller mapping (GameController/SDL), keyboard/mouse, graphics settings (resolution, framerate, frame pacing fixes to prevent speed-ups tied to frame rate).
8. **Shared signatures** (section 6).
10. **Builder UI (Napple Studio).** A simple, compact macOS app (SwiftUI over the Rust core via FFI):
    - First launch: asks for keys + firmware (stored in Keychain/app support, replaceable later in Settings).
    - After that: drop any game file (XCI/NSP, or a folder) onto the window; it starts the pipeline with progress and logs.
    - Output target selector: **macOS, iPhone, iPad** (these three for now; Apple TV later).
    - Install tools: macOS app to `/Applications`; iPhone/iPad via connected device (Xcode `devicectl`/`ios-deploy`-style
      install, local signing with the user's Apple ID/team), with clear errors for signing/trust steps.
11. **Updates and DLC (important).** A build takes a base game plus any number of Update and DLC (AddOnContent)
    packs, in any container (XCI/NSP/NSZ), dropped together or one at a time:
    - Packs are grouped by title id (base `...0000`, update `...0800`, DLC `...1xxx`) and checked against the base
      (same application id, version ordering, required-version fields in the CNMT meta NCA).
    - Update: BKTR (patch) sections are decoded over the base RomFS (relocation + subsection tables, AES-CTR-EX), and
      the update's ExeFS replaces the base's. The newest selected version wins; the user can pick an older one.
    - DLC: each pack's data NCA RomFS is mounted into the runtime's content-path space for `nn::fs` add-on content.
    - Studio shows the detected base/update/DLC set with versions and lets the user include/exclude each pack.
    - Per-version signatures: `knownSignatures/<titleId>/<buildId>.json` already keys by build id, so each update
      resolves its own signatures (build id comes from the NSO header).
12. **Runtime DLC toggle and version picker (no recompile).** The in-app launcher/settings menu (reachable at app
    start and from the in-game overlay) offers:
    - A per-DLC enable/disable switch.
    - An update version picker (base, and every update version included at build time).
    - How: the build embeds *all* selected versions. Each version has its own rewritten ExeFS image and patch table
      (code differs per update), while RomFS is stored once as the base plus per-update BKTR deltas and per-DLC
      layers, so shared data is not duplicated. The runtime composes the chosen layers when it boots the guest.
    - Switching takes effect by restarting the guest inside the app (a quick reboot of the guest, not a rebuild;
      mid-play switching is not possible because the game reads add-on content and code at boot).
    - Per-version save slots by default or shared with a warning, since saves from a newer version can be unreadable
      by an older one; the picker shows the warning and keeps a backup before downgrades.
    - Trade-off: more versions means a bigger app, so Studio shows the size cost of each included version before building.
9. **Platform agnostic:** keep `runtime/` free of AppKit; platform glue in a thin layer so iOS/tvOS targets reuse it. macOS first.

## 5. Per-game data

```
knownSignatures/<titleId>/<buildId>.json   # per binary build
```

Never contains game bytes: only addresses, short instruction-pattern masks, hashes, names, struct layouts.

## 6. Signature sharing flow

1. For each unresolved item (function, pointer, struct, patch site) query `knownSignatures` by pattern hash.
2. Miss: run REA to solve it, and validate it (e.g. emulate the rewritten site against the original with random inputs, as `MacNativeGames` does).
3. Validated new signature: ask the user whether to share. On yes, push to an anonymous fork and open a PR (no user identity in commit metadata, review for accidental game content before submission).
4. Default is local only. No network access without consent.

## 7. Security and compliance notes

- Keys: memory only or Keychain, never in argv, logs or the repo. Temp files 0600 and removed on exit.
- Container parsing is untrusted input: strict bounds, size limits, no path traversal on extraction.
- The signature PR flow must strip any byte sequences longer than the minimum needed pattern.
- Each change to this repo gets its own `.md` in `docs/changes/`.

## 8. Milestones

1. Ingest: XCI/NSP/NSZ + keys -> ExeFS/RomFS (done for NSP base; real-game validated).
1b. Updates and DLC: multi-pack input, CNMT parsing, BKTR RomFS patching, update ExeFS, DLC mounting, NSZ
   (zstd) decompression. Needs a real update and a real DLC pack to validate; synthetic fixtures only until then.
2. NSO loader + site scanner (done, see docs/changes/2026-10-20-decomp-plan.md); rewriter with per-site verification, signature schema + repository client.
3. Horizon HLE minimum (threads, memory, FS, time) to reach `nn::init::Start` on the first test game.
4. NVN->Metal, audio, input.
5. Packaging and local signing (macOS, iOS AOT).
6. REA integration and the share flow.
7. Second/third game: measure what generalizes; iOS target.

## 8b. Studio UI milestone
Between milestones 5 and 6: Studio shell (keys/firmware setup, drag-and-drop, target picker, progress, device install).
The CLI remains the source of truth; the UI only calls the same use cases.

## 9. Decisions on the open questions

- **Core approach:** native A64 + ahead-of-time rewrite (confirmed by the owner).
- **Languages (performance-first, decided with the owner):**
  - *Decomp/build time:* **Rust** (Cargo workspace `crates/`): ingest, NCA crypto, NSO, analysis, A64 rewriter,
    signature DB, CLI. Chosen because inputs are untrusted multi-GB files (memory safety), the rewriter scans
    millions of instructions (speed, parallelism with rayon), and it ships as one static binary. Also builds for
    iOS/tvOS as a static library later.
  - *Runtime (inside the .app):* **Rust** for loader, guest memory, Horizon HLE and audio; **AArch64 assembly** for
    the svc/TLS/counter stubs and guest<->host trampolines (no overhead on the hot path: guest code runs natively);
    **Objective-C++ / Metal 4 (+ Metal Shading Language)** for the NVN->Metal backend and shader pipeline;
    **Swift/SwiftUI** only for the launcher, settings, controller mapping UI. Python is not used.
  - Rust crates stay free of AppKit; platform glue lives behind traits so iOS/tvOS reuse the same core.
- **REA** = [morluto/rea](https://github.com/morluto/rea) (MIT, npm `rea-agents`, TypeScript): an MCP server and CLI
  (`rea ... --json`) that drives Hopper, Ghidra or IDA. It is a build-time/analysis dependency, never shipped in the
  `.app`. It sits behind the `Reverser` port: `ReaCliReverser` runs the `rea` CLI with JSON output, with a hard
  timeout, no shell, and file inputs only from Napple's work dir. Needs Node + a native provider (Ghidra is free).
  `npx rea-agents setup` edits agent configs, so it is the owner's call, not run automatically.
- **Code reuse:** both references are MIT, so reuse is legal with attribution (`THIRD_PARTY_NOTICES.md`). Policy:
  reuse ideas, hash manifests and verification strategies; write Napple's code against the hexagonal ports rather
  than copying game-specific modules.
- **First test game / firmware:** pending; the owner will supply games.

## 9b. Decisions from the owner's questionnaire

- **iOS signing/install:** both: direct install with the user's Apple ID (local signing) and IPA export.
- **No JIT, ever.** Ahead-of-time rewrite only. Memory: request the increased-memory entitlement and show a
  per-game compatibility warning on iPhone/iPad.
- **Studio stack:** SwiftUI + Rust core over a C ABI.
- **Keys/firmware storage:** copied into app support (not just referenced), listed in Studio with one-click delete,
  and multiple key/firmware sets can coexist; the user picks the set at the start of each build. Files 0600, never logged.
- **Signature sharing:** fork + PR to `erostalevi/Napple` via `gh`/GitHub API, per-batch consent, with a preview of exactly what is sent.
- **First target definition of done:** Link's Awakening boots on macOS and iPad.
- **Baseline:** macOS 26+, Metal 4, M1 or newer; one graphics path.

## 10. Milestone 1 scope (ingest)

Implemented in `crates/napple-core` (see `docs/changes/2026-10-20-ingest.md`). Supported: XCI (HFS0), NSP (PFS0), NCA3 with AES-XTS header, AES-CTR sections and rights-id (titlekey) crypto,
PFS0/ExeFS, RomFS (IVFC) read. Keys are used directly when present and otherwise derived from the source keys / `master_key_XX`
(any firmware generation, as long as the keys are valid). Moved to milestone 1b: NSZ, BKTR (update patches), DLC. Not yet: NCA0/2, personalized
tickets, XTS sections, IVFC full-tree hash verification.
