/* UART1 debug output on QSPI_DOUT (MCU pin 19, CON5-7), polled, TX only use.
 * Divider 13: 115200 baud if the UART runs on fsys = 48 MHz (RM eqn 28-1),
 * 57600 if it runs on fsys/2 - either is a standard rate for the console. */
#include "mcf52254.h"
#include "boot.h"

void uart_init(void)
{
    PQSPAR = (PQSPAR & ~PQSPAR_UART1_MASK) | PQSPAR_UART1_VAL;
    UCR1 = UCR_RESET_RX;
    UCR1 = UCR_RESET_TX;
    UCR1 = UCR_RESET_ERROR;
    UCR1 = UCR_RESET_MR;
    UMR1_1 = UMR1_PM_NONE | UMR1_BC_8;
    UMR2_1 = UMR2_SB_1;
    UCSR1 = UCSR_BUSCLK;
    UIMR1 = 0;
    UBG1_1 = 0;
    UBG2_1 = 13;
    UCR1 = UCR_TX_ENABLE;
    UCR1 = UCR_RX_ENABLE;
}

void uart_putc(char c)
{
    if (c == '\n')
        uart_putc('\r');
    while (!(USR1 & USR_TXRDY))
        ;
    UTB1 = (uint8_t)c;
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

void uart_puthex(uint32_t v, int digits)
{
    while (digits--)
        uart_putc("0123456789abcdef"[(v >> (4 * digits)) & 15]);
}

/* ---- non-blocking debug log ------------------------------------------------ */
static char dbg_ring[1024];
static uint16_t dbg_head, dbg_tail;

void dbg_puts(const char *s)
{
#ifdef NO_DEBUG
    (void)s;
    return;
#endif
    while (*s && (uint16_t)(dbg_head - dbg_tail) < sizeof dbg_ring)
        dbg_ring[dbg_head++ % sizeof dbg_ring] = *s++;
}

void dbg_hex(uint32_t v)
{
    char b[9];
    int i = 8;
    b[8] = 0;
    do {
        b[--i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    dbg_puts(b + i);
}

void dbg_flush(void)
{
    if (dbg_tail != dbg_head && (USR1 & USR_TXRDY))
        UTB1 = (uint8_t)dbg_ring[dbg_tail++ % sizeof dbg_ring];
}

void dbg_drain(void)                     /* blocking: before reset / app start */
{
    while (dbg_tail != dbg_head) {
        while (!(USR1 & USR_TXRDY))
            ;
        UTB1 = (uint8_t)dbg_ring[dbg_tail++ % sizeof dbg_ring];
    }
}
