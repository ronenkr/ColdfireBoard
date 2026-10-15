/* MCF52254: 512 KB flash at 0, 64 KB SRAM at 0x20000000 (via RAMBAR).
 * Standalone image: link at 0 (EzPort). Under the USB bootloader: make APP=1,
 * which links at 0x4000 and leaves the CFM config field to the bootloader. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 512K
    RAM   (rwx) : ORIGIN = __ram_base, LENGTH = __ram_size
}

ENTRY(_start)

__vector_base = DEFINED(__app_base) ? __app_base : 0;
/* app: 0x20000000-0x200003FF = RAM vector table, top 8K = bootloader USB state */
__ram_base = DEFINED(__app_base) ? 0x20000400 : 0x20000000;
__ram_size = DEFINED(__app_base) ? 55K : 64K;

SECTIONS
{
    .vectors __vector_base : { KEEP(*(.vectors)) } > FLASH
    .cfmconfig 0x00000400 : { KEEP(*(.cfmconfig)) } > FLASH
    .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH

    .data : {
        __data_start = .;
        *(.ramfunc*)          /* time-critical code, copied to SRAM by start.S */
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT > FLASH
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    __stack_top = ORIGIN(RAM) + LENGTH(RAM);

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
