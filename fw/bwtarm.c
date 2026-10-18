/* Test app: arms the backup watchdog timer the way the original SafeQ firmware
 * left it (WMR then WCR, both write-once until the next power-on reset) and
 * then never feeds it, so the chip resets about 0.35 s later. Used to check
 * that the bootloader copes with a leftover watchdog (boot/boot.h bwt_service).
 *
 * After running this, only a power cycle disarms the watchdog again. */
#include <stdint.h>

#define BWT_WCR  (*(volatile uint16_t *)0x40140000)
#define BWT_WMR  (*(volatile uint16_t *)0x40140002)

int main(void)
{
    BWT_WMR = 0x0800;               /* timeout (WM+1)*4096 cycles of fsys/2 = 0.35 s */
    BWT_WCR = 0x0003;               /* EN = 1 (bit 1 reads as 1) */
    for (;;)
        ;
}
