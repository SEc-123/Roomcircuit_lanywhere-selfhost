# SPDX-License-Identifier: AGPL-3.0-only
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO must point to the devkitPro toolchain)
endif
include $(DEVKITPRO)/libnx/switch_rules

TARGET := roomcircuit-network-probe
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
STARTUP := ../emummc-startup
CFLAGS := $(ARCH) -O2 -g -Wall -Wextra -Werror -D__SWITCH__ -I$(LIBNX)/include -I$(STARTUP) -Ibuild
LDFLAGS := $(ARCH) -specs=$(LIBNX)/switch.specs -L$(LIBNX)/lib
CA_DER ?=
PROBE_IPV4 ?=
ifeq ($(strip $(CA_DER)),)
$(error Explicit CA_DER is required)
endif
ifeq ($(strip $(PROBE_IPV4)),)
$(error Explicit PROBE_IPV4 is required)
endif

.PHONY: all clean FORCE
all: $(TARGET).nro

FORCE:
build/rc_build_config.h: FORCE $(CA_DER) ../build-inputs/prepare.py
	python3 ../build-inputs/prepare.py --ca "$(CA_DER)" --build-dir build --address "$(PROBE_IPV4)"
build/ca.o: build/rc_build_config.h Makefile
	cd build && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 --rename-section .data=.rodata,alloc,load,readonly,data,contents probe_ca.der ca.o

$(TARGET).elf: source/main.c build/rc_build_config.h build/ca.o $(STARTUP)/startup.c $(STARTUP)/startup.h Makefile
	$(CC) $(CFLAGS) $< build/ca.o $(STARTUP)/startup.c $(LDFLAGS) -lnx -o $@

$(TARGET).nacp: Makefile
	nacptool --create "RoomCircuit Network Probe" "RoomCircuit" "0.1.0" $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
	elf2nro $< $@ --nacp=$(TARGET).nacp

clean:
	$(RM) -r build $(TARGET).elf $(TARGET).nacp $(TARGET).nro
