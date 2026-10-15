/* Bootloader: flash 0x0000-0x3FFF (16 KB). Application at 0x4000.
 * RAM: 0x20000000-0x2000DFFF belongs to the application (bootloader stack lives
 * there only while the bootloader runs); 0x2000E000-0x2000FFFF holds the
 * bootloader's data, which applications use through the service table. */
MEMORY
{
    FLASH   (rx)  : ORIGIN = 0x00000000, LENGTH = 16K
    BOOTRAM (rwx) : ORIGIN = 0x2000E000, LENGTH = 8K - 16
    NOINIT  (rw)  : ORIGIN = 0x2000FFF0, LENGTH = 16
}

ENTRY(_start)

SECTIONS
{
    .vectors 0x00000000 : { KEEP(*(.vectors)) } > FLASH
    .cfmconfig 0x00000400 : { KEEP(*(.cfmconfig)) } > FLASH
    .svc 0x00000418 : { KEEP(*(.svc)) } > FLASH
    .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH

    /* .ramfunc: flash erase/program code must run from SRAM (RM ch. 18) */
    .data : {
        __data_start = .;
        *(.ramfunc*)
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > BOOTRAM AT > FLASH
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > BOOTRAM

    .noinit (NOLOAD) : { *(.noinit*) } > NOINIT

    __stack_top = 0x2000E000;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
