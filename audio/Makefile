# audio: Horizon audio renderer / audout / hwopus host library.  make [test|demo|clean]
CC      ?= clang
OPUS    ?= /opt/homebrew/opt/opus
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-deprecated-declarations
INC     := -Iinclude -Isrc -I$(OPUS)/include/opus
SRC     := $(wildcard src/*.c)
OBJ     := $(patsubst src/%.c,build/%.o,$(SRC))
FW      := -framework AudioToolbox -framework CoreAudio -framework Foundation
LIBOPUS := $(OPUS)/lib/libopus.a

all: libnapple_audio.a

build/%.o: src/%.c $(wildcard src/*.h) include/napple_audio.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

libnapple_audio.a: $(OBJ)
	@rm -f $@
	ar rcs $@ $(OBJ)

build/test_%: tests/test_%.c libnapple_audio.a
	$(CC) $(CFLAGS) $(INC) $< libnapple_audio.a $(LIBOPUS) $(FW) -o $@

TESTS := build/test_audren build/test_effects build/test_reverb3d build/test_review build/test_audin build/test_audout build/test_hwopus build/test_engine build/test_threads build/test_speed
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

build/demo_sine: tests/demo_sine.c libnapple_audio.a
	$(CC) $(CFLAGS) $(INC) $< libnapple_audio.a $(LIBOPUS) $(FW) -o $@
demo: build/demo_sine
	./build/demo_sine

clean:
	rm -rf build libnapple_audio.a
.PHONY: all test demo clean

# ThreadSanitizer / AddressSanitizer runs of the whole suite
tsan:
	@set -o pipefail; mkdir -p build/tsan && for t in audren effects reverb3d review audin audout threads; do \
	  $(CC) -std=gnu11 -O1 -g -fsanitize=thread $(INC) $(SRC) tests/test_$$t.c $(LIBOPUS) $(FW) -o build/tsan/t_$$t && echo "== tsan $$t" && ./build/tsan/t_$$t 2>&1 | grep -v "renderer open" | tail -15 || exit 1; done
asan:
	@set -o pipefail; mkdir -p build/asan && for t in audren effects reverb3d review audin audout hwopus engine threads; do \
	  $(CC) -std=gnu11 -O1 -g -fsanitize=address,undefined $(INC) $(SRC) tests/test_$$t.c $(LIBOPUS) $(FW) -o build/asan/t_$$t && echo "== asan $$t" && ./build/asan/t_$$t 2>&1 | grep -v "renderer open" | tail -15 || exit 1; done
.PHONY: tsan asan
