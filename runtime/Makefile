# napple-runtime: native Horizon host for rewritten Switch games.   make -C runtime [debug=1]
CC ?= clang
BUILD := build
MAKEFLAGS += -j$(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)
DEPFLAGS = -MMD -MP
# A32_INTERP=1 builds the AArch32 interpreter loop and the decoders into the runtime: a debug build (NAPPLE_A32_INTERP) for diagnostics and the test oracle.
# The shipped runtime has no interpreter: lifted code (a32jit.dylib) plus out-of-line operation semantics (a32_op_exec) only.
A32_INTERP ?= 0
A32SRC := $(filter-out src/a32/a32_decode_arm.c src/a32/a32_decode_thumb.c,$(wildcard src/a32/*.c))
ifeq ($(A32_INTERP),1)
A32SRC := $(wildcard src/a32/*.c)
A32DEF := -DNAPPLE_A32_INTERP
endif
SRC := $(wildcard src/*.c src/services/*.c) $(A32SRC) src/a32jit/a32jit.c
MSRC := $(wildcard src/*.m)
ASM := $(wildcard src/*.S)
OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC)) $(patsubst src/%.m,$(BUILD)/%.o,$(MSRC)) $(patsubst src/%.S,$(BUILD)/%.o,$(ASM))
# GFX=napple links the real graphics driver (gfx/libnapple_gfx.a + adapter) instead of the stub back end.
GFX ?= napple
ifeq ($(GFX),napple)
OBJ := $(filter-out $(BUILD)/gfx_stub.o,$(OBJ))
GFX_LIBS := ../gfx/build/obj/adapter/gfx_napple.o ../gfx/libnapple_gfx.a -framework UniformTypeIdentifiers -framework CoreGraphics
$(BUILD)/napple-runtime: ../gfx/libnapple_gfx.a ../gfx/build/obj/adapter/gfx_napple.o
endif
OPT ?= $(if $(debug),-O0,-O2)
ROOT := ..
INCS := -I$(ROOT)/input/include -I$(ROOT)/audio/include -I$(ROOT)/gfx/include -I$(ROOT)/frontend/include
CFLAGS := $(EXTRA_CFLAGS) -std=gnu11 $(A32DEF) $(INCS) $(OPT) -g -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -I/opt/homebrew/opt/libvpx/include -I/opt/homebrew/opt/opus/include -DNAPPLE_HAVE_OPUS=1 -Isrc -Isrc/services
FRAMEWORKS := -framework Cocoa -framework QuartzCore -framework CoreVideo -framework Metal -framework Foundation -framework CoreFoundation -framework AudioToolbox -framework CoreAudio -framework GameController
# Release link: drop unreferenced code and local symbols (the build is always -g for the object files, but no dSYM is produced).
LDFLAGS ?= -Wl,-dead_strip -Wl,-x
OPUS ?= /opt/homebrew/opt/opus/lib/libopus.a
VPX ?= /opt/homebrew/opt/libvpx/lib/libvpx.a
FRAMEWORKS += -framework VideoToolbox -framework CoreMedia -framework ImageIO -lcompression
EXTRA_LIBS ?= -L$(ROOT)/frontend -lnapple_frontend -L$(ROOT)/input -lnapple_input -L$(ROOT)/audio -lnapple_audio $(OPUS) $(VPX) -framework CoreHaptics -framework IOKit

all: $(BUILD)/napple-runtime

# Library mode: the runtime as a dylib (own copy of every global) that a host process dlopens to run a firmware applet in-process (iOS: the Mii Maker).
# `make applet-lib` -> build-lib/libnapple-applet.dylib, entry napple_applet_main(argc, argv); see src/libmode.c and tests/inproc_host.c.
applet-lib:
	$(MAKE) BUILD=build-lib EXTRA_CFLAGS=-DNAPPLE_LIB build-lib/libnapple-applet.dylib
build-lib/libnapple-applet.dylib: $(OBJ) $(ROOT)/frontend/libnapple_frontend.a $(ROOT)/input/libnapple_input.a $(ROOT)/audio/libnapple_audio.a ../gfx/libnapple_gfx.a ../gfx/build/obj/adapter/gfx_napple.o
	$(CC) -dynamiclib -install_name @rpath/libnapple-applet.dylib $(OBJ) $(GFX_LIBS) $(FRAMEWORKS) $(EXTRA_LIBS) $(LDFLAGS) -o $@
$(BUILD)/inproc_host: tests/inproc_host.c
	$(CC) -std=gnu11 -O1 -g $< -o $@

$(BUILD)/napple-runtime: $(OBJ) $(ROOT)/frontend/libnapple_frontend.a $(ROOT)/input/libnapple_input.a $(ROOT)/audio/libnapple_audio.a
	$(CC) $(OBJ) $(GFX_LIBS) $(FRAMEWORKS) $(EXTRA_LIBS) $(LDFLAGS) -o $@

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.m
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -fobjc-arc -c $< -o $@

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
$(BUILD)/tests/test_media: tests/test_media.c src/vt_h264.c src/vt_h264.h tests/check.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) tests/test_media.c src/vt_h264.c -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreFoundation -framework ImageIO -framework CoreGraphics -o $@
test: $(BUILD)/tests/test_media $(BUILD)/tests/test_ipc $(BUILD)/tests/test_fs_journal $(BUILD)/tests/test_hid_service $(BUILD)/tests/test_timescale
	@for t in $^; do case $$t in *.o) continue;; esac; echo "== $$t"; ./$$t || exit 1; done

# Host unit tests (any POSIX host, e.g. Linux): the portable kernel, loader and service code with tests/host stand-ins for the macOS-only
# parts (memory reservation, gate, modules). `make test-host`; HOST_CC picks the compiler.
HOST_CC ?= cc
HOST_BUILD := $(BUILD)/host
HOST_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-unused-result -Isrc -Isrc/services -Itests/host -pthread
HOST_KERNEL := src/kernel.c src/threads.c src/ksession.c src/kmsg.c src/kcode.c src/timescale.c tests/host/host_stubs.c
HOST_LOADER := src/module.c src/dispatch.c src/json.c src/nro.c src/sha256.c src/ipc.c
HOST_HDRS := $(wildcard src/*.h) $(wildcard tests/host/*.h) tests/check.h
# media-services (2026-10-26): the portable media engines nvdrv.c now calls (host1x parser, engines, null decoder backend, header parsers, JPEG codec)
HOST_MEDIA := src/services/nvdrv_media_core.c src/services/decoder_null_core.c src/services/codec_headers_core.c src/services/jpeg_core.c
HOST_TESTS := $(HOST_BUILD)/test_svc_async $(HOST_BUILD)/test_kernel_ipc $(HOST_BUILD)/test_loader $(HOST_BUILD)/test_nvdrv $(HOST_BUILD)/test_fs_savedata $(HOST_BUILD)/test_fs_journal $(HOST_BUILD)/test_ios_dylib
HOST_FS := src/ipc.c $(HOST_KERNEL) tests/host/host_nomodules.c tests/host/host_save_dir.c src/services/romfs_map.c
$(HOST_BUILD)/test_kernel_ipc: tests/host/test_kernel_ipc.c src/ipc.c $(HOST_KERNEL) $(HOST_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/host/test_kernel_ipc.c src/ipc.c $(HOST_KERNEL) tests/host/host_nomodules.c -o $@
$(HOST_BUILD)/test_svc_async: tests/host/test_svc_async.c src/kernel.c $(HOST_KERNEL) $(HOST_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/host/test_svc_async.c src/ipc.c $(HOST_KERNEL) tests/host/host_nomodules.c -o $@
$(HOST_BUILD)/test_loader: tests/host/test_loader.c src/ro.c $(HOST_KERNEL) $(HOST_LOADER) $(HOST_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/host/test_loader.c $(HOST_KERNEL) $(HOST_LOADER) -o $@
$(HOST_BUILD)/test_nvdrv: tests/host/test_nvdrv.c src/services/nvdrv.c src/ipc.c $(HOST_KERNEL) $(HOST_MEDIA) $(HOST_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/host/test_nvdrv.c src/ipc.c $(HOST_KERNEL) tests/host/host_nomodules.c $(HOST_MEDIA) -lm -o $@
$(HOST_BUILD)/test_fs_savedata: tests/host/test_fs_savedata.c src/services/fs.c $(HOST_KERNEL) $(HOST_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/host/test_fs_savedata.c $(HOST_FS) -o $@
$(HOST_BUILD)/test_fs_journal: tests/test_fs_journal.c src/services/fs.c $(HOST_KERNEL) $(HOST_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/test_fs_journal.c $(HOST_FS) -o $@
$(HOST_BUILD)/test_ios_dylib: tests/host/test_ios_dylib.c src/ios_dylib.c $(HOST_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/host/test_ios_dylib.c src/ios_dylib.c -o $@
# nx-kernel (2026-10-26): same-core handoff, yield, sleep precision, FreeThreadCount, ideal core -3
$(HOST_BUILD)/test_kernel_sched: tests/host/test_kernel_sched.c $(HOST_KERNEL) $(HOST_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/host/test_kernel_sched.c src/ipc.c $(HOST_KERNEL) tests/host/host_nomodules.c -o $@
HOST_TESTS += $(HOST_BUILD)/test_kernel_sched
# media-services (2026-10-26): the media engines through nvdrv's own ioctl handlers (also runs under tools/linux-check on Linux)
$(HOST_BUILD)/test_nvdrv_media: tests/test_svc_nvdrv_media.c src/services/nvdrv.c src/ipc.c $(HOST_KERNEL) $(HOST_MEDIA) $(HOST_HDRS) $(wildcard tests/*vectors.h)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -DNAPPLE_HOST_TEST tests/test_svc_nvdrv_media.c src/ipc.c $(HOST_KERNEL) tests/host/host_nomodules.c $(HOST_MEDIA) -lm -o $@
HOST_TESTS += $(HOST_BUILD)/test_nvdrv_media
test-host: $(HOST_TESTS)
	@for t in $^; do echo "== $$t"; ./$$t || exit 1; done

clean:
	rm -rf $(BUILD)

.PHONY: all clean test test-host

# Header dependency tracking: touching a header rebuilds only the objects that include it.
-include $(OBJ:.o=.d)

# Service unit tests (services-2026-10-24): `make test-services`
-include tests/services.mk

# AArch32 lifter tests (a32-full, 2026-10-26): lifted code vs the interpreter oracle (difftest, progtest) and the no-interpreter proof for the shipped binary.
# The core fuzz against Unicorn is python3 tests/a32/fuzz_all.py (needs `pip install unicorn`). `make test-a32`
A32_TEST_SRC := $(filter-out src/a32/a32_module.c src/a32/a32_proc.c src/a32/a32_hle.c,$(wildcard src/a32/a32_*.c))
$(BUILD)/tests/a32/difftest: tests/a32jit/difftest.c src/a32jit/a32lift.c $(A32_TEST_SRC)
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -DNAPPLE_A32_INTERP -Isrc/a32 -o $@ tests/a32jit/difftest.c src/a32jit/a32lift.c $(A32_TEST_SRC)
test-a32: $(BUILD)/tests/a32/difftest $(BUILD)/napple-runtime
	$(BUILD)/tests/a32/difftest --mode arm --count 20000
	$(BUILD)/tests/a32/difftest --mode thumb --count 20000
	python3 tests/a32jit/inline_words.py --count 2000 --difftest $(BUILD)/tests/a32/difftest
	python3 tests/a32jit/progtest.py
	../tools/check_a32_no_interp.sh $(BUILD)/napple-runtime
.PHONY: test-a32
