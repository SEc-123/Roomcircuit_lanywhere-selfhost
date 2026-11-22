ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO must point to the devkitPro toolchain)
endif
include $(DEVKITPRO)/libnx/switch_rules
TARGET := roomcircuit-native-trust
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
STARTUP := ../emummc-startup
CA_DER ?=
ifeq ($(strip $(CA_DER)),)
$(error Explicit CA_DER is required)
endif
CFLAGS := $(ARCH) -O2 -g -Wall -Wextra -Werror -D__SWITCH__ -I$(LIBNX)/include -I$(STARTUP) -Ibuild
LDFLAGS := $(ARCH) -specs=$(LIBNX)/switch.specs -L$(LIBNX)/lib
all: $(TARGET).nro
.PHONY: FORCE
FORCE:
build/rc_build_config.h: FORCE $(CA_DER) ../build-inputs/prepare.py
	python3 ../build-inputs/prepare.py --ca "$(CA_DER)" --build-dir build
$(TARGET).elf: main.c store.c store.h build/rc_build_config.h $(STARTUP)/startup.c $(STARTUP)/startup.h Makefile
	$(CC) $(CFLAGS) main.c store.c $(STARTUP)/startup.c $(LDFLAGS) -lnx -o $@
$(TARGET).nacp: Makefile
	nacptool --create "RoomCircuit Native Trust" "RoomCircuit" "0.1.0" $@
$(TARGET).nro: $(TARGET).elf $(TARGET).nacp build/rc_build_config.h
	elf2nro $< $@ --nacp=$(TARGET).nacp --romfsdir=build/romfs
