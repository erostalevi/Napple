# libnapple_gfx: NVN -> Metal graphics driver for Napple. Build: make -C gfx   Test: make -C gfx test
# Requires Xcode command line tools (Metal framework). Metal validation: tests run with MTL_DEBUG_LAYER=1.

OUT    := build
MAKEFLAGS += -j$(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)
DEPFLAGS = -MMD -MP
CC     := xcrun clang
AR     := xcrun ar
ARCH   := -arch arm64
OPT    ?= -O2
WARN   := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS := -std=gnu11 $(OPT) -g $(WARN) -fno-strict-aliasing -Iinclude -Isrc $(ARCH) $(EXTRA_CFLAGS)
OFLAGS := $(CFLAGS) -fobjc-arc -fmodules -Wno-deprecated-declarations
FRAMEWORKS := -framework Metal -framework Foundation -framework QuartzCore -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers -lcompression

C_SRC  := $(wildcard src/*.c) $(wildcard src/tex/*.c) $(wildcard src/shader/*.c)
M_SRC  := $(wildcard src/*.m) $(wildcard src/shader/*.m)
GEN    := $(OUT)/gen/nvn_names.c $(OUT)/gen/nvn_unimpl.S
OBJS   := $(patsubst src/%.c,$(OUT)/obj/%.o,$(C_SRC)) $(patsubst src/%.m,$(OUT)/obj/%.o,$(M_SRC)) \
          $(OUT)/obj/gen/nvn_names.o $(OUT)/obj/gen/nvn_unimpl.o
LIB    := libnapple_gfx.a

.PHONY: all lib clean test tests
-include $(OBJS:.o=.d)
all lib: $(LIB)

$(LIB): $(OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

$(OUT)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(OUT)/obj/%.o: src/%.m
	@mkdir -p $(dir $@)
	$(CC) $(OFLAGS) $(DEPFLAGS) -c $< -o $@

# NVN name table and the per-name "unimplemented" stubs are generated from the API name list.
$(OUT)/gen/nvn_names.c: src/nvnapi/names.txt tools/gen_nvn_tables.sh
	@mkdir -p $(dir $@)
	sh tools/gen_nvn_tables.sh src/nvnapi/names.txt $(OUT)/gen
$(OUT)/gen/nvn_unimpl.S: $(OUT)/gen/nvn_names.c
$(OUT)/obj/gen/nvn_names.o: $(OUT)/gen/nvn_names.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/obj/gen/nvn_unimpl.o: $(OUT)/gen/nvn_unimpl.S
	@mkdir -p $(dir $@)
	$(CC) $(ARCH) -c $< -o $@

# ---- tests (headless Metal probes) ----------------------------------------------------------------------------------
T_SRC  := $(wildcard tests/t_*.c) $(wildcard tests/t_*.m)
T_BIN  := $(patsubst tests/%.c,$(OUT)/tests/%,$(filter %.c,$(T_SRC))) $(patsubst tests/%.m,$(OUT)/tests/%,$(filter %.m,$(T_SRC)))

$(OUT)/tests/%: tests/%.c $(LIB) $(wildcard tests/*.h) $(wildcard tests/*.inc)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fobjc-arc $< $(LIB) $(FRAMEWORKS) -o $@
$(OUT)/tests/%: tests/%.m $(LIB) $(wildcard tests/*.h) $(wildcard tests/*.inc)
	@mkdir -p $(dir $@)
	$(CC) $(OFLAGS) $< $(LIB) $(FRAMEWORKS) -o $@

tests: $(T_BIN)
test: tests
	@fail=0; for t in $(T_BIN); do echo "== $$t"; MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=0 $$t || { echo "FAILED: $$t"; fail=1; }; done; \
	  echo "== t_triangle with a disk cache (binary archive)"; rm -rf build/t_triangle-cache; \
	  for p in 1 2; do T_CACHE_PASS=$$p MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=0 $(OUT)/tests/t_triangle || { echo "FAILED: t_triangle cache pass $$p"; fail=1; }; done; \
	  echo "== t_pipe_stress: persisted pipelines, prewarm racing the draws, GCD pool flooded (pass 2 compiles without the archive)"; \
	  T_STRESS_PASS=1 MTL_DEBUG_LAYER=1 $(OUT)/tests/t_pipe_stress || { echo "FAILED: t_pipe_stress pass 1"; fail=1; }; \
	  for th in 1 4; do T_STRESS_PASS=2 T_STRESS_THREADS=$$th NAPPLE_GFX_NO_ARCHIVE=1 MTL_DEBUG_LAYER=1 $(OUT)/tests/t_pipe_stress || { echo "FAILED: t_pipe_stress pass 2 ($$th threads)"; fail=1; }; done; \
	  if [ $$fail = 0 ]; then echo "ALL TESTS PASSED"; else exit 1; fi

clean:
	rm -rf $(OUT) $(LIB)

# ---- runtime adapter: replaces runtime/src/gfx_stub.c (see docs/architecture/gfx.md, section 9) ----------------------------------
$(OUT)/obj/adapter/gfx_napple.o: adapter/gfx_napple.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -I../runtime/src -I../input/include -I../audio/include -c $< -o $@
adapter: $(OUT)/obj/adapter/gfx_napple.o
-include $(OUT)/obj/adapter/gfx_napple.d
.PHONY: adapter
