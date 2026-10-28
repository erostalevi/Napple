# Fuzz drivers for the untrusted-input C parsers (included from the runtime Makefile). See tests/fuzz/HOWTO.md.
#   make -C runtime fuzz-build                    build every driver (needs clang with the sanitizer runtimes; libFuzzer builds need libclang-rt-dev too)
#   make -C runtime fuzz-smoke                    short deterministic run of every driver (a few seconds each, no external input): suitable for CI
#   make -C runtime fuzz-run FUZZ_SECONDS=300     FUZZ_SECONDS of mutation fuzzing per target (in parallel); FUZZ_ENGINE=libfuzzer uses libFuzzer instead
#   make -C runtime fuzz-regress                  replay the regression tests of the crashes the fuzzers found (also part of test-host)
FZ_CC ?= clang
FZ_DIR := tests/fuzz
FZ_OUT := $(BUILD)/fuzz
FUZZ_SECONDS ?= 60
FUZZ_ENGINE ?= mut
FUZZ_SEED ?= 1
FUZZ_SMOKE_ITERS ?= 4000
FZ_SAN := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
FZ_CFLAGS := -std=gnu11 -O1 -g -w -Isrc -Isrc/services -I$(FZ_DIR) -pthread
FZ_UNAME := $(shell uname -s)
FZ_OPUS := $(shell pkg-config --exists opus 2>/dev/null && echo 1)
ifeq ($(FZ_OPUS),1)
FZ_OPUS_FLAGS := -DNAPPLE_HAVE_OPUS=1 $(shell pkg-config --cflags --libs opus) $(shell pkg-config --cflags opus | sed "s,/opus$$,,;s,/opus ,/ ,g")
endif

# Portable targets: pure parsers, no host stand-ins. Each FZ_SRC_<name> lists the sources linked with tests/fuzz/t_<name>.c (paths relative to runtime/).
FZ_TARGETS := codec_headers jpeg movie_audio json amiibo libapplet romfs_map romfs_overlay nro
FZ_SRC_codec_headers := src/services/codec_headers_core.c
FZ_SRC_jpeg := src/services/jpeg_core.c -lm
FZ_SRC_movie_audio := src/services/movie_audio_core.c $(FZ_OPUS_FLAGS) -lm
FZ_SRC_json := src/json.c
FZ_SRC_amiibo := src/services/amiibo_core.c src/services/amiibo_crypto_core.c src/services/amiibo_lib_core.c src/sha256.c -lm
FZ_SRC_libapplet := src/services/applet_lib_core.c
FZ_SRC_romfs_map := src/services/romfs_map.c
FZ_SRC_romfs_overlay := src/services/romfs_map.c
FZ_SRC_nro := src/nro.c src/sha256.c

