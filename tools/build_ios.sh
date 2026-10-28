#!/bin/sh
# Builds the napple-runtime executable for iPhone / iPad (iOS 26+) -> runtime/build-ios/napple-runtime. Mac with Xcode only.
#   tools/build_ios_deps.sh   # once: libopus (and optionally libvpx) for iPhoneOS
#   tools/build_ios.sh        # then this; `napple build --target ios` picks up runtime/build-ios/napple-runtime
# Everything compiles into runtime/build-ios and gfx/build-ios, input/build-ios, audio/build-ios, frontend/build-ios; the macOS build outputs are untouched.
# iOS Simulator (smoke tests, arm64 Mac only): SDK=iphonesimulator tools/build_ios_deps.sh && SDK=iphonesimulator tools/build_ios.sh
#   -> runtime/build-ios-sim/napple-runtime (all outputs in build-ios-sim dirs; deps in build-ios-deps/prefix-sim); use with `napple build --target ios-sim`.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${SDK:-iphoneos}
case $SDK in
  iphoneos)        SFX=ios;     TRIPLE=arm64-apple-ios26.0;           DEPS_DEFAULT=$ROOT/build-ios-deps/prefix ;;
  iphonesimulator) SFX=ios-sim; TRIPLE=arm64-apple-ios26.0-simulator; DEPS_DEFAULT=$ROOT/build-ios-deps/prefix-sim ;;
  *) echo "SDK must be iphoneos or iphonesimulator, got $SDK" >&2; exit 1 ;;
esac
B=build-$SFX
SYSROOT=${IOS_SDK:-$(xcrun --sdk "$SDK" --show-sdk-path)}
DEPS=${DEPS:-$DEPS_DEFAULT}
CC="xcrun --sdk $SDK clang -target $TRIPLE -isysroot $SYSROOT"
[ -f "$DEPS/lib/libopus.a" ] || { echo "libopus for iOS not found in $DEPS: run tools/build_ios_deps.sh first" >&2; exit 1; }
HAVE_VPX=0; [ -f "$DEPS/lib/libvpx.a" ] && HAVE_VPX=1
CF="-std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations"

objs() {  # dir outdir flags... : compiles every source of dir into outdir (flat), prints the object list
  dir=$1; out=$2; shift 2; mkdir -p "$out"
  for f in $dir; do
    case $f in *.c) lang="" ;; *.m) lang="-fobjc-arc" ;; *.S) lang="" ;; *) continue ;; esac
    o=$out/$(echo "$f" | sed "s|$ROOT/||; s|/|_|g").o
    $CC $CF $lang "$@" -c "$f" -o "$o"; echo "$o"
  done
}

# gfx: its own Makefile, redirected to iOS (no -arch: the target triple decides)
make -C "$ROOT/gfx" OUT=$B LIB=libnapple_gfx_$SFX.a CC="$CC" AR="xcrun ar" ARCH="" libnapple_gfx_$SFX.a $B/obj/adapter/gfx_napple.o
# input
IN=$(objs "$ROOT/input/src/*.c $ROOT/input/src/*.m" "$ROOT/input/$B" -I"$ROOT/input/include" -I"$ROOT/input/src")
rm -f "$ROOT/input/$B/libnapple_input.a"; xcrun ar rcs "$ROOT/input/$B/libnapple_input.a" $IN
# audio (engine.c picks RemoteIO on iOS; engine_ios.m owns the AVAudioSession)
AU=$(objs "$ROOT/audio/src/*.c $ROOT/audio/src/*.m" "$ROOT/audio/$B" -I"$ROOT/audio/include" -I"$ROOT/audio/src" -I"$DEPS/include" -I"$DEPS/include/opus")
rm -f "$ROOT/audio/$B/libnapple_audio.a"; xcrun ar rcs "$ROOT/audio/$B/libnapple_audio.a" $AU
# frontend (UIKit)
make -C "$ROOT/frontend" -f Makefile.ios CC="$CC" OUT=$B
# runtime
RT=$(objs "$ROOT/runtime/src/*.c $ROOT/runtime/src/services/*.c $ROOT/runtime/src/a32/*.c $ROOT/runtime/src/a32jit/a32jit.c $ROOT/runtime/src/*.m $ROOT/runtime/src/*.S" "$ROOT/runtime/$B" \
     -I"$ROOT/input/include" -I"$ROOT/audio/include" -I"$ROOT/gfx/include" -I"$ROOT/frontend/include" -I"$DEPS/include" -I"$ROOT/runtime/src" -I"$ROOT/runtime/src/services")
