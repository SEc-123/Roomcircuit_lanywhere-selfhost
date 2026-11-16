# SPDX-License-Identifier: AGPL-3.0-only
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO must point to the devkitPro toolchain)
endif
include $(DEVKITPRO)/libnx/switch_rules

TARGET := roomcircuit-network-probe
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
STARTUP := ../emummc-startup
CFLAGS := $(ARCH) -O2 -g -Wall -Wextra -Werror -D__SWITCH__ -I$(LIBNX)/include -I$(STARTUP)
LDFLAGS := $(ARCH) -specs=$(LIBNX)/switch.specs -L$(LIBNX)/lib
CA_DER ?= ca.der

.PHONY: all clean
all: $(TARGET).nro

build/ca.o: $(CA_DER) Makefile
	mkdir -p build
	cp -- $< build/probe_ca.der
	cd build && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 --rename-section .data=.rodata,alloc,load,readonly,data,contents probe_ca.der ca.o

$(TARGET).elf: source/main.c build/ca.o $(STARTUP)/startup.c $(STARTUP)/startup.h Makefile
	$(CC) $(CFLAGS) $< build/ca.o $(STARTUP)/startup.c $(LDFLAGS) -lnx -o $@

$(TARGET).nacp: Makefile
	nacptool --create "RoomCircuit Network Probe" "RoomCircuit" "0.1.0" $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
	elf2nro $< $@ --nacp=$(TARGET).nacp

clean:
	$(RM) -r build $(TARGET).elf $(TARGET).nacp $(TARGET).nro
