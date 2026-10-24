# napple-launcher: tiny exec wrapper placed in Contents/MacOS of built games.   make -C packaging
CC ?= clang
BUILD := build
all: $(BUILD)/napple-launcher
$(BUILD)/napple-launcher: launcher.c
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O2 -Wall -Wextra $< -o $@
clean:
	rm -rf $(BUILD)
