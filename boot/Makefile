TC      := ../toolchain/root/usr
export LD_LIBRARY_PATH := $(TC)/lib/x86_64-linux-gnu
CC      := $(TC)/bin/m68k-linux-gnu-gcc-11
OBJCOPY := $(TC)/bin/m68k-linux-gnu-objcopy
OBJDUMP := $(TC)/bin/m68k-linux-gnu-objdump
SIZE    := $(TC)/bin/m68k-linux-gnu-size

CFLAGS  := $(EXTRA) -mcpu=52254 -Os -Wall -Wextra -ffreestanding -fno-pic -fno-common \
           -ffunction-sections -fdata-sections -I../fw
LDFLAGS := -nostdlib -static -no-pie -T link.ld -Wl,--gc-sections -Wl,--build-id=none

# Debug log on UART1 TX (MCU pin 19, CON5-7, 115200 8N1): make DEBUG=1.
ifneq ($(DEBUG),1)
CFLAGS += -DNO_DEBUG
endif

OBJS := start.o main.o uart.o usb.o flash.o svc.o


boot.bin: boot.elf
	$(OBJCOPY) -O binary $< $@
	$(SIZE) $<

boot.elf: $(OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

dis: boot.elf
	$(OBJDUMP) -d $<

clean:
	rm -f *.o boot.elf boot.bin

.PHONY: dis clean
