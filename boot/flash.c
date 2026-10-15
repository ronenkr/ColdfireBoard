/* CFM program/erase. The whole 512 KB flash is one logical block, so the
 * command launch + completion poll must run from SRAM (RM 18.1.1): these
 * functions live in .ramfunc, copied to RAM by start.S. Interrupts are masked. */
#include "mcf52254.h"
#include "boot.h"

#define RAMFUNC __attribute__((section(".ramfunc"), noinline))
#define BACKDOOR(off) (*(volatile uint32_t *)(CFM_BACKDOOR + (off)))

/* 48 MHz fsys: CFM clock is fsys/2 = 24 MHz (RM fig 7-1), /8 /16 -> 187.5 kHz.
 * Should the CFM run on full fsys instead, this gives 375 kHz: too fast
 * (incomplete erase/program, caught by verify) but never the < 150 kHz
 * overstress case. */
#define CFMCLKD_VAL (CFMCLKD_PRDIV8 | CFMCLKD_DIV(15))

void flash_init(void)
{
    if (!(CFMCLKD & CFMCLKD_DIVLD))      /* write-once per reset */
        CFMCLKD = CFMCLKD_VAL;
    CFMPROT = 0;                         /* temporary (LOCK = 0); reset reloads 0 */
}

static RAMFUNC int cfm_cmd(uint32_t off, uint32_t data, uint8_t cmd)
{
    while (!(CFMUSTAT & CFMUSTAT_CBEIF))
        ;
    if (CFMUSTAT & CFMUSTAT_PVIOL)       /* write-1-to-clear, one bit per write */
        CFMUSTAT = CFMUSTAT_PVIOL;
    if (CFMUSTAT & CFMUSTAT_ACCERR)
        CFMUSTAT = CFMUSTAT_ACCERR;
    BACKDOOR(off) = data;
    CFMCMD = cmd;
    CFMUSTAT = CFMUSTAT_CBEIF;           /* launch */
    while (!(CFMUSTAT & CFMUSTAT_CCIF))
        ;
    return (CFMUSTAT & (CFMUSTAT_PVIOL | CFMUSTAT_ACCERR)) ? -1 : 0;
}

/* Erase the page(s) holding addr. Pages are 4 KB per the CFM chapter but the
 * EzPort chapter says 2 KB sectors, so callers erase every 2 KB: harmless if
 * the page is really 4 KB (erased twice), correct if it is 2 KB. */
RAMFUNC int flash_erase_page(uint32_t addr)
{
    if (addr < APP_BASE || addr >= FLASH_END)
        return -2;
    return cfm_cmd(addr & ~3u, 0, CFMCMD_PAGE_ERASE);
}

RAMFUNC int flash_program(uint32_t addr, const uint32_t *words, unsigned nwords)
{
    if ((addr & 3) || addr < APP_BASE || addr + 4 * nwords > FLASH_END)
        return -2;
    while (nwords--) {
        if (*words != 0xFFFFFFFFu && cfm_cmd(addr, *words, CFMCMD_WORD_PROGRAM))
            return -1;
        addr += 4;
        words++;
    }
    return 0;
}
