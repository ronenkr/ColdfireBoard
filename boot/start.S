/* USB DFU bootloader startup (MCF52254). Lives in flash 0x0000-0x3FFF. */

        .section .vectors, "a"
        .long   __stack_top             /* 0: initial SSP */
        .long   _start                  /* 1: initial PC  */
        .rept   254
        .long   _default_handler
        .endr

/* CFM flash configuration field, 0x400..0x417 (RM table 18-1): no protection,
 * unsecured. Protection is deliberately NOT used for the bootloader sector: it
 * would block EzPort recovery (CFMPROT is reloaded from here at every reset). */
        .section .cfmconfig, "a"
        .long   0, 0                    /* backdoor comparison key */
        .long   0                       /* CFMPROT  */
        .long   0                       /* CFMSACC  */
        .long   0                       /* CFMDACC  */
        .long   0                       /* CFMSEC   */

        .text
        .globl  _start
_start:
        move.w  #0x2700, %sr            /* supervisor, interrupts masked */
        move.l  #0x20000201, %d0        /* RAMBAR: base 0x20000000, SPV, valid */
        movec   %d0, %rambar
        moveq   #0, %d0
        movec   %d0, %vbr
        move.l  #__stack_top, %sp

        lea     __data_load, %a0        /* .data and .ramfunc are copied together */
        lea     __data_start, %a1
        lea     __data_end, %a2
1:      cmp.l   %a2, %a1
        bcc.s   2f
        move.l  (%a0)+, (%a1)+
        bra.s   1b
2:      lea     __bss_start, %a1
        lea     __bss_end, %a2
3:      cmp.l   %a2, %a1
        bcc.s   4f
        clr.l   (%a1)+
        bra.s   3b
4:      jsr     main
5:      bra.s   5b

/* void jump_to_app(uint32_t base): VBR = base, SP = [base], PC = [base+4] */
        .globl  jump_to_app
jump_to_app:
        move.l  4(%sp), %a0
        move.w  #0x2700, %sr
        move.l  %a0, %d0
        movec   %d0, %vbr
        move.l  (%a0), %sp
        move.l  4(%a0), %a0
        jmp     (%a0)

/* Unexpected exception: report vector + PC on the debug UART, then reset
 * (fault() in main.c) instead of halting with USB still attached. */
        .globl  _default_handler
_default_handler:
        move.l  4(%sp), -(%sp)          /* PC */
        move.l  4(%sp), -(%sp)          /* format/vector/SR word */
        jsr     fault
1:      bra.s   1b