# Linux-only targets: the IPC dispatcher and the services that build with the host stand-ins (tests/host, tests/linux/mach_shim.h).
ifeq ($(FZ_UNAME),Linux)
FZ_TARGETS += ipc
FZ_SERVICES := $(filter-out src/services/host_info.c src/services/services.c src/services/audio.c src/services/hid.c,$(wildcard src/services/*.c))
FZ_SRC_ipc := $(FZ_SERVICES) src/kernel.c src/threads.c src/ksession.c src/kmsg.c src/kcode.c src/timescale.c tests/host/host_stubs.c tests/host/host_nomodules.c tests/host/host_save_dir.c tests/linux/applet_stubs.c tests/fuzz/ipc_stubs.c tests/linux/ipc_mock.c src/json.c src/sha256.c -include tests/linux/mach_shim.h -Itests/host -I../gfx/include -I../frontend/include -I../audio/include -I../input/include -Wno-error=implicit-function-declaration -Wno-error=int-conversion -DFUZZ_RUNTIME_FAIL -Wl,--allow-multiple-definition -Wl,--wrap=pthread_mutex_lock -Wl,--wrap=pthread_mutex_unlock -lm -ldl
endif

FZ_MUT_BINS := $(addprefix $(FZ_OUT)/,$(addsuffix _mut,$(FZ_TARGETS)))
FZ_LF_BINS := $(addprefix $(FZ_OUT)/,$(addsuffix _lf,$(FZ_TARGETS)))
FZ_DEPS := $(FZ_DIR)/fuzz.h $(FZ_DIR)/fuzz_support.c $(wildcard tests/*vectors.h) $(wildcard src/*.h src/services/*.h) $(wildcard src/*.c src/services/*.c src/third_party/*.c tests/host/*.c tests/linux/*.c)

$(FZ_OUT)/fuzz_mut.o: $(FZ_DIR)/fuzz_mut.c $(FZ_DIR)/fuzz.h
	@mkdir -p $(dir $@)
	$(FZ_CC) $(FZ_CFLAGS) $(FZ_SAN) -c $< -o $@
# the mutation driver: target code instrumented with trace-pc-guard (new code reached = input kept), the driver itself is not
$(FZ_OUT)/%_mut: $(FZ_DIR)/t_%.c $(FZ_OUT)/fuzz_mut.o $(FZ_DEPS)
	@mkdir -p $(dir $@)
	$(FZ_CC) $(FZ_CFLAGS) $(FZ_SAN) -fsanitize-coverage=trace-pc-guard $< $(FZ_DIR)/fuzz_support.c $(FZ_OUT)/fuzz_mut.o $(FZ_SRC_$*) -o $@
$(FZ_OUT)/%_lf: $(FZ_DIR)/t_%.c $(FZ_DIR)/fuzz_libfuzzer.c $(FZ_DEPS)
	@mkdir -p $(dir $@)
	$(FZ_CC) $(FZ_CFLAGS) -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $< $(FZ_DIR)/fuzz_support.c $(FZ_DIR)/fuzz_libfuzzer.c $(FZ_SRC_$*) -o $@

fuzz-build: $(FZ_MUT_BINS)
fuzz-build-libfuzzer: $(FZ_LF_BINS)

# Deterministic smoke run: seeds + FUZZ_SMOKE_ITERS mutations per target with a fixed seed; stops at the first crash.
fuzz-smoke: $(FZ_MUT_BINS) fuzz-regress
	@set -e; for t in $(FZ_TARGETS); do $(FZ_OUT)/$${t}_mut -n $(FUZZ_SMOKE_ITERS) -s $(FUZZ_SEED) -o $(FZ_OUT)/crashes; done

# Timed run per target (the targets run in parallel under make -j; logs in build/fuzz/logs, crashes in build/fuzz/crashes).
FZ_RUN := $(addprefix fuzz-run-,$(FZ_TARGETS))
fuzz-run: $(FZ_RUN)
	@echo "fuzz-run: $(FUZZ_SECONDS)s per target done, no crash"
$(FZ_RUN): fuzz-run-%: $(if $(filter libfuzzer,$(FUZZ_ENGINE)),$(FZ_OUT)/%_lf,$(FZ_OUT)/%_mut)
	@mkdir -p $(FZ_OUT)/logs $(FZ_OUT)/crashes $(FZ_OUT)/corpus-$*
ifeq ($(FUZZ_ENGINE),libfuzzer)
	@$(FZ_OUT)/$*_lf -max_total_time=$(FUZZ_SECONDS) -seed=$(FUZZ_SEED) -max_len=65536 -timeout=20 -artifact_prefix=$(FZ_OUT)/crashes/ $(FZ_OUT)/corpus-$* > $(FZ_OUT)/logs/$*.log 2>&1 && tail -n 1 $(FZ_OUT)/logs/$*.log && echo "$*: ok" || { tail -n 30 $(FZ_OUT)/logs/$*.log; exit 1; }
else
	@$(FZ_OUT)/$*_mut -t $(FUZZ_SECONDS) -s $(FUZZ_SEED) -o $(FZ_OUT)/crashes > $(FZ_OUT)/logs/$*.log 2>&1 && tail -n 1 $(FZ_OUT)/logs/$*.log || { tail -n 30 $(FZ_OUT)/logs/$*.log; exit 1; }
endif

# Regression tests of the crashes the fuzzers found: one small program per fix (tests/fuzz/regress_*.c), built with the sanitizers so the bug would still be caught.
FZ_REGRESS := $(patsubst $(FZ_DIR)/regress_%.c,%,$(wildcard $(FZ_DIR)/regress_*.c))
ifneq ($(FZ_UNAME),Linux)
FZ_REGRESS := $(filter-out ipc kmsg,$(FZ_REGRESS)) # these link the services with the Linux host stand-ins
endif
FZ_REGRESS_BINS := $(addprefix $(FZ_OUT)/regress_,$(FZ_REGRESS))
FZ_REGRESS_SRC_codec_headers := src/services/codec_headers_core.c
FZ_REGRESS_SRC_jpeg := src/services/jpeg_core.c -lm
FZ_REGRESS_SRC_json := src/json.c
FZ_REGRESS_SRC_amiibo := $(FZ_SRC_amiibo)
FZ_REGRESS_SRC_movie_audio := $(FZ_SRC_movie_audio)
FZ_REGRESS_SRC_romfs := src/services/romfs_map.c
FZ_REGRESS_SRC_nro := $(FZ_SRC_nro)
FZ_REGRESS_SRC_libapplet := $(FZ_SRC_libapplet)
FZ_REGRESS_SRC_ipc := $(FZ_DIR)/fuzz_support.c $(FZ_SRC_ipc)
FZ_REGRESS_SRC_kmsg := $(FZ_DIR)/fuzz_support.c $(FZ_SRC_kmsg)
$(FZ_OUT)/regress_%: $(FZ_DIR)/regress_%.c tests/check.h $(FZ_DEPS) $(wildcard $(FZ_DIR)/t_*.c)
	@mkdir -p $(dir $@)
	$(FZ_CC) $(FZ_CFLAGS) $(FZ_SAN) $< $(FZ_REGRESS_SRC_$*) -o $@
fuzz-regress: $(FZ_REGRESS_BINS)
	@set -e; for t in $^; do echo "== $$t"; ./$$t; done
.PHONY: fuzz-build fuzz-build-libfuzzer fuzz-smoke fuzz-run fuzz-regress $(FZ_RUN)
