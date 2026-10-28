# 2026-10-27 iOS device candidate (branch claude/ios-candidate-2026-10-27)

Goal: pick the title for the first real iPhone run, measure memory on the iOS Simulator, apply cheap memory changes, deliver a signed IPA. Result and tables: `docs/guides/ios-device-candidate.md`. Recommendation: **LAS** (782 MB footprint in play, 60 fps on the Simulator); signed IPA at `~/Desktop/NappleIOS/LAS-com.napple.las.ipa` (not installed).

Changes
* `crates/napple-build`: `--update-romfs in-place` is allowed with `--target ios-sim` (a full app with dylibs and a 280 MB bundle that reads the RomFS from the game files; the games must sit outside `~/Documents`, the Simulator process cannot read it: `cp -c` them to a scratch dir). A firmware-applet module that does not fit the Mach-O page layout (the Mii Maker's tiny `sdk`) is skipped with a note instead of failing the whole iOS build.
* `tools/build_ios.sh`: `-lcompression` for `libnapple-applet.dylib` (link failed on the gfx command-recording LZ4). `tools/build_ios_deps.sh`: libvpx for iPhoneOS built as the generic arm64 target with the iOS toolchain (the darwin target forced the macOS SDK, "toolchain is unable to link").
* `runtime/src/memlog.m` + `gfx/src/mtl_memstat.m`: periodic `memlog:` line (footprint, peak, guest-mapped, Metal allocation, iOS available memory); on by default on iOS, `NAPPLE_MEMLOG=1` elsewhere. `NAPPLE_HEAP_CAP_MB` overrides the guest heap cap for diagnosis (`runtime/src/memory.c`).
* `input/src/bindings.c`: handheld (720p) is the default operation mode on iPhone / iPad.

Verified (Simulator, Xcode 27.1): all of the above built and run; LAS boots to gameplay with the default and with the device layout (3.25 GiB cap, 1 GiB alias); `make -C input test` passes. Not verified: anything on a phone. SMO stops after its menu ("Compiling shaders", Simulator colour-attachment budget), ACNH aborts on a cube-array texture, MK8D on an RGBA8 to RGBA32Uint texture view (gfx, not touched).
