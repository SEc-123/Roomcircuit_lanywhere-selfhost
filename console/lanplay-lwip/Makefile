CC ?= cc
AR ?= ar
CFLAGS ?= -O2
SOURCES := $(wildcard src/core/*.c) $(wildcard src/core/ipv4/*.c)
BUILD ?= build/host
LIBRARY := $(BUILD)/liblanplay_lwip.a
OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(SOURCES))
all: $(LIBRARY)
$(LIBRARY): $(OBJECTS)
	$(AR) rcs $@ $^
$(BUILD)/%.o: %.c include/lwipopts.h include/arch/cc.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=c11 -MMD -MP -Iinclude -c $< -o $@
clean:
	rm -rf $(BUILD)
.PHONY: all clean

-include $(OBJECTS:.o=.d)
