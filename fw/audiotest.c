/* Audio path test: PWM0 on MCU pin 5 (piezo) as an 8-bit DAC, PIT1 interrupt
 * at 31.25 kHz synthesising one voice. Controlled from the USB console:
 *
 *   q F   square  F Hz        t F  triangle      w F  sawtooth
 *   s F   sine    F Hz        n F  noise (LFSR clocked at F Hz)
 *   v N   volume 0..255       x    stop          d    demo sweep of all waves
 *   ?     help + ISR load
 */
#include <stdint.h>
#include "../boot/cfboot.h"

#define REG8(a)   (*(volatile uint8_t  *)(0x40000000u + (a)))
#define REG16(a)  (*(volatile uint16_t *)(0x40000000u + (a)))
#define REG32(a)  (*(volatile uint32_t *)(0x40000000u + (a)))

/* pin mux: PTC0 (pin 5) = 11 -> PWM0 */
#define PTCPAR    REG8(0x10006F)
/* PWM module (RM ch. 31) */
#define PWME      REG8(0x1B0000)
#define PWMPOL    REG8(0x1B0001)
#define PWMCLK    REG8(0x1B0002)
#define PWMPRCLK  REG8(0x1B0003)
#define PWMCAE    REG8(0x1B0004)
#define PWMCTL    REG8(0x1B0005)
#define PWMPER0   REG8(0x1B0014)
#define PWMDTY0   REG8(0x1B001C)
/* PIT1 (RM ch. 24), clocked at fsys/2 = 24 MHz */
#define PCSR1     REG16(0x160000)
#define PMR1      REG16(0x160002)
#define PCSR_PIE  (1u << 3)
#define PCSR_PIF  (1u << 2)
#define PCSR_RLD  (1u << 1)
#define PCSR_EN   (1u << 0)
/* interrupt controller 0 (RM ch. 16): PIT1 = source 56 -> vector 64+56 */
#define IMRH0     REG32(0x000C08)
#define IMRL0     REG32(0x000C0C)
#define ICR0(n)   REG8(0x000C40 + (n))
#define PIT1_SRC  56

#define RATE      31250u
#define PIT_DIV   (24000000u / RATE)           /* 768 */

static volatile uint32_t * const cf_vectors = (uint32_t *)0x20000000;
static const struct cfboot_svc *usb;

/* ---- one test voice ------------------------------------------------------ */
enum { OFF, SQUARE, TRIANGLE, SAW, SINE, NOISE };
static volatile uint8_t wave = OFF, volume = 200;
static volatile uint32_t phase, step;
static uint16_t lfsr = 0x7FFF;
static int8_t noise_out;
static volatile uint32_t isr_count, isr_busy;   /* load measurement */

static const int8_t sine_tab[64] = {           /* quarter period, 0..127 */
    0, 3, 6, 9, 12, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46,
    49, 51, 54, 57, 60, 63, 65, 68, 71, 73, 76, 78, 81, 83, 85, 88,
    90, 92, 94, 96, 98, 100, 102, 104, 106, 107, 109, 111, 112, 113, 115, 116,
    117, 118, 120, 121, 122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127
};

static int8_t sine(uint8_t p)
{
    uint8_t q = p & 63;
    switch (p >> 6) {
    case 0:  return sine_tab[q];
    case 1:  return sine_tab[63 - q];
    case 2:  return -sine_tab[q];
    default: return -sine_tab[63 - q];
    }
}

__attribute__((interrupt_handler)) static void pit1_isr(void)
{
    int s = 0;
    uint32_t old = phase;
    uint8_t p;

    PCSR1 = PCSR_RLD | PCSR_PIE | PCSR_EN | PCSR_PIF;   /* ack */
    phase += step;
    p = phase >> 24;
    switch (wave) {
    case SQUARE:   s = (p & 0x80) ? 127 : -127; break;
    case SAW:      s = (int)p - 128; break;
    case TRIANGLE: s = (p & 0x80) ? 383 - 2 * (int)p : 2 * (int)p - 128; break;
    case SINE:     s = sine(p); break;
    case NOISE:
        if ((phase ^ old) & 0x80000000u) {        /* clock LFSR twice per period */
            uint16_t bit = (lfsr ^ (lfsr >> 1)) & 1;
            lfsr = (lfsr >> 1) | (bit << 14);
        }
        if ((phase ^ old) & 0x40000000u)
            noise_out = (lfsr & 1) ? 127 : -127;
        s = noise_out;
        break;
    default:
        PWMDTY0 = 0;                              /* silent: transistor off */
        isr_count++;
        return;
    }
    s = 128 + (s * volume) / 256;
    PWMDTY0 = s < 0 ? 0 : s > 255 ? 255 : s;
    isr_count++;
    isr_busy += PMR1 - REG16(0x160004);           /* PIT ticks spent so far */
}

