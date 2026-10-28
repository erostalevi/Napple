# frontend: macOS window, overlay, settings, swkbd bar.  make [test|demo|clean]
CC      ?= clang
MAKEFLAGS += -j$(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)
DEPFLAGS = -MMD -MP
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-deprecated-declarations
INC     := -Iinclude -Isrc -I../input/include -I../audio/include -I../runtime/src/services
CSRC    := $(wildcard src/*.c)
MSRC    := $(wildcard src/*.m)
OBJ     := $(patsubst src/%.c,build/%.o,$(CSRC)) $(patsubst src/%.m,build/%.o,$(MSRC))
FW      := -framework Cocoa -framework QuartzCore -framework CoreVideo -framework Metal -framework Foundation -framework AVFoundation -framework CoreMedia -framework UniformTypeIdentifiers -framework WebKit
SIBLINGS:= ../input/libnapple_input.a ../audio/libnapple_audio.a /opt/homebrew/opt/opus/lib/libopus.a
SFW     := -framework GameController -framework CoreHaptics -framework AudioToolbox -framework CoreAudio

all: libnapple_frontend.a

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@
# -fmodules: the objects carry their framework link options (AVFoundation for simulators.m) so the runtime link line needs no change.
build/%.o: src/%.m
	@mkdir -p build
	$(CC) $(CFLAGS) $(DEPFLAGS) -fobjc-arc -fmodules $(INC) -c $< -o $@

libnapple_frontend.a: $(OBJ)
	@rm -f $@
	ar rcs $@ $(OBJ)

$(SIBLINGS):
	$(MAKE) -C ../input
	$(MAKE) -C ../audio

build/test_settings: tests/test_settings.c libnapple_frontend.a
	$(CC) $(CFLAGS) $(INC) $< libnapple_frontend.a $(FW) $(NORT) -o $@
# The frontend's own binaries have no runtime: the simulator pages' weak references to it resolve to NULL.
NORT    := -Wl,-undefined,dynamic_lookup
build/test_window: tests/test_window.m libnapple_frontend.a $(SIBLINGS)
	$(CC) $(CFLAGS) -fobjc-arc $(INC) $< libnapple_frontend.a $(SIBLINGS) $(FW) $(SFW) $(NORT) -o $@
build/demo_window: tests/demo_window.m libnapple_frontend.a $(SIBLINGS)
	$(CC) $(CFLAGS) -fobjc-arc $(INC) $< libnapple_frontend.a $(SIBLINGS) $(FW) $(SFW) $(NORT) -o $@

test: build/test_settings build/test_window
	@cmp src/tinyjson.h ../input/src/tinyjson.h && echo "tinyjson.h copies identical"
	./build/test_settings
	./build/test_window
# Interactive-free check of the applet sheets (they open real windows for about a second each; NAPPLE_SHEET_SHOT=<dir> keeps their screenshots).
build/test_sheets: tests/test_sheets.m libnapple_frontend.a $(SIBLINGS)
	$(CC) $(CFLAGS) -fobjc-arc $(INC) $< libnapple_frontend.a $(SIBLINGS) $(FW) $(SFW) $(NORT) -o $@
test-sheets: build/test_sheets
	./build/test_sheets
demo: build/demo_window
	./build/demo_window
-include $(OBJ:.o=.d)

clean:
	rm -rf build libnapple_frontend.a
.PHONY: all test test-sheets demo clean
