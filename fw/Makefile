TC      := ../toolchain/root/usr
export LD_LIBRARY_PATH := $(TC)/lib/x86_64-linux-gnu
CC      := $(TC)/bin/m68k-linux-gnu-gcc-11
OBJCOPY := $(TC)/bin/m68k-linux-gnu-objcopy
OBJDUMP := $(TC)/bin/m68k-linux-gnu-objdump
SIZE    := $(TC)/bin/m68k-linux-gnu-size

CFLAGS  := $(EXTRA) -mcpu=52254 -Os -Wall -Wextra -ffreestanding -fno-pic -fno-common \
           -ffunction-sections -fdata-sections
LDFLAGS := -nostdlib -static -no-pie -T link.ld -Wl,--gc-sections -Wl,--build-id=none

PROG ?= main
OBJS := start.o $(PROG).o minilibc.o

ifeq ($(APP),1)          # image for the USB bootloader, linked at 0x4000
CFLAGS  += -DAPP_AT_0x4000
LDFLAGS += -Wl,--defsym=__app_base=0x4000
endif

fw.bin: fw.elf
	$(OBJCOPY) -O binary $< $@
	$(SIZE) $<

fw.elf: $(OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

dis: fw.elf
	$(OBJDUMP) -d $<

clean:
	rm -f *.o fw.elf fw.bin

.PHONY: dis clean
