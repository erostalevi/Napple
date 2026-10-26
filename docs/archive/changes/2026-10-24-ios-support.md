# iOS / iPadOS 26+ support (2026-10-24, branch claude/wonderful-goldberg-k4752w)

Design, usage, risks and the first-run checklist: `docs/ios.md`. This note is the change list.

- `napple build --target ios|iphone|ipad`: flat bundle, rewritten modules packed as signable Mach-O dylibs (`macho.rs`, no JIT), profile-based signing, `--ipa`, `--install` (`devicectl`), `--list-signing`.
- Runtime: dlopen path for modules (`module.c`, `ios_dylib.c`), `vm_*` shim, bundle bootstrap (`ios_platform.m`), no executable code-memory aliases on iOS.
- UIKit frontend with on-screen controller (`frontend/src/ios/`), `napple_input_virtual_pad()`, iOS audio session, gfx `displaySyncEnabled` guard.
- Studio: target picker, signing identity/profile/device pickers, install on device, `.ipa`, unsigned mode.
- Tools: `tools/build_ios.sh`, `tools/build_ios_deps.sh`.

Verified on Linux: Mach-O writer, iOS build path up to the dylib step, plist/profile logic, `ios_dylib.c`, virtual pad merge, controller geometry. Everything that needs UIKit, Xcode, Swift or a device is unbuilt (list in `docs/ios.md`).
