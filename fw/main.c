/* Test firmware for the SafeQ MCF52254 board.
 *
 * Square wave on the QSPI pins (CON5) so the RP2040 can check it runs:
 *   PQS0 = QSPI_DOUT, MCU pin 19, CON5-7 -> Pico GP4: PIT0 wave
 *          f = (fsys/2) / PIT_PERIOD / 2, so the Pico can compute fsys.
 *   PQS2 = QSPI_CLK,  MCU pin 20, CON5-5 -> Pico GP2: toggles every 64 periods
 *
 * Built with APP=1 (under the USB bootloader) it also prints a heartbeat on
 * the board's USB serial port via the bootloader services, and echoes input.
 */
#include <stdint.h>
#if defined(APP_AT_0x4000) && !defined(NO_USB)
#define USE_USB 1
#include "../boot/cfboot.h"
#endif

#define REG8(a)   (*(volatile uint8_t  *)(a))
#define REG16(a)  (*(volatile uint16_t *)(a))

#define IPSBAR    0x40000000u
#define PORTQS    REG8(IPSBAR + 0x10000C)
#define DDRQS     REG8(IPSBAR + 0x100024)
#define PQSPAR    REG16(IPSBAR + 0x10006C)

#define PCSR0     REG16(IPSBAR + 0x150000)
#define PMR0      REG16(IPSBAR + 0x150002)
#define PCSR_PIF  (1u << 2)
#define PCSR_RLD  (1u << 1)
#define PCSR_EN   (1u << 0)

#define PQS0      (1u << 0)
#define PQS2      (1u << 2)

#define PIT_PERIOD 6000u

#ifdef USE_USB
static const struct cfboot_svc *usb;

static void print(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    if (usb)
        usb->write((const uint8_t *)s, n);
}

static void print_dec(uint32_t v)
{
    char b[11];
    int i = 10;
    b[i] = 0;
    do {
        b[--i] = '0' + v % 10;
        v /= 10;
    } while (v);
    print(b + i);
}
#endif

int main(void)
{
#ifdef CRASH
    __builtin_trap();               /* test app: illegal instruction right away */
#endif
    PQSPAR &= ~0x0033u;             /* PQS0, PQS2 -> GPIO */
    PORTQS &= ~(PQS0 | PQS2);
    DDRQS |= PQS0 | PQS2;

    PCSR0 = 0;
    PMR0 = PIT_PERIOD - 1;
    PCSR0 = PCSR_RLD | PCSR_PIF;    /* prescaler /1, reload, clear flag */
    PCSR0 = PCSR_RLD | PCSR_EN;

#ifdef USE_USB
    if (CFBOOT_SVC->magic == CFBOOT_SVC_MAGIC) {
        usb = CFBOOT_SVC;
        usb->usb_init();
    }
#endif

    for (unsigned n = 0;; n++) {
        while (!(PCSR0 & PCSR_PIF)) {
#ifdef USE_USB
            if (usb)
                usb->usb_poll();
#endif
        }
        PCSR0 = PCSR_RLD | PCSR_EN | PCSR_PIF;
        PORTQS ^= PQS0;
        if ((n & 63) == 63)
            PORTQS ^= PQS2;
#ifdef USE_USB
        if (usb && n % 2000 == 0 && usb->connected()) {      /* every 0.5 s at 48 MHz */
            uint8_t c[32];
            int k = usb->read(c, sizeof c);
            print("app tick ");
            print_dec(n / 2000);
            if (k > 0) {
                print(" got: ");
                usb->write(c, k);
            }
            print("\r\n");
        }
#endif
    }
}
