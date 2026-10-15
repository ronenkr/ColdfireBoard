/* MCF52254 startup: vector table, CFM config field, RAMBAR, C runtime init. */

        .section .vectors, "a"
        .long   __stack_top             /* 0: initial SSP */
        .long   _start                  /* 1: initial PC  */
        .rept   254
        .long   _default_handler
        .endr

#ifndef APP_AT_0x4000
/* CFM flash configuration field, 0x400..0x417 (RM table 18-1).
 * Backdoor key 0, CFMPROT 0 (no sector protected), CFMSACC 0, CFMDACC 0,
 * CFMSEC 0 (unsecured, backdoor disabled). Keeps the part re-flashable via EzPort. */
        .section .cfmconfig, "a"
        .long   0, 0                    /* backdoor comparison key */
        .long   0                       /* CFMPROT  */
        .long   0                       /* CFMSACC  */
        .long   0                       /* CFMDACC  */
        .long   0                       /* CFMSEC   */
#endif

        .text
        .globl  _start
_start:
        move.w  #0x2700, %sr            /* supervisor, interrupts masked */
        move.l  #0x20000201, %d0        /* RAMBAR: base 0x20000000, SPV, valid */
        movec   %d0, %rambar
        move.l  #__stack_top, %sp
#ifdef APP_AT_0x4000
        /* VBR ignores its low 20 bits (RM 3.2.6: 1 MB aligned), so a table at
         * 0x4000 cannot be used directly. Copy it to RAM at 0x20000000 (1 MB
         * aligned; the app's RAM starts at 0x20000400) and point VBR there.
         * Handlers can then be installed at run time: cf_vectors[n] = isr. */
        lea     __vector_base, %a0
        lea     0x20000000, %a1
        move.l  #256, %d1
0:      move.l  (%a0)+, (%a1)+
        subq.l  #1, %d1
        bne.s   0b
        move.l  #0x20000000, %d0
#else
        move.l  #__vector_base, %d0
#endif
        movec   %d0, %vbr

        /* copy .data from flash, zero .bss */
        lea     __data_load, %a0
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

/* Unexpected exception: software reset (RCR SOFTRST, RM 11.5.1) instead of
 * halting, so a crashed app always falls back to the USB bootloader. */
        .globl  _default_handler
_default_handler:
        move.l  #0x40110000, %a0
        move.b  #0x80, (%a0)
1:      bra.s   1b
