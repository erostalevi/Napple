# input: Horizon HID host library (shared memory writer + keyboard/mouse/GameController backend).  make [test|demo|clean]
CC      ?= clang
MAKEFLAGS += -j$(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)
DEPFLAGS = -MMD -MP
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-deprecated-declarations
OBJCFLAGS ?= -fobjc-arc
INC     := -Iinclude -Isrc
CSRC    := $(wildcard src/*.c)
MSRC    := $(wildcard src/*.m)
OBJ     := $(patsubst src/%.c,build/%.o,$(CSRC)) $(patsubst src/%.m,build/%.o,$(MSRC))
FW      := -framework GameController -framework CoreHaptics -framework Foundation

all: libnapple_input.a

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@
build/%.o: src/%.m
	@mkdir -p build
	$(CC) $(CFLAGS) $(DEPFLAGS) $(OBJCFLAGS) $(INC) -c $< -o $@

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

-include $(OBJ:.o=.d)

clean:
	rm -rf build libnapple_input.a
.PHONY: all test demo clean
