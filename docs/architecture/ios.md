# iOS / iPadOS support (iOS 26 and later)

Goal: `napple build --target ios` (or Studio's "iPhone & iPad" target) turns a game you own into an app you install on **your own** iPhone or iPad with your own Apple ID.
Status: **written on Linux; compiled and run on the iOS Simulator (LAS reaches its title screen, SMO its menu: see the last section); never run on an iPhone.** What is verified and what is not is listed at the end. Read "Risks" before relying on it.

## Why iOS needed more than a new target string

macOS Napple maps each rewritten module (`<module>.patched.bin`) into anonymous memory and `mprotect(PROT_EXEC)`s the text. iOS refuses that for an app without the
JIT entitlement (and Napple never uses JIT). Executable pages on iOS must come from a **code-signed Mach-O file that dyld maps**. So for iOS:

```
macOS   build:  modules/<m>.patched.bin (+ .tables)  --runtime: mmap anon, pread, relocate, mprotect RX
iOS     build:  Frameworks/<ver>.<m>.dylib (+ modules/<m>.tables)  --runtime: dlopen, relocate, mprotect only the key page
```

`crates/napple-build/src/macho.rs` wraps the unchanged image in a dylib: a 16 KiB header page, then the image at the same relative offsets, with the segments the runtime's
`seal()` would have produced (`__TEXT` r-x for the text, `__RODATA` r--, `__DATA` rw- for data + relocation targets + the key page, `__TEXT_EXEC` r-x for the rewriter's patch area).
Every offset in the NTB1 tables stays valid; the runtime uses `header + 0x4000` as the module base (`runtime/src/module.c`, `ios_dylib.c`). `codesign` signs each dylib, then the app.
Run-time modules (NROs) take the same path (`<ver>.nro.<moduleid>.dylib`). Anything that needs *new* executable memory at run time (a guest-side JIT, `CreateCodeMemory` + `ControlCodeMemory` with
execute permission) is refused with `R_INVALID_MEMORY_PERMISSIONS` and noted in the run summary.

## What changed

| Area | Files |
|---|---|
| Build (Rust, Linux-tested) | `crates/napple-build/src/{macho,ios}.rs`, `cli.rs`, `pipeline.rs` (`--target`, signing, IPA, install, dylibs) |
| Runtime (C) | `platform.h`, `mach_vm_compat.h` (vm_* for mach_vm_* on iOS), `ios_dylib.[ch]` (portable, host-tested), `module.c` (dlopen path), `memory.c` (no exec aliases), `ios_platform.[hm]` (bundle paths, fonts), `main.c`, `app.m` (no AppKit) |
| Frontend (UIKit) | `frontend/src/ios/frontend_ios.m` (same C API as the macOS frontend), `ios_logic.[ch]` (controller geometry + HID key map, host-tested), `frontend/Makefile.ios` |
| Input | `napple_input_virtual_pad()` : on-screen controller with analog sticks (tested on Linux) |
| Audio | `engine.c`: RemoteIO instead of the HAL on iOS; `engine_ios.m`: AVAudioSession + interruption recovery |
| gfx | `mtl_core.m`: `displaySyncEnabled` is macOS-only. The rest was already iOS-aware (BC fallback to RGBA8, no Depth24, ASTC) |
| Tools | `tools/build_ios.sh`, `tools/build_ios_deps.sh` (libopus, optional libvpx for iPhoneOS) |
| Studio | target picker, signing identity/profile pickers, device list, install via `devicectl`, `.ipa`, unsigned mode |

## Bundle layout (flat)

```
<Name>.app/  Info.plist  PkgInfo  embedded.mobileprovision  AppIcon60x60@2x.png ...
  napple-runtime            CFBundleExecutable; there is no launcher, iOS cannot exec
  Frameworks/<buildid16>.<module>.dylib   rewritten modules (signed)
  game/                     as macOS Resources/game (napple-app.json, <buildid16>/{manifest.json,main.npdm,romfs.bin,modules/*.tables,nro/}, dlc/, system/fonts)
```
Info.plist: iOS/iPadOS 26.0 minimum, landscape only, UIScene life cycle (`NappleSceneDelegate`), `UIFileSharingEnabled` (saves and screenshots appear in Files > Napple),
`CADisableMinimumFrameDurationOnPhone` (120 Hz), `GCSupportsControllerUserInteraction`. Saves/settings: `Documents/Napple/<titleid>/` (the macOS runtime's `~/Library/Application Support/Napple`).

## Getting an app onto your phone (all local, from a Mac)

1. Build the iOS runtime once: `tools/build_ios_deps.sh && tools/build_ios.sh` -> `runtime/build-ios/napple-runtime` (Xcode + cmake). Rebuild Studio (`studio/build_app.sh`) so it bundles it as `napple-runtime-ios`.
2. Signing needs two things from Xcode (an Apple ID is enough, no paid account):
   - a certificate: Xcode > Settings > Accounts > your Apple ID > Manage Certificates > + Apple Development;
   - a provisioning profile for an **explicit bundle id** that includes your device: make a throwaway iOS App project in Xcode with that bundle id (for example `com.<you>.napple1`),
     select your phone, Run once. Xcode registers the id and the device and stores the profile in `~/Library/Developer/Xcode/UserData/Provisioning Profiles`.
     Free accounts allow 3 app ids per 7 days and profiles last 7 days: re-sign (rebuild) weekly. A paid account can use a wildcard profile (`TEAM.*`) and gets the memory entitlements.
3. On the phone: Settings > Privacy & Security > Developer Mode on (restart), trust the computer when asked.
4. Studio: drop the game, Target = iPhone & iPad (or iPhone / iPad), pick identity and profile (the bundle id fills in from the profile), Build. Studio installs it with `devicectl` and launches it.
   CLI equivalent:
   `napple build game.nsp -k prod.keys --firmware fw.zip --target ios --signing-identity <sha1> --provisioning-profile <file> --bundle-id com.you.napple1 --ipa --install <device> --launch -o out`
5. First launch of a free-account app: Settings > General > VPN & Device Management > trust your developer certificate.

`--no-sign` builds an unsigned app (and `.ipa` with `--ipa`) for AltStore, Sideloadly or `rcodesign`. The dylibs must be signed with the same identity as the app.

## Controls

Touch screen: on-screen Switch controller (analog sticks, D-pad, ABXY with the Switch layout, L/R/ZL/ZR, +/-), gear button for the settings sheet (video, audio, controls, speed, DLC/update, screenshot, quit),
touches outside controls act as the Switch touch screen. It hides while a physical controller (MFi, DualSense, Xbox, Joy-Con) is connected; Home/Guide opens the settings sheet.
iPad keyboards work through the normal key bindings; F1 settings, F2/F3 turbo, F4 slow motion, F5/F6 speed, F7 reset, F9 statistics, F12 screenshot.

## Risks and known limits (read these)

1. **Never run on a device.** Only the Simulator has run this code; device signing, execute-page rules and memory limits are unverified, so expect problems on the first device run.
2. **Hand-written Mach-O.** The dylib is produced by `macho.rs`, not by `ld`. It parses with `llvm-objdump` and its layout is unit tested, but dyld on iOS 26 may reject something (the executable `__TEXT_EXEC`
   segment name, the empty `LC_DYLD_INFO_ONLY`, no `LC_FUNCTION_STARTS`). The runtime prints dyld's message (`dlerror()`). Check on a Mac with `otool -l Frameworks/*.dylib` and `codesign -dvv`. If dyld insists on
   `__TEXT` for executable pages, the fallback is to link the module with Apple's `ld` from an assembly file that `.incbin`s the image (not implemented).
3. **Memory.** The guest wants several GB. Default iOS per-app limits are about half of RAM on recent phones; the runtime reserves ~10 GiB of address space (PROT_NONE, free) and commits what the game uses.
   The `increased-memory-limit` and `extended-virtual-addressing` entitlements need a paid account; with a free Apple ID big games can be jetsam-killed. iPads with 16 GB are the best bet.
4. **Performance**: the GPU path is the same NVN->Metal; thermals and the lack of the game-specific fixes that exist on macOS are untested. Texture formats BC1-BC7 are decoded on the CPU when the GPU lacks BC (existing `napple_bcdec`); start with a lower render scale.
5. **No JIT-dependent features** (see above). **No App Store / TestFlight** path: personal use, same legal position as the macOS app.
6. **Debugger**: attaching Xcode's debugger changes how SIGSEGV based memory tracking behaves; run without a debugger (`devicectl process launch`), read logs via Console or `--console`.
7. Free profiles expire after 7 days; the build prints the expiry as a note.

## Verification status

| Item | Status |
|---|---|
| Mach-O writer: layout, alignment, load commands, `llvm-objdump` parse | tested on Linux (`cargo test -p napple-build`) |
| `--target ios` bundle assembly up to the dylib step, CLI parsing, plist, profile matching, early signing check | tested on Linux (synthetic fixtures; real signing/IPA/install code paths use macOS tools and are unrun) |
| Runtime `ios_dylib.c` naming + Mach-O check | tested on Linux (`make -C runtime test-host`); `module.c` iOS branch syntax-checked with stub headers only |
| `napple_input_virtual_pad` | tested on Linux |
| Controller geometry on 4 device sizes, HID->mac key map | tested on Linux (`make -C frontend -f Makefile.ios test-host`) |
| `frontend_ios.m`, `engine_ios.m`, `ios_platform.m`, `mach_vm_compat.h`, Makefiles/scripts | written on Linux; built and run on the iOS Simulator (section below) |
| Studio changes | written without Swift on Linux; the Studio package is built and tested on the Mac (`swift test`) |
| Anything on a real iPhone/iPad | **never run** |

## First-run checklist (on the Mac)

1. `cargo test -p napple-build && make -C runtime test-host && make -C input test && make -C frontend -f Makefile.ios test-host`
2. `tools/build_ios_deps.sh && tools/build_ios.sh` and fix compile errors (expect some in `frontend_ios.m`; `mach_vm_compat.h` if a vm_* signature differs).
3. Build a small game with `--no-sign`, inspect `otool -l Frameworks/<ver>.main.dylib` (segments, platform IOS), then `codesign -f -s <id> --timestamp=none` it and `codesign -dvv`.
4. Install, launch from the phone; read the runtime log (`devicectl device process launch --console ...`). First failure points to look at: `cannot load ... dlerror`, `cannot reserve the guest address space`, jetsam.

## iOS Simulator smoke test (verified 2026-10-24, Xcode 27.1 SDK, iOS 26.1 runtime, Apple silicon Mac)

```
SDK=iphonesimulator tools/build_ios_deps.sh && SDK=iphonesimulator tools/build_ios.sh      # -> runtime/build-ios-sim/napple-runtime
napple build <game> -k prod.keys --firmware fw.zip --target ios-sim --runtime runtime/build-ios-sim/napple-runtime -o out
xcrun simctl install booted out/<Name>.app && xcrun simctl launch --console-pty booted com.napple.game.<titleid>
```
`--target ios-sim` writes LC_BUILD_VERSION platform 7 (IOSSIMULATOR), `CFBundleSupportedPlatforms = iPhoneSimulator`, signs ad hoc (dylibs, then the app), needs no profile.
Result with Super Mario Odyssey: dyld accepts the hand-written dylibs as they are (`__TEXT_EXEC`, empty `LC_DYLD_INFO_ONLY`, no `LC_FUNCTION_STARTS`), the guest boots, reaches `nn::init::Start` and the game's menu renders with the touch controller.
**Not validated by the Simulator:** it runs arm64 code as a macOS process, so device code-signing, AMFI/exec-page rules and the real memory limits are untested; the device path is still unverified.
Simulator-only workarounds: no MetalFX in the Xcode 27 Simulator SDK; `addPresentedHandler:` / `presentDrawable:afterMinimumDuration:` missing from its headers (the former also at run time); Metal binary archive disabled; the Simulator's phantom "Gamepad (MFi)" does not hide the touch controller.

## Simulator run of the four titles (2026-10-25, branch claude/ios-sim-2026-10-25-titles; Xcode 27.1, iPhone 17 / iOS 26.1 Simulator)

Real libopus and libvpx (v1.14.1) built for the Simulator (`SDK=iphonesimulator VPX=1 tools/build_ios_deps.sh`; the script's libvpx step now uses `--target=arm64-linux-gcc` with the Simulator clang because libvpx's darwin targets force the macOS SDK), then `SDK=iphonesimulator tools/build_ios.sh`, `napple build ... --target ios-sim`, `simctl install/launch`, screenshots through the Simulator tool. Everything below was driven by hand through the touch controller or unattended; none of it was "played".

| Title | Result on the Simulator |
|---|---|
| LAS (01006BB00C6F0000, base v0) | **boots to the title screen** ("Press L+R"); intro movie (VP9 via the iOS libvpx) and audio (RemoteIO) run; touch controller drawn. Screenshots: shared notes folder `ios-sim-las-intro-*.png`, `ios-sim-las-title.png` |
| SMO (0100000000010000, base v0) | reaches the Start / Assist Mode menu at about 75 s (audio 0 underruns). A tap on the touch A button did not visibly advance the menu (not investigated; the panel shows the landscape app in portrait, tap coordinates unverified) |
| MK8D, `--updates latest` | the latest update is an **AArch64** build (id fe941ed5ba14be5d): stops at 1.1 s with `IPC command time:u:300 not implemented` (services, same on macOS data of that update; not iOS) |
| MK8D, base v0 (AArch32 interpreter) | boots through the A32 loader, runs about 15 s of interpreted code (`nnMain still running ... 1600 M instructions`), then aborts in gfx: `Render Pipeline Descriptor Validation: This set of render targets requires 48 bytes of pixel storage. This device supports 32 bytes.` The **Simulator GPU** allows 32 bytes of colour attachment per pixel; Apple A11+ allows 64, so a device would likely pass this (unverified). gfx work: split MK8D's G-buffer pass or fall back on the Simulator |
| ACNH 3.0.3 | see the ACNH line in `docs/archive/changes/2026-10-25-ios-sim-titles.md` |

What was broken and is fixed on this branch (all found by the first real Simulator run):
1. `tools/build_ios.sh` did not compile `runtime/src/a32/*.c` (link failed with `a32_boot` and friends).
2. Depth/stencil textures were created with `MTLStorageModeShared`, which iOS and the Simulator refuse (assertion in `mtl_tex.m`): Private on iOS (`gfx/src/mtl_tex.m`, `TARGET_OS_IOS`).
3. A 424 MB guest memory pool exceeds the Simulator's `maxBufferLength` (256 MiB); `gfx_mtl_pool_add` now wraps big pools as overlapping Metal buffers (`mtl_core.m`; 32 MiB overlap, `mtl_addr` already scans neighbours). Real iPhones report larger limits but may also be below the pool size.
4. Without libvpx the guest's own movie decoder runs and calls the unimplemented NVN function `nvnTextureGetNvRmSurfaceNVX` (SIGSEGV): libvpx for the Simulator fixes it.
5. LAS aborts in its movie library with the 3.25 GiB heap cap used for the 6 GiB device window: the Simulator build uses the macOS 4 GiB cap (`memory.c`, `TARGET_OS_SIMULATOR`). **Open for devices**: LAS probably needs more than 3.25 GiB of guest heap, so a non-entitled iPhone (6 GiB window) may not run it; a smaller `low_gap_v` (1 GiB tail) could free the 0.75 GiB but is unverified.
6. `napple build --target ios*` died on AArch32 titles (`not an NTB1 table file`): the interpreter reads NTB2 images as data, so `dylibize` now skips them.
7. `--ipa`: `ditto` stored AppleDouble `._*` files in the IPA when the bundle files carry extended attributes; it now passes `--norsrc --noextattr --noacl`.

IPA check (`napple build LAS --target ios --no-sign --ipa`, iPhoneOS runtime built with `SDK=iphoneos`): `Payload/<Name>.app` with `napple-runtime` (platform 2, minos 26.0), `Frameworks/*.dylib` (4, platform 2), `game/`, `Info.plist` (`CFBundleSupportedPlatforms = iPhoneOS`, landscape, scene manifest), icons; 3.99 GB compressed, 169 files; `codesign -dv` says "not signed at all" as intended. Signing/installing on a device stays unverified.

Not proven by the Simulator (still open): signing and AMFI/exec-page rules for the dylibs on a device, the real memory limits (jetsam, the 6 GiB window, `maxBufferLength` on A-series), dyld on a phone, Files app integration, thermals/performance, MFi controllers (the Simulator has a phantom "Gamepad (MFi)"; once the settings sheet opened by itself right after launch, not reproduced), the colour-attachment budget of real GPUs.

### Follow-up notes (2026-10-26)

- **Touch input on the Simulator is unverified.** `simctl`/tool taps and `touch_path`/`touch2_path` never reached the app (no `hitTest` call, main thread idle), so the on-screen pad has not been exercised; only `NAPPLE_INPUT_SCRIPT` (keyboard bindings path, `SIMCTL_CHILD_NAPPLE_INPUT_SCRIPT=...`) was used. With it LAS goes title -> "Please select a file" (File 1-3 New Game). Beware zsh: `"$t:A"` expands as a path modifier, write `"${t}:A"`. `NAPPLE_INPUT_LOG=1` now also traces pad touches and virtual-pad pushes.
- **Time:u command 300** (`CalculateMonotonicSystemClockBaseTimePoint`) is implemented (returns the clock context offset, unverified against hardware); needed by the MK8D 3.x (AArch64) build.
- **Memory budget on a phone (from nxnative's notes):** without `extended-virtual-addressing` a single 6 GB reservation succeeds but Metal needs address space too; 5 GB total is the proven figure on a phone. Our device layout reserves 6 GiB (0.75 stack/gaps + 3.25 heap + 1 gap + 1 alias), so the device build may fail at Metal setup, and LAS needs more than the 3.25 GiB heap. Candidate layout to try on a device: drop the 1 GiB tail gap (0.75 + 3.25 + 1 = 5 GiB), and give LAS a profile-sized pool. Not applied: no device and the purpose of `low_gap_v` is unverified.
- **Not measured:** RSS of the four titles on the Simulator (game data under `work/games` was removed by another session for disk space; rebuilding needs 6-18 GB each, disk had 2.6 GB free). Studio `swift test` and per-title IPA sizes (only LAS: 3.99 GB compressed) not run for the same reason.
- nxnative rows not applicable to my paths: G10/G13/G19 (gfx: present hardening, iOS relaxed math, same-pass feedback snapshot) are needs for the gfx owners; T2 (CI with an iOS compile smoke, `nxnative/dm/ios/tools/build_smoke.py`) is a CI item; T5 (iOS build/launcher settings table) is only partially useful: `tools/build_ios.sh` could take a per-title settings file.

## Simulator re-run on integrated main (2026-10-26, main after the 2026-10-26-nx batch; iPhone 17 Simulator, iOS 27.1)

`SDK=iphonesimulator VPX=1 tools/build_ios_deps.sh && SDK=iphonesimulator tools/build_ios.sh` (needed the a32jit.c fix in `build_ios.sh`), `napple build --target ios-sim` (LAS, SMO full RomFS; ACNH 3.0.3 + 3 DLC and MK8D 3.0.5 + DLC with `--update-romfs delta`, about 11 GB each), `simctl install`, `SIMCTL_CHILD_NAPPLE_INPUT_SCRIPT` for A presses, screenshots through the Simulator tool. Unattended or scripted only.

| Title | Where it stops |
|---|---|
| LAS | intro movie at 61 fps (~66 s), **title screen** "Press L+R" at ~95 s, 55 fps; scripted A at 75/82/88 s did not leave the title (the title wants L+R). No abort |
| SMO | **Start / Assist Mode menu** at ~55 s; scripted A at 60 s -> abort: `This set of render targets requires 48 bytes of pixel storage. This device supports 32 bytes.` (build_pipe) |
| ACNH 3.0.3 + DLC | abort at ~2.3 s, same 48-byte colour-attachment assertion, before any frame was shown (macOS shows the Nook Inc. scene) |
| MK8D 3.0.5 + DLC (AArch64) | past `time:u:300` now; abort at ~2.3 s with the same 48-byte assertion |

All three aborts are the Simulator GPU's 32-byte colour-attachment budget (A11+ devices allow 64; unverified on a device). One gfx fix (split or fall back on wide MRT sets when the device limit is 32) would likely unblock SMO past its menu, ACNH and MK8D on the Simulator.
