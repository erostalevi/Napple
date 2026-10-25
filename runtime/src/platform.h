/* Which Apple platform the runtime is built for. NAPPLE_IOS is 1 for iOS / iPadOS (26+), 0 for macOS and for the portable host tests.
 *
 * iOS differences (docs/ios.md): no JIT, so guest code is mapped by dyld from signed Mach-O dylibs that `napple build --target ios`
 * writes (module.c, ios_dylib.c); no exec/fork, so the runtime is the app executable (ios_platform.m); UIKit instead of AppKit
 * (frontend/src/frontend_ios.m); the 64-bit vm_* calls stand in for mach_vm_* (mach_vm_compat.h). */
#pragma once
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IOS
#define NAPPLE_IOS 1
#else
#define NAPPLE_IOS 0
#endif