RT=$(echo "$RT" | grep -v 'gfx_stub')
METALFX="-framework MetalFX"; [ "$SDK" = iphonesimulator ] && METALFX=""   # the Xcode 27 Simulator SDK has no MetalFX
VPXLIB=""; [ $HAVE_VPX = 1 ] && VPXLIB="$DEPS/lib/libvpx.a"
$CC $RT "$ROOT/gfx/$B/obj/adapter/gfx_napple.o" "$ROOT/gfx/libnapple_gfx_$SFX.a" \
    "$ROOT/frontend/$B/libnapple_frontend.a" "$ROOT/input/$B/libnapple_input.a" "$ROOT/audio/$B/libnapple_audio.a" "$DEPS/lib/libopus.a" $VPXLIB \
    -framework UIKit -framework QuartzCore -framework Metal $METALFX -framework Foundation -framework CoreFoundation -framework CoreGraphics -framework ImageIO \
    -framework UniformTypeIdentifiers -framework AudioToolbox -framework CoreAudio -framework AVFoundation -framework GameController -framework CoreHaptics -framework CoreVideo \
    -framework CoreMedia -framework VideoToolbox -lcompression -lc++ -Wl,-rpath,@executable_path/Frameworks -o "$ROOT/runtime/$B/napple-runtime"
echo "built $ROOT/runtime/$B/napple-runtime"
# the runtime as a library (-DNAPPLE_LIB, its own globals): dlopened by the app to run a firmware applet (the Mii Maker) in-process; see runtime/src/libmode.c
RTL=$(objs "$ROOT/runtime/src/*.c $ROOT/runtime/src/services/*.c $ROOT/runtime/src/a32/*.c $ROOT/runtime/src/a32jit/a32jit.c $ROOT/runtime/src/*.m $ROOT/runtime/src/*.S" "$ROOT/runtime/$B-lib" -DNAPPLE_LIB \
     -I"$ROOT/input/include" -I"$ROOT/audio/include" -I"$ROOT/gfx/include" -I"$ROOT/frontend/include" -I"$DEPS/include" -I"$ROOT/runtime/src" -I"$ROOT/runtime/src/services")
RTL=$(echo "$RTL" | grep -v 'gfx_stub')
$CC -dynamiclib -install_name @rpath/libnapple-applet.dylib $RTL "$ROOT/gfx/$B/obj/adapter/gfx_napple.o" "$ROOT/gfx/libnapple_gfx_$SFX.a" \
    "$ROOT/frontend/$B/libnapple_frontend.a" "$ROOT/input/$B/libnapple_input.a" "$ROOT/audio/$B/libnapple_audio.a" "$DEPS/lib/libopus.a" $VPXLIB \
    -framework UIKit -framework QuartzCore -framework Metal $METALFX -framework Foundation -framework CoreFoundation -framework CoreGraphics -framework ImageIO \
    -framework UniformTypeIdentifiers -framework AudioToolbox -framework CoreAudio -framework AVFoundation -framework GameController -framework CoreHaptics -framework CoreVideo \
    -framework CoreMedia -framework VideoToolbox -lcompression -lc++ -o "$ROOT/runtime/$B/libnapple-applet.dylib"
echo "built $ROOT/runtime/$B/libnapple-applet.dylib"
