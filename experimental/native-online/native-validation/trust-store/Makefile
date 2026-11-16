ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO must point to the devkitPro toolchain)
endif
include $(DEVKITPRO)/libnx/switch_rules
TARGET := roomcircuit-native-trust
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
STARTUP := ../emummc-startup
CFLAGS := $(ARCH) -O2 -g -Wall -Wextra -Werror -D__SWITCH__ -I$(LIBNX)/include -I$(STARTUP)
LDFLAGS := $(ARCH) -specs=$(LIBNX)/switch.specs -L$(LIBNX)/lib
all: $(TARGET).nro
$(TARGET).elf: main.c store.c store.h ca_hash.h $(STARTUP)/startup.c $(STARTUP)/startup.h Makefile
	$(CC) $(CFLAGS) main.c store.c $(STARTUP)/startup.c $(LDFLAGS) -lnx -o $@
$(TARGET).nacp: Makefile
	nacptool --create "RoomCircuit Native Trust" "RoomCircuit" "0.1.0" $@
$(TARGET).nro: $(TARGET).elf $(TARGET).nacp romfs/ca.der
	elf2nro $< $@ --nacp=$(TARGET).nacp --romfsdir=romfs
