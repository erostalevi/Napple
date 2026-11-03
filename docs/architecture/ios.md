# iOS / iPadOS support (iOS 26 and later)

Goal: `napple build --target ios` (or Studio's "iPhone & iPad" target) turns a game you own into an app you install on **your own** iPhone or iPad with your own Apple ID.
Status: builds for the device and for the iOS Simulator; the Simulator runs the runtime, gfx, audio and frontend end to end. **Never run on a physical iPhone or iPad yet.** What is verified and what is not is listed in "Verification status". Read "Risks" before relying on it.

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

## Where the iOS code lives

| Area | Files |
|---|---|
| Build (Rust) | `crates/napple-build/src/{macho,ios}.rs`, `cli.rs`, `pipeline.rs` (`--target`, signing, IPA, install, dylibs) |
| Runtime (C) | `platform.h`, `mach_vm_compat.h` (vm_* for mach_vm_* on iOS), `ios_dylib.[ch]` (portable, host-tested), `module.c` (dlopen path), `memory.c` (no exec aliases), `ios_platform.[hm]` (bundle paths, fonts), `main.c`, `app.m` (no AppKit) |
| Frontend (UIKit) | `frontend/src/ios/frontend_ios.m` (same C API as the macOS frontend), `ios_logic.[ch]` (controller geometry + HID key map, host-tested), `frontend/Makefile.ios` |
| Input | `napple_input_virtual_pad()` : on-screen controller with analog sticks |
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
4. **Performance**: the GPU path is the same NVN->Metal; thermals are untested. Texture formats BC1-BC7 are decoded on the CPU when the GPU lacks BC (existing `napple_bcdec`); start with a lower render scale.
5. **No JIT-dependent features** (see above). **No App Store / TestFlight** path: personal use, same legal position as the macOS app.
6. **Debugger**: attaching Xcode's debugger changes how SIGSEGV based memory tracking behaves; run without a debugger (`devicectl process launch`), read logs via Console or `--console`.
7. Free profiles expire after 7 days; the build prints the expiry as a note.

## Memory layout and memory log

The device build reserves a 6 GiB guest window: 0.75 GiB stack and gaps, a 3.25 GiB heap cap, a 1 GiB tail gap (`low_gap_v` in `runtime/src/memory.c`) and a 1 GiB alias region. The Simulator build uses the macOS 4 GiB heap cap (`TARGET_OS_SIMULATOR`).
Without `extended-virtual-addressing` a single 6 GB reservation succeeds on a phone, but Metal needs address space too; about 5 GB in total is the figure known to work on a phone. A candidate device layout drops the 1 GiB tail gap (0.75 + 3.25 + 1 = 5 GiB); it is not applied because no device run has confirmed it and the purpose of `low_gap_v` is unverified. A title that needs more than 3.25 GiB of guest heap may not run on a non-entitled device.

**Jetsam and the swap.** The footprint (what iOS kills for) is mostly dirty anonymous guest pages. `NAPPLE_IOS_SWAP=1` backs them with unlinked temp files, which the kernel does not count in the footprint, and `runtime/src/mempressure.m` trims on memory warnings. Measurements, policy and the device test procedure: [memory-swap.md](memory-swap.md).

| Variable | Effect |
|---|---|
| `NAPPLE_HEAP_CAP_MB=<MB>` | try another guest heap cap; `NAPPLE_HEAP_CAP_MB=3328 NAPPLE_ALIAS_GIB=1` reproduces the 6 GiB device window on the Simulator |
| `NAPPLE_ALIAS_GIB=<1..64>` | size of the alias region (see [environment variables](../reference/environment-variables.md)) |
| `NAPPLE_MEMLOG=1`, `NAPPLE_MEMLOG_SECS=<s>` | memory log (`runtime/src/memlog.m`): on by default on iOS, every 20 s one `memlog:` line with `phys_footprint` (what jetsam judges), its peak, guest-mapped bytes, `MTLDevice.currentAllocatedSize` (0 on the Simulator) and, on iOS, `os_proc_available_memory()` |

The footprint is dominated by the guest pages the title touches; the reserved but untouched address space does not count. The Simulator keeps Metal textures outside the process, so on a device add the GPU share that `memlog` prints.
iPhone and iPad default to handheld mode (720p) through the input binding default (`input/src/bindings.c`): a docked 1080p render target costs GPU memory and heat. A saved `input.json` or `NAPPLE_OP_MODE=docked` overrides it.

## Verification status

| Item | Status |
|---|---|
| Mach-O writer: layout, alignment, load commands, `llvm-objdump` parse | tested (`cargo test -p napple-build`) |
| `--target ios` bundle assembly, CLI parsing, plist, profile matching, early signing check | tested on synthetic fixtures; device signing and install are unrun |
| Runtime `ios_dylib.c` naming + Mach-O check | tested (`make -C runtime test-host`) |
| `napple_input_virtual_pad`, controller geometry on 4 device sizes, HID->mac key map | tested (`make -C input test`, `make -C frontend -f Makefile.ios test-host`) |
| `frontend_ios.m`, `engine_ios.m`, `ios_platform.m`, `mach_vm_compat.h`, build scripts | built and run on the iOS Simulator |
| dyld loading of the hand-written dylibs (`__TEXT_EXEC`, empty `LC_DYLD_INFO_ONLY`, no `LC_FUNCTION_STARTS`) | accepted on the Simulator |
| Unsigned IPA for the device (`--target ios --no-sign --ipa`) | built and inspected: `Payload/<Name>.app` with `napple-runtime` (platform 2, minos 26.0), `Frameworks/*.dylib` (platform 2), `game/`, `Info.plist` (`CFBundleSupportedPlatforms = iPhoneOS`), icons; `codesign -dv` says "not signed at all" as intended |
| Touch input on the Simulator | unverified: `simctl` and tool taps never reach the app, so only scripted keys (`SIMCTL_CHILD_NAPPLE_INPUT_SCRIPT=...`) were used; `NAPPLE_INPUT_LOG=1` traces pad touches and virtual-pad pushes |
| Anything on a real iPhone/iPad | **never run** |

Per-title Simulator results (where each title stops, footprint, fps) are in the [title notes](../titles/README.md).

## Simulator

```
SDK=iphonesimulator VPX=1 tools/build_ios_deps.sh && SDK=iphonesimulator tools/build_ios.sh   # -> runtime/build-ios-sim/napple-runtime
napple build <game> -k prod.keys --firmware fw.zip --target ios-sim --runtime runtime/build-ios-sim/napple-runtime -o out
xcrun simctl install booted out/<Name>.app && xcrun simctl launch --console-pty booted com.napple.game.<titleid>
```
`--target ios-sim` writes LC_BUILD_VERSION platform 7 (IOSSIMULATOR), `CFBundleSupportedPlatforms = iPhoneSimulator`, signs ad hoc (dylibs, then the app) and needs no profile.
`VPX=1` builds libvpx (the script uses `--target=arm64-linux-gcc` with the Simulator clang, because libvpx's darwin targets force the macOS SDK). Without libvpx a title's own movie decoder runs and calls the unimplemented `nvnTextureGetNvRmSurfaceNVX`.
Environment variables reach the app as `SIMCTL_CHILD_<NAME>`. In zsh write `"${t}:A"`, not `"$t:A"` (a path modifier).

The Simulator is not a device: it runs arm64 code as a macOS process, so code signing, AMFI / exec-page rules and the real memory limits are untested there. Its GPU is also more limited:

| Simulator limit | Effect | On a device |
|---|---|---|
| 32 bytes of colour attachment per pixel | a render target set needing 48 bytes aborts in `build_pipe` (`This set of render targets requires 48 bytes of pixel storage`) | A11 and later allow 64 (unverified) |
| no cube-array textures | `Texture Cube Array is not supported on this device` in `mtl_tex_get` | A-series GPUs support them (unverified) |
| `maxBufferLength` 256 MiB | big guest memory pools are wrapped as overlapping Metal buffers (`gfx_mtl_pool_add`, 32 MiB overlap) | larger, but may still be below a pool's size |
| no MetalFX, no `addPresentedHandler:` / `presentDrawable:afterMinimumDuration:` | upscaler falls back to the built-in one; binary archive disabled | available |
| a phantom "Gamepad (MFi)" | does not hide the touch controller | - |

Platform rules the Simulator found that also apply to devices: depth/stencil textures use `MTLStorageModePrivate` on iOS (`mtl_tex.m`, `TARGET_OS_IOS`); `dylibize` skips AArch32 images (NTB2 tables, read as data); `--ipa` passes `--norsrc --noextattr --noacl` to `ditto` so no AppleDouble files land in the IPA.

## First run on a device

1. `cargo test -p napple-build && make -C runtime test-host && make -C input test && make -C frontend -f Makefile.ios test-host`
2. `VPX=1 tools/build_ios_deps.sh && tools/build_ios.sh` (libopus, libvpx, `runtime/build-ios/napple-runtime`).
3. Build the title with `--target ios` and your identity and profile (`napple build --list-signing` lists them), or `--no-sign` and sign yourself: inspect `otool -l Frameworks/<ver>.main.dylib` (segments, platform IOS), `codesign -f -s <id> --timestamp=none`, `codesign -dvv`.
4. Install and launch with the console: `xcrun devicectl device install app --device <udid> <app.ipa>`, then `xcrun devicectl device process launch --console --device <udid> <bundle id>`.
5. Read, in order: a `cannot load ... dlerror` line (dyld refused a hand-built dylib, see "Risks"); `cannot reserve the guest address space`; the `memlog:` lines (footprint, `metal ... MB` GPU share, `system still allows`); then thermals after about 10 minutes.

Choose a first title with a small footprint that already runs on the Simulator; the [title notes](../titles/README.md) record the Simulator footprint per title.

History of the iOS work: [change archive](../archive/changes/README.md).
