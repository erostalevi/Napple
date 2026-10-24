# input: Horizon HID host library (shared memory writer + keyboard/mouse/GameController backend).  make [test|demo|clean]
CC      ?= clang
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-deprecated-declarations
OBJCFLAGS ?= -fobjc-arc
INC     := -Iinclude -Isrc
CSRC    := $(wildcard src/*.c)
MSRC    := $(wildcard src/*.m)
OBJ     := $(patsubst src/%.c,build/%.o,$(CSRC)) $(patsubst src/%.m,build/%.o,$(MSRC))
FW      := -framework GameController -framework CoreHaptics -framework Foundation

all: libnapple_input.a

build/%.o: src/%.c $(wildcard src/*.h) include/napple_input.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) -c $< -o $@
build/%.o: src/%.m $(wildcard src/*.h) include/napple_input.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(OBJCFLAGS) $(INC) -c $< -o $@

libnapple_input.a: $(OBJ)
	@rm -f $@
	ar rcs $@ $(OBJ)

build/test_%: tests/test_%.c libnapple_input.a tests/check.h
	$(CC) $(CFLAGS) $(INC) $< libnapple_input.a $(FW) -o $@

TESTS := build/test_hid build/test_bindings build/test_motion build/test_input
test: $(TESTS)
	@cmp src/tinyjson.h ../frontend/src/tinyjson.h && echo "tinyjson.h copies identical"
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

build/demo_pads: tests/demo_pads.m libnapple_input.a
	$(CC) $(CFLAGS) $(OBJCFLAGS) $(INC) $< libnapple_input.a $(FW) -framework AppKit -o $@
demo: build/demo_pads
	./build/demo_pads

clean:
	rm -rf build libnapple_input.a
.PHONY: all test demo clean
