# frontend: macOS window, overlay, settings, swkbd bar.  make [test|demo|clean]
CC      ?= clang
MAKEFLAGS += -j$(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)
DEPFLAGS = -MMD -MP
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-deprecated-declarations
INC     := -Iinclude -Isrc -I../input/include -I../audio/include
CSRC    := $(wildcard src/*.c)
MSRC    := $(wildcard src/*.m)
OBJ     := $(patsubst src/%.c,build/%.o,$(CSRC)) $(patsubst src/%.m,build/%.o,$(MSRC))
FW      := -framework Cocoa -framework QuartzCore -framework CoreVideo -framework Metal -framework Foundation
SIBLINGS:= ../input/libnapple_input.a ../audio/libnapple_audio.a /opt/homebrew/opt/opus/lib/libopus.a
SFW     := -framework GameController -framework CoreHaptics -framework AudioToolbox -framework CoreAudio

all: libnapple_frontend.a

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@
build/%.o: src/%.m
	@mkdir -p build
	$(CC) $(CFLAGS) $(DEPFLAGS) -fobjc-arc $(INC) -c $< -o $@

libnapple_frontend.a: $(OBJ)
	@rm -f $@
	ar rcs $@ $(OBJ)

$(SIBLINGS):
	$(MAKE) -C ../input
	$(MAKE) -C ../audio

build/test_settings: tests/test_settings.c libnapple_frontend.a
	$(CC) $(CFLAGS) $(INC) $< libnapple_frontend.a $(FW) -o $@
build/test_window: tests/test_window.m libnapple_frontend.a $(SIBLINGS)
	$(CC) $(CFLAGS) -fobjc-arc $(INC) $< libnapple_frontend.a $(SIBLINGS) $(FW) $(SFW) -o $@
build/demo_window: tests/demo_window.m libnapple_frontend.a $(SIBLINGS)
	$(CC) $(CFLAGS) -fobjc-arc $(INC) $< libnapple_frontend.a $(SIBLINGS) $(FW) $(SFW) -o $@

test: build/test_settings build/test_window
	@cmp src/tinyjson.h ../input/src/tinyjson.h && echo "tinyjson.h copies identical"
	./build/test_settings
	./build/test_window
demo: build/demo_window
	./build/demo_window
-include $(OBJ:.o=.d)

clean:
	rm -rf build libnapple_frontend.a
.PHONY: all test demo clean
