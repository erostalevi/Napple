# iOS / iPadOS support (iOS 26 and later)

Goal: `napple build --target ios` (or Studio's "iPhone & iPad" target) turns a game you own into an app you install on **your own** iPhone or iPad with your own Apple ID.
Status: **written on Linux, never compiled or run on a Mac or an iPhone.** What is verified and what is not is listed at the end. Read "Risks" before spending an evening on it.

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

1. **Never run on a device.** Every item marked *unbuilt* below may have compile errors; the first run will find the runtime-level problems. Expect a few evenings of fixing, not zero.
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
| `frontend_ios.m`, `engine_ios.m`, `ios_platform.m`, `mach_vm_compat.h`, Makefiles/scripts | **unbuilt** |
| Studio changes | **unbuilt** (no Swift on Linux) |
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
