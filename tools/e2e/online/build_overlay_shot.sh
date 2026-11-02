#!/bin/bash
# Builds tools/e2e/online/overlay_shot (needs frontend/, input/, audio/ libs and the release napple-cloud lib: make -C frontend; make -C runtime CLOUD=1 build/online/online_ui.o).
set -e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"; cd "$ROOT"
cc -std=gnu11 -O1 -g -Wno-deprecated-declarations -fobjc-arc -Ifrontend/include -Ifrontend/src -Iinput/include -Iaudio/include -Iruntime/src/services -Iruntime/src/online -Icrates/napple-cloud/include \
  tools/e2e/online/overlay_shot.m runtime/build/online/online_ui.o runtime/build/json.o frontend/libnapple_frontend.a input/libnapple_input.a audio/libnapple_audio.a target/release/libnapple_cloud.a \
  /opt/homebrew/opt/opus/lib/libopus.a -framework Cocoa -framework QuartzCore -framework CoreVideo -framework Metal -framework Foundation -framework AVFoundation -framework CoreMedia \
  -framework UniformTypeIdentifiers -framework WebKit -framework GameController -framework CoreHaptics -framework AudioToolbox -framework CoreAudio -framework Security -Wl,-undefined,dynamic_lookup \
  -o "${TMPDIR:-/tmp}/overlay_shot"
echo "${TMPDIR:-/tmp}/overlay_shot"
