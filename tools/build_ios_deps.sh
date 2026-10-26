#!/bin/sh
# Builds the third-party static libraries the iOS runtime links (libopus; libvpx is optional) into build-ios-deps/ with the iPhoneOS SDK.
# Mac with Xcode + cmake only. Nothing is committed; sources are downloaded from the upstream release servers, or give your own checkout:
#   OPUS_SRC=/path/to/opus  VPX_SRC=/path/to/libvpx  tools/build_ios_deps.sh
# iOS Simulator (arm64): SDK=iphonesimulator tools/build_ios_deps.sh  -> build-ios-deps/prefix-sim (the device prefix is untouched).
# The script prints the SHA-256 of what it downloaded: compare it with the project's release page before trusting the build.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/build-ios-deps
SDKNAME=${SDK:-iphoneos}
case $SDKNAME in
  iphoneos) SFX="" ;;
  iphonesimulator) SFX=-sim ;;
  *) echo "SDK must be iphoneos or iphonesimulator, got $SDKNAME" >&2; exit 1 ;;
esac
SDK=${IOS_SDK:-$(xcrun --sdk "$SDKNAME" --show-sdk-path)}
MIN=26.0
VMINFLAG=-miphoneos-version-min=$MIN; [ "$SDKNAME" = iphonesimulator ] && VMINFLAG=-mios-simulator-version-min=$MIN
PREFIX=$OUT/prefix$SFX
mkdir -p "$OUT/src" "$PREFIX"

fetch() {  # url dest-dir
  name=$(basename "$1")
  [ -f "$OUT/src/$name" ] || curl -fL --retry 3 -o "$OUT/src/$name" "$1"
  echo "sha256 $name: $(shasum -a 256 "$OUT/src/$name" | cut -d' ' -f1)"
  mkdir -p "$2" && tar -xzf "$OUT/src/$name" -C "$2" --strip-components 1
}

if [ ! -f "$PREFIX/lib/libopus.a" ]; then
  SRC=${OPUS_SRC:-}
  if [ -z "$SRC" ]; then SRC=$OUT/src/opus; [ -d "$SRC" ] || fetch https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz "$SRC"; fi
  cmake -S "$SRC" -B "$OUT/opus-build$SFX" -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$SDKNAME -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN \
        -DBUILD_SHARED_LIBS=OFF -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX"
  cmake --build "$OUT/opus-build$SFX" -j 8 && cmake --install "$OUT/opus-build$SFX"
fi

if [ "${VPX:-0}" = 1 ] && [ ! -f "$PREFIX/lib/libvpx.a" ]; then   # optional: VP9 movie decode; without it the game's own decoder path is used
  SRC=${VPX_SRC:-$OUT/src/libvpx}
  [ -d "$SRC" ] || fetch https://github.com/webmproject/libvpx/archive/refs/tags/v1.14.1.tar.gz "$SRC"
  mkdir -p "$OUT/vpx-build$SFX"
  if [ "$SDKNAME" = iphonesimulator ]; then   # libvpx's darwin targets force the macOS SDK; build as a generic arm64 target with the Simulator toolchain instead (NEON intrinsics still on)
    TC="xcrun --sdk iphonesimulator clang -target arm64-apple-ios$MIN-simulator -isysroot $SDK"
    TCXX="xcrun --sdk iphonesimulator clang++ -target arm64-apple-ios$MIN-simulator -isysroot $SDK"
    (cd "$OUT/vpx-build$SFX" && CC="$TC" CXX="$TCXX" AS="$TC" LD="$TC" AR="xcrun ar" "$SRC/configure" --target=arm64-linux-gcc --prefix="$PREFIX" --disable-examples --disable-tools --disable-docs \
        --disable-unit-tests --enable-vp9-decoder --disable-vp8 --disable-vp9-encoder --enable-pic --disable-install-bins && make -j 8 && make install)
  else
  (cd "$OUT/vpx-build$SFX" && "$SRC/configure" --target=arm64-darwin20-gcc --prefix="$PREFIX" --disable-examples --disable-tools --disable-docs \
      --disable-unit-tests --enable-vp9-decoder --disable-vp8 --disable-vp9-encoder --enable-pic --extra-cflags="-isysroot $SDK $VMINFLAG" \
      --extra-cxxflags="-isysroot $SDK $VMINFLAG" && make -j 8 && make install)
  fi
fi
echo "ok: $PREFIX"