static void audio_init(void)
{
    PTCPAR = (PTCPAR & ~0x03) | 0x03;             /* pin 5 = PWM0 */
    PWME = 0;
    PWMPOL |= 0x01;                               /* high for DTY counts */
    PWMCLK &= ~0x01;                              /* clock A */
    PWMPRCLK &= ~0x07;                            /* A = bus clock */
    PWMCAE &= ~0x01;                              /* left aligned */
    PWMCTL = 0;
    PWMPER0 = 255;
    PWMDTY0 = 0;
    PWME |= 0x01;

    cf_vectors[64 + PIT1_SRC] = (uint32_t)pit1_isr;
    PCSR1 = 0;
    PMR1 = PIT_DIV - 1;
    PCSR1 = PCSR_RLD | PCSR_PIE | PCSR_EN | PCSR_PIF;
    ICR0(PIT1_SRC) = (6 << 3) | 0;                /* level 6, priority 0 */
    IMRH0 &= ~(1u << (PIT1_SRC - 32));
    IMRL0 &= ~1u;                                 /* clear MASKALL */
    __asm__ volatile ("move.w #0x2000, %%sr" ::: "memory");   /* enable IRQs */
}

static void play(uint8_t w, uint32_t hz)
{
    step = hz * 137439u;                          /* 2^32 / 31250, < 0.0001 % error */
    wave = hz ? w : OFF;
}

/* ---- console ---------------------------------------------------------------- */
static void print(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
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

static void delay_ms(uint32_t ms)                  /* uses the sample clock */
{
    uint32_t end = isr_count + ms * (RATE / 1000);
    while ((int32_t)(isr_count - end) < 0)
        usb->usb_poll();
}

static void demo(void)
{
    static const uint8_t waves[] = { SQUARE, TRIANGLE, SAW, SINE };
    static const char *names[] = { "square", "triangle", "saw", "sine" };
    unsigned w;
    uint32_t f;
    for (w = 0; w < 4; w++) {
        print(names[w]);
        print(" sweep 200..3000 Hz\r\n");
        for (f = 200; f <= 3000; f += 20) {
            play(waves[w], f);
            delay_ms(12);
        }
    }
    print("noise\r\n");
    for (f = 500; f <= 16000; f *= 2) {
        play(NOISE, f);
        delay_ms(400);
    }
    play(OFF, 0);
}

static void help(void)
{
    uint32_t n = isr_count, b = isr_busy;
    print("\r\naudiotest: q/t/w/s/n F = square/triangle/saw/sine/noise at F Hz, "
          "v N volume, x stop, d demo\r\nsample rate ");
    print_dec(RATE);
    print(" Hz, ISR load ");
    print_dec(n ? b * 100 / n / PIT_DIV : 0);
    print("%\r\n");
}

static uint32_t num(const char *p)
{
    uint32_t v = 0;
    while (*p == ' ')
        p++;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0');
    return v;
}

int main(void)
{
    static char line[32];
    unsigned len = 0, was = 0;

    usb = CFBOOT_SVC;
    audio_init();
    usb->usb_init();
    for (;;) {
        uint8_t c;
        usb->usb_poll();
        if (usb->connected() && !was) {
            was = 1;
            help();
            print("> ");
        } else if (!usb->connected()) {
            was = 0;
        }
        if (usb->read(&c, 1) != 1)
            continue;
        if (c != '\r' && c != '\n') {
            if (len < sizeof line - 1) {
                line[len++] = c;
                usb->write(&c, 1);
            }
            continue;
        }
        line[len] = 0;
        print("\r\n");
        switch (line[0]) {
        case 'q': play(SQUARE, num(line + 1)); break;
        case 't': play(TRIANGLE, num(line + 1)); break;
        case 'w': play(SAW, num(line + 1)); break;
        case 's': play(SINE, num(line + 1)); break;
        case 'n': play(NOISE, num(line + 1)); break;
        case 'v': volume = num(line + 1); break;
        case 'x': play(OFF, 0); break;
        case 'd': demo(); break;
        case '?': help(); break;
        }
        len = 0;
        print("> ");
    }
}
