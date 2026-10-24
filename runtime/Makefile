# napple-runtime: native Horizon host for rewritten Switch games.   make -C runtime [debug=1]
CC ?= clang
BUILD := build
SRC := $(wildcard src/*.c src/services/*.c)
MSRC := $(wildcard src/*.m)
ASM := $(wildcard src/*.S)
OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC)) $(patsubst src/%.m,$(BUILD)/%.o,$(MSRC)) $(patsubst src/%.S,$(BUILD)/%.o,$(ASM))
# GFX=napple links the real graphics driver (gfx/libnapple_gfx.a + adapter) instead of the stub back end.
GFX ?= napple
ifeq ($(GFX),napple)
OBJ := $(filter-out $(BUILD)/gfx_stub.o,$(OBJ))
GFX_LIBS := ../gfx/build/obj/adapter/gfx_napple.o ../gfx/libnapple_gfx.a -framework Metal -framework ImageIO -framework UniformTypeIdentifiers -framework CoreGraphics
$(BUILD)/napple-runtime: ../gfx/libnapple_gfx.a ../gfx/build/obj/adapter/gfx_napple.o
endif
OPT := $(if $(debug),-O0,-O2)
ROOT := ..
INCS := -I$(ROOT)/input/include -I$(ROOT)/audio/include -I$(ROOT)/gfx/include -I$(ROOT)/frontend/include
CFLAGS := -std=gnu11 $(INCS) $(OPT) -g -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -I/opt/homebrew/opt/libvpx/include -Isrc -Isrc/services
FRAMEWORKS := -framework Cocoa -framework QuartzCore -framework CoreVideo -framework Metal -framework Foundation -framework CoreFoundation -framework AudioToolbox -framework CoreAudio -framework GameController
OPUS ?= /opt/homebrew/opt/opus/lib/libopus.a
VPX ?= /opt/homebrew/opt/libvpx/lib/libvpx.a
EXTRA_LIBS ?= -L$(ROOT)/frontend -lnapple_frontend -L$(ROOT)/input -lnapple_input -L$(ROOT)/audio -lnapple_audio $(OPUS) $(VPX) -framework CoreHaptics -framework IOKit

all: $(BUILD)/napple-runtime

$(BUILD)/napple-runtime: $(OBJ) $(ROOT)/frontend/libnapple_frontend.a $(ROOT)/input/libnapple_input.a $(ROOT)/audio/libnapple_audio.a
	$(CC) $(OBJ) $(GFX_LIBS) $(FRAMEWORKS) $(EXTRA_LIBS) -o $@

$(BUILD)/%.o: src/%.c $(wildcard src/*.h) $(wildcard src/services/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.m $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fobjc-arc -c $< -o $@

$(BUILD)/%.o: src/%.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

# Unit tests (runtime/tests): link the runtime objects without main/app; tests that include a source file leave its object out.
TESTOBJ = $(filter-out $(BUILD)/main.o $(BUILD)/app.o,$(OBJ))
$(BUILD)/tests/stubs.o: tests/stubs.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/tests/test_ipc: tests/test_ipc.c tests/check.h $(BUILD)/tests/stubs.o $(OBJ)
	$(CC) $(CFLAGS) tests/test_ipc.c $(BUILD)/tests/stubs.o $(filter-out $(BUILD)/ipc.o,$(TESTOBJ)) $(GFX_LIBS) $(FRAMEWORKS) $(EXTRA_LIBS) -o $@
$(BUILD)/tests/test_fs_journal: tests/test_fs_journal.c tests/check.h $(BUILD)/tests/stubs.o $(OBJ)
	$(CC) $(CFLAGS) tests/test_fs_journal.c $(BUILD)/tests/stubs.o $(filter-out $(BUILD)/services/fs.o,$(TESTOBJ)) $(GFX_LIBS) $(FRAMEWORKS) $(EXTRA_LIBS) -o $@
$(BUILD)/tests/test_hid_service: tests/test_hid_service.c tests/check.h $(BUILD)/tests/stubs.o $(OBJ)
	$(CC) $(CFLAGS) tests/test_hid_service.c $(BUILD)/tests/stubs.o $(filter-out $(BUILD)/services/hid.o,$(TESTOBJ)) $(GFX_LIBS) $(FRAMEWORKS) $(EXTRA_LIBS) -o $@
$(BUILD)/tests/test_timescale: tests/test_timescale.c src/timescale.c src/timescale.h tests/check.h $(BUILD)/tests/stubs.o $(OBJ)
	$(CC) $(CFLAGS) tests/test_timescale.c $(BUILD)/tests/stubs.o $(filter-out $(BUILD)/timescale.o,$(TESTOBJ)) $(GFX_LIBS) $(FRAMEWORKS) $(EXTRA_LIBS) -o $@
test: $(BUILD)/tests/test_ipc $(BUILD)/tests/test_fs_journal $(BUILD)/tests/test_hid_service $(BUILD)/tests/test_timescale
	@for t in $^; do case $$t in *.o) continue;; esac; echo "== $$t"; ./$$t || exit 1; done

clean:
	rm -rf $(BUILD)

.PHONY: all clean test
