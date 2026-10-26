# napple-runtime: native Horizon host for rewritten Switch games.   make -C runtime [debug=1]
CC ?= clang
BUILD := build
MAKEFLAGS += -j$(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)
DEPFLAGS = -MMD -MP
SRC := $(wildcard src/*.c src/services/*.c src/a32/*.c) src/a32jit/a32jit.c
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
CFLAGS := -std=gnu11 $(INCS) $(OPT) -g -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -I/opt/homebrew/opt/libvpx/include -I/opt/homebrew/opt/opus/include -DNAPPLE_HAVE_OPUS=1 -Isrc -Isrc/services
FRAMEWORKS := -framework Cocoa -framework QuartzCore -framework CoreVideo -framework Metal -framework Foundation -framework CoreFoundation -framework AudioToolbox -framework CoreAudio -framework GameController
# Release link: drop unreferenced code and local symbols (the build is always -g for the object files, but no dSYM is produced).
LDFLAGS ?= -Wl,-dead_strip -Wl,-x
OPUS ?= /opt/homebrew/opt/opus/lib/libopus.a
VPX ?= /opt/homebrew/opt/libvpx/lib/libvpx.a
FRAMEWORKS += -framework VideoToolbox -framework CoreMedia -framework ImageIO
EXTRA_LIBS ?= -L$(ROOT)/frontend -lnapple_frontend -L$(ROOT)/input -lnapple_input -L$(ROOT)/audio -lnapple_audio $(OPUS) $(VPX) -framework CoreHaptics -framework IOKit

all: $(BUILD)/napple-runtime

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
