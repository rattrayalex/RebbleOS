# Host unit test for rcore/keyboard_map.c, the firmware-free half of the
# external keyboard support: `make -C tests/host` builds it with the native cc and runs it.

CC ?= cc
CFLAGS ?= -std=gnu99 -Wall -Wextra -Werror -g
ROOT := ../..
BUILD := $(ROOT)/build/host

all: run

$(BUILD)/keyboard_map_test: keyboard_map_test.c $(ROOT)/rcore/keyboard_map.c $(ROOT)/rcore/keyboard_map.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(ROOT)/rcore -o $@ keyboard_map_test.c $(ROOT)/rcore/keyboard_map.c

run: $(BUILD)/keyboard_map_test
	$(BUILD)/keyboard_map_test

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
