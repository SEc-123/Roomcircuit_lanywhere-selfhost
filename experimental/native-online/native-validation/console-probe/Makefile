# SPDX-License-Identifier: AGPL-3.0-only
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO must point to the devkitPro toolchain)
endif
include $(DEVKITPRO)/libnx/switch_rules

TARGET := roomcircuit-system-probe
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
STARTUP := ../emummc-startup
CFLAGS := $(ARCH) -O2 -g -Wall -Wextra -Werror -D__SWITCH__ -I$(LIBNX)/include -I$(STARTUP)
LDFLAGS := $(ARCH) -specs=$(LIBNX)/switch.specs -L$(LIBNX)/lib

.PHONY: all clean
all: $(TARGET).nro
$(TARGET).elf: source/main.c $(STARTUP)/startup.c $(STARTUP)/startup.h Makefile
	$(CC) $(CFLAGS) $< $(STARTUP)/startup.c $(LDFLAGS) -lnx -o $@
$(TARGET).nacp: Makefile
	nacptool --create "RoomCircuit System Probe" "RoomCircuit / OpenPak" "0.1.0" $@
$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
	elf2nro $< $@ --nacp=$(TARGET).nacp
clean:
	$(RM) $(TARGET).elf $(TARGET).nacp $(TARGET).nro
