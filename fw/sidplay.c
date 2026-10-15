/* SID player for the SafeQ MCF52254 board.
 *
 * Emulates the C64 SID chip (3 voices: triangle / sawtooth / pulse / noise,
 * ring modulation, hard sync, ADSR envelopes, multimode filter, master volume)
 * in a 31.25 kHz PIT1 interrupt and plays it through PWM0 on MCU pin 5 (the
 * piezo) used as an 8-bit DAC. The RGB LED (LP5562 on I2C1) follows the three
 * voices' envelopes.
 *
 * Tunes come from a bank in flash (sid/mkbank.py, played from power-up, the
 * button skips to the next one) or are streamed over USB by sid/sidplay.py,
 * which takes over while it streams (the C64 tune itself runs on the PC, see
 * sid/sid2log.py). Host -> board packets:
 *   'T' u16 frame_us        start a tune: reset SID, set frame period
 *   'F' u32 mask, values    one frame: registers whose mask bit is set
 *   'X'                     stop
 *   'G' u8 gain             fixed output gain when AGC is off (default 64)
 *   'A' u8 boost            AGC maximum boost, 0 = AGC off (default 12)
 *   'Q'                     status line back to the host
 * Board -> host: "BTN\n" when the button on CON5-5 (MCU pin 20, PQS2, to GND)
 * is pressed. The board then silences itself at once and discards frames until
 * the next 'T', so skipping does not wait for buffered USB data.
 * When the frame FIFO is full the board stops reading USB, which makes the host
 * block: playback speed is set by the board's sample clock.
 */
#pragma GCC optimize("O2")
#include <stdint.h>
#include "../boot/cfboot.h"
#include "svf_tables.h"

#define REG8(a)   (*(volatile uint8_t  *)(0x40000000u + (a)))
#define REG16(a)  (*(volatile uint16_t *)(0x40000000u + (a)))
#define REG32(a)  (*(volatile uint32_t *)(0x40000000u + (a)))

/* Hot code runs from SRAM: the MCF5225x has no instruction cache, and code in
 * flash stalls on wait states (the ISR took ~2x longer from flash). */
#define HOT __attribute__((optimize("O2"), section(".ramfunc")))

/* ---- hardware ------------------------------------------------------------- */
#define PTCPAR    REG8(0x10006F)          /* PTC0 = pin 5: 11 = PWM0 */
#define PUCPAR    REG8(0x100073)          /* PUC2/3 = pins 65/64: 10 = I2C1 */
#define PWME      REG8(0x1B0000)
#define PWMPOL    REG8(0x1B0001)
#define PWMCLK    REG8(0x1B0002)
#define PWMPRCLK  REG8(0x1B0003)
#define PWMCAE    REG8(0x1B0004)
#define PWMCTL    REG8(0x1B0005)
#define PWMPER0   REG8(0x1B0014)
#define PWMDTY0   REG8(0x1B001C)
#define PCSR1     REG16(0x160000)
#define PMR1      REG16(0x160002)
#define PCNTR1    REG16(0x160004)
#define PCSR_RUN  0x000Bu                 /* PIE | RLD | EN */
#define PCSR_PIF  0x0004u
#define IMRH0     REG32(0x000C08)
#define IMRL0     REG32(0x000C0C)
#define ICR0(n)   REG8(0x000C40 + (n))
#define PIT1_SRC  56
#define PQSPAR    REG16(0x10006C)
#define DDRQS     REG8(0x100024)
#define PINQS     REG8(0x10003C)          /* pin state on read */
#define I2FDR1    REG8(0x000384)
#define I2CR1     REG8(0x000388)
#define I2SR1     REG8(0x00038C)
#define I2DR1     REG8(0x000390)

#define RATE      31250u
#define PIT_DIV   (24000000u / RATE)      /* PIT1 runs at fsys/2 */
#define CYC_STEP  8071u                   /* PAL SID cycles per sample << 8 (31.528) */
#define CYC_ONE   256u

static volatile uint32_t * const cf_vectors = (uint32_t *)0x20000000;
static const struct cfboot_svc *usb;

/* ---- SID emulation ----------------------------------------------------------- */
enum { ENV_ATTACK, ENV_DECAY, ENV_RELEASE };

typedef struct {
    uint32_t acc;                 /* 24-bit SID accumulator << 8 */
    uint32_t step;                /* per sample */
    uint32_t lfsr;                /* 23-bit noise register */
    uint32_t rate_cnt;            /* SID cycles << 8 towards the next env step */
    uint32_t rate_per;            /* current rate period, SID cycles << 8 */
    uint16_t pw;                  /* 12-bit pulse width */
    uint8_t  ctrl, ad, sr;
    uint8_t  env, state, exp_cnt;
    uint8_t  msb_rose;
} voice_t;

static const uint16_t rate_cycles[16] = {
    9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251
};

static voice_t v[3];
static uint8_t regs[25];
static int32_t f_lp, f_bp;
static uint16_t f_f, f_q;
static uint8_t f_route, f_mode, vol;
static volatile uint8_t gain = 64;

/* AGC: out = m * agc_g >> 20 (m = mixed voices x volume, +-92160 at most).
 * Unity (agc_g = AGC_UNITY) maps full SID scale to full DAC scale; quieter
 * passages get up to agc_boost x more. Peak: instant attack, ~0.5 s release. */
#define AGC_UNITY 1445                  /* (127 << 20) / 92160 */
static volatile uint8_t agc_boost = 12;
static int32_t agc_peak, agc_g = AGC_UNITY;
static uint8_t agc_tick;

static HOT void set_rate(voice_t *x)
{
    uint8_t r = x->state == ENV_ATTACK ? x->ad >> 4 :
                x->state == ENV_DECAY  ? x->ad & 15 : x->sr & 15;
    x->rate_per = (uint32_t)rate_cycles[r] * CYC_ONE;
}

static void sid_reset(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        v[i] = (voice_t){ 0 };
        v[i].lfsr = 0x7FFFF8;
        v[i].state = ENV_RELEASE;
        set_rate(&v[i]);
    }
    for (i = 0; i < 25; i++)
        regs[i] = 0;
    f_lp = f_bp = 0;
    f_f = svf_f[0];
    f_q = svf_q[0];
    f_route = f_mode = vol = 0;
}

static HOT void sid_write(uint8_t r, uint8_t val)
{
    regs[r] = val;
    if (r < 21) {
        voice_t *x = &v[r / 7];
        switch (r % 7) {
        case 0: case 1: {
            uint16_t freq = regs[r - r % 7] | regs[r - r % 7 + 1] << 8;
            x->step = freq * CYC_STEP;
            break;
        }
        case 2: case 3:
            x->pw = (regs[r - r % 7 + 2] | regs[r - r % 7 + 3] << 8) & 0xFFF;
            break;
        case 4:
            if ((val & 1) && !(x->ctrl & 1)) {            /* gate on */
                x->state = ENV_ATTACK;
                set_rate(x);
            } else if (!(val & 1) && (x->ctrl & 1)) {     /* gate off */
                x->state = ENV_RELEASE;
                set_rate(x);
            }
            if (val & 8) {                                /* test bit */
                x->acc = 0;
                x->lfsr = 0x7FFFF8;
            }
            x->ctrl = val;
            break;
        case 5: x->ad = val; set_rate(x); break;
        case 6: x->sr = val; set_rate(x); break;
        }
        return;
    }
    switch (r) {
    case 21: case 22:
        f_f = svf_f[((regs[21] & 7) | regs[22] << 3) >> 3];
        break;
    case 23:
        f_q = svf_q[val >> 4];
        f_route = val & 7;
        break;
    case 24:
        f_mode = val >> 4;
        vol = val & 15;
        break;
    }
}

/* one 12-bit waveform sample of voice i (accumulators already advanced) */
static inline HOT uint16_t wave(voice_t *x, voice_t *src)
{
    uint32_t a = x->acc >> 8;                 /* 24-bit accumulator */
    uint16_t out = 0xFFF;
    uint8_t w = x->ctrl >> 4;
    if (!w)
        return 0x800;                         /* no waveform: mid level */
    if (w & 1) {                              /* triangle (+ ring mod) */
        uint32_t msb = a & 0x800000;
        if (x->ctrl & 4)
            msb ^= (src->acc >> 8) & 0x800000;
        out &= ((msb ? ~a : a) >> 11) & 0xFFF;
    }
    if (w & 2)                                /* sawtooth */
        out &= a >> 12;
    if (w & 4)                                /* pulse */
        out &= ((a >> 12) >= x->pw || (x->ctrl & 8)) ? 0xFFF : 0;
    if (w & 8) {                              /* noise */
        uint32_t l = x->lfsr;
        out &= ((l >> 11) & 0x800) | ((l >> 10) & 0x400) | ((l >> 7) & 0x200) |
               ((l >> 5) & 0x100) | ((l >> 4) & 0x080) | ((l >> 1) & 0x040) |
               ((l << 1) & 0x020) | ((l << 2) & 0x010);
    }
    return out;
}

static inline HOT void envelope(voice_t *x)
{
    /* idle fast paths: nothing changes until the next gate edge */
    if (x->state != ENV_ATTACK &&
        (x->env == 0 || (x->state == ENV_DECAY && x->env == (x->sr >> 4) * 17))) {
        x->rate_cnt = 0;
        return;
    }
    x->rate_cnt += CYC_STEP;
    while (x->rate_cnt >= x->rate_per) {
        x->rate_cnt -= x->rate_per;
        if (x->state == ENV_ATTACK) {
            if (++x->env == 255) {
                x->state = ENV_DECAY;
                set_rate(x);
            }
            continue;
        }
        /* decay / release: exponential slow-down at low levels */
        {
            uint8_t e = x->env;
            uint8_t per = e > 93 ? 1 : e > 54 ? 2 : e > 26 ? 4 : e > 14 ? 8 : e > 6 ? 16 : 30;
            if (++x->exp_cnt < per)
                continue;
            x->exp_cnt = 0;
        }
        if (x->state == ENV_DECAY && x->env <= (x->sr >> 4) * 17)
            continue;                         /* sustain level reached */
        if (x->env)
            x->env--;
    }
}

/* ---- frame FIFO (main loop -> ISR) ---------------------------------------------- */
#define FIFO_N 128
typedef struct { uint32_t mask; uint8_t val[25]; } frame_t;
static frame_t fifo[FIFO_N];
static volatile uint16_t f_head, f_tail;
static volatile uint32_t frame_len = 625u << 16;   /* samples per frame, 16.16 */
static volatile uint32_t frame_pos, underruns, frames_played;
static volatile uint8_t playing;
static uint8_t skipping;                /* button pressed: drop frames until 'T' */

/* ---- tune bank in flash at 0x10000 (sid/mkbank.py) ------------------------------- */
#include "../sid/sidz_dec.h"
#define BANK ((const uint8_t *)0x10000)
static uint16_t bank_count, bank_tune;
static uint32_t bank_left;              /* frames still to decode for this tune */
static sidz_dec_t dec[4];
static uint8_t stream_mode;             /* host is streaming: bank paused */
static uint32_t stream_last;            /* isr_n of the last streamed frame */

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static HOT void next_frame(void)
{
    frame_t *fr;
    uint8_t r, k = 0;
    if (f_head == f_tail) {
        if (playing)
            underruns++;
        return;
    }
    fr = &fifo[f_tail % FIFO_N];
    for (r = 0; r < 25; r++)
        if (fr->mask & (1u << r))
            sid_write(r, fr->val[k++]);
    f_tail++;
    frames_played++;
}

/* ---- the 31.25 kHz interrupt --------------------------------------------------------- */
static volatile uint32_t isr_cycles_max, isr_cycles_sum, isr_n;

__attribute__((interrupt_handler)) static HOT void pit1_isr(void)
{
    int i;
    int32_t direct = 0, filt = 0, out;
    uint8_t rose[3];

    PCSR1 = PCSR_RUN | PCSR_PIF;

    frame_pos += 1u << 16;
    if (frame_pos >= frame_len) {
        frame_pos -= frame_len;
        next_frame();
    }

    for (i = 0; i < 3; i++) {                   /* oscillators */
        voice_t *x = &v[i];
        uint32_t old = x->acc;
        if (x->ctrl & 8)
            continue;
        x->acc += x->step;
        rose[i] = !(old & 0x80000000u) && (x->acc & 0x80000000u);
        if ((x->ctrl & 0x80)) {                 /* noise: clock on acc bit 19 rising */
            /* rising edges of acc bit 27 (= SID bit 19) this sample; mod 16
             * because the accumulator wraps (a plain difference underflowed
             * to ~4e9 and the ISR never returned) */
            uint32_t n = (((x->acc + (1u << 27)) >> 28) - ((old + (1u << 27)) >> 28)) & 15;
            while (n--) {
                uint32_t b = ((x->lfsr >> 22) ^ (x->lfsr >> 17)) & 1;
                x->lfsr = ((x->lfsr << 1) | b) & 0x7FFFFF;
            }
        }
    }
    for (i = 0; i < 3; i++)                     /* hard sync from the previous voice */
        if ((v[i].ctrl & 2) && rose[(i + 2) % 3] && !(v[(i + 2) % 3].ctrl & 8))
            v[i].acc = 0;

    for (i = 0; i < 3; i++) {
        voice_t *x = &v[i];
        int32_t s;
        envelope(x);
        s = ((int32_t)wave(x, &v[(i + 2) % 3]) - 0x800) * x->env >> 8;   /* +-2047 */
        if (f_route & (1 << i))
            filt += s;
        else if (i != 2 || !(f_mode & 8))     /* 3OFF mutes unfiltered voice 3 */
            direct += s;
    }

    if (f_mode & 7) {                           /* state-variable filter, Q12 */
        int32_t hp = filt - f_lp - ((f_q * f_bp) >> 12);
        f_bp += (f_f * hp) >> 12;
        f_lp += (f_f * f_bp) >> 12;
        filt = 0;
        if (f_mode & 1) filt += f_lp;
        if (f_mode & 2) filt += f_bp;
        if (f_mode & 4) filt += hp;
    }

    out = (direct + filt) * vol;                /* +-92k for 3 full voices */
    if (agc_boost) {
        int32_t a = out < 0 ? -out : out;
        if (a > agc_peak)
            agc_peak = a;
        else
            agc_peak -= agc_peak >> 14;
        if (!(++agc_tick & 63)) {               /* every 2 ms */
            int32_t p = agc_peak < 1500 ? 1500 : agc_peak;
            int32_t t = (127 << 20) / p;
            int32_t lim = AGC_UNITY * agc_boost;
            if (t > lim)
                t = lim;
            agc_g += (t - agc_g) >> 2;
        }
        out = ((out >> 4) * agc_g) >> 16;
    } else {
        out = (out * gain) >> 15;
    }
    if (!vol || !(v[0].env | v[1].env | v[2].env))
        PWMDTY0 = 0;                            /* silence: transistor off */
    else
        PWMDTY0 = out < -128 ? 0 : out > 127 ? 255 : out + 128;

    {                                           /* load measurement */
        uint32_t used = PIT_DIV - PCNTR1;
        isr_cycles_sum += used;
        if (used > isr_cycles_max)
            isr_cycles_max = used;
        isr_n++;
    }
}

static void audio_init(void)
{
    sid_reset();
    PTCPAR = (PTCPAR & ~0x03) | 0x03;           /* pin 5 = PWM0 */
    PWME = 0;
    PWMPOL |= 0x01;
    PWMCLK &= ~0x01;
    PWMPRCLK &= ~0x07;
    PWMCAE &= ~0x01;
    PWMCTL = 0;
    PWMPER0 = 255;
    PWMDTY0 = 0;
    PWME |= 0x01;

    cf_vectors[64 + PIT1_SRC] = (uint32_t)pit1_isr;
    PCSR1 = 0;
    PMR1 = PIT_DIV - 1;
    PCSR1 = PCSR_RUN | PCSR_PIF;
    ICR0(PIT1_SRC) = 6 << 3;
    IMRH0 &= ~(1u << (PIT1_SRC - 32));
    IMRL0 &= ~1u;
    __asm__ volatile ("move.w #0x2000, %%sr" ::: "memory");
}

/* ---- RGB LED: LP5562 on I2C1 (0x30) ------------------------------------------------- */
static int i2c_wait(void)
{
    uint32_t n = 20000;
    while (!(I2SR1 & 0x02))
        if (!--n)
            return -1;
    I2SR1 = 0;
    return 0;
}

static void led_write(uint8_t reg, uint8_t val)
{
    uint32_t n = 20000;
    while ((I2SR1 & 0x20) && --n)               /* bus busy */
        ;
    I2CR1 = 0x90;                               /* IEN | MTX */
    I2CR1 = 0xB0;                               /* + MSTA: START */
    I2DR1 = 0x30 << 1;
    if (!i2c_wait()) {
        I2DR1 = reg;
        if (!i2c_wait()) {
            I2DR1 = val;
            i2c_wait();
        }
    }
    I2CR1 = 0x80;                               /* STOP */
}

static void led_init(void)
{
    volatile uint32_t d;
    PUCPAR = (PUCPAR & ~0xF0) | 0xA0;
    I2CR1 = 0;
    I2FDR1 = 0x13;
    I2CR1 = 0x80;
    I2SR1 = 0;
    led_write(0x00, 0x40);                      /* CHIP_EN */
    for (d = 0; d < 20000; d++)
        ;
    led_write(0x08, 0x01);                      /* internal clock */
    led_write(0x70, 0x00);                      /* PWM from I2C registers */
    led_write(0x04, 0);
    led_write(0x03, 0);
    led_write(0x02, 0);
}

static void led_update(void)
{
    static uint8_t last[3];
    static const uint8_t reg[3] = { 0x04, 0x03, 0x02 };     /* R, G, B */
    int i;
    for (i = 0; i < 3; i++) {
        uint8_t e = (v[i].ctrl >> 4) ? v[i].env : 0;
        uint8_t b = (uint8_t)((e * e * 4 / 5) >> 9);   /* gamma-ish, max ~101 (80 %) */
        if (b != last[i]) {
            led_write(reg[i], b);
            last[i] = b;
        }
    }
}

/* ---- USB stream parser ------------------------------------------------------------------ */
static void print(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    usb->write((const uint8_t *)s, n);
}

static void print_dec(uint32_t x)
{
    char b[11];
    int i = 10;
    b[i] = 0;
    do {
        b[--i] = '0' + x % 10;
        x /= 10;
    } while (x);
    print(b + i);
}

static void status(void)
{
    uint32_t n = isr_n;
    print("SIDPLAY mode=");
    print(stream_mode ? "stream" : bank_count ? "bank" : "idle");
    print(" tune=");
    print_dec(bank_tune);
    print("/");
    print_dec(bank_count);
    print(" fifo=");
    print_dec((uint16_t)(f_head - f_tail));
    print(" played=");
    print_dec(frames_played);
    print(" underruns=");
    print_dec(underruns);
    print(" isr_avg=");
    print_dec(n ? isr_cycles_sum / n * 100 / PIT_DIV : 0);
    print("% isr_max=");
    print_dec(isr_cycles_max * 100 / PIT_DIV);
    print("% agc_gain=");
    print_dec(agc_boost ? (uint32_t)agc_g * 10 / AGC_UNITY : 0);
    print("/10\n");
}

static uint8_t popcount25(uint32_t m)
{
    uint8_t c = 0;
    for (m &= 0x1FFFFFF; m; m &= m - 1)
        c++;
    return c;
}

static void bank_start(uint16_t i);

static void stop_playback(void)
{
    uint16_t sr;
    __asm__ volatile ("move.w %%sr, %0; move.w #0x2700, %%sr" : "=d"(sr) :: "memory");
    f_head = f_tail = 0;
    sid_reset();
    playing = 0;
    __asm__ volatile ("move.w %0, %%sr" :: "d"(sr) : "memory");
}

/* button on PQS2: internal pull-up, pressed = 0; 20 ms debounce */
static void button_init(void)
{
    PQSPAR &= ~(3u << 4);
    DDRQS &= ~(1u << 2);
}

static void button_poll(void)
{
    static uint32_t last_tick;
    static uint8_t stable = 1, count;
    uint8_t now;
    if (isr_n - last_tick < RATE / 1000)        /* sample once per ms */
        return;
    last_tick = isr_n;
    now = (PINQS >> 2) & 1;
    if (now == stable) {
        count = 0;
        return;
    }
    if (++count < 20)
        return;
    stable = now;
    count = 0;
    if (!now) {                                 /* pressed */
        if (stream_mode) {
            skipping = 1;
            stop_playback();
            print("BTN\n");
        } else if (bank_count) {
            bank_start((bank_tune + 1) % bank_count);
        }
    }
}

static void bank_start(uint16_t i)
{
    const uint8_t *e = BANK + 8 + 64 * i;
    int g;
    stop_playback();
    frame_len = (uint32_t)(e[16] | e[17] << 8) << 11;
    frame_pos = 0;
    for (g = 0; g < 4; g++)
        sidz_init(&dec[g], BANK + le32(e + 4 * g));
    bank_left = le32(e + 20);
    bank_tune = i;
    underruns = frames_played = 0;
    agc_peak = 0;
    agc_g = AGC_UNITY;
}

/* decode bank frames into the FIFO; start the next tune when one ends */
static void bank_feed(void)
{
    if (stream_mode || !bank_count)
        return;
    while (bank_left && (uint16_t)(f_head - f_tail) < FIFO_N) {
        frame_t *fr = &fifo[f_head % FIFO_N];
        uint8_t reg[8], val[8];
        int g, n, i, k = 0;
        fr->mask = 0;
        for (g = 0; g < 4; g++) {
            n = sidz_next(&dec[g], reg, val);
            for (i = 0; i < n; i++) {
                fr->mask |= 1u << reg[i];
                fr->val[k++] = val[i];
            }
        }
        f_head++;
        bank_left--;
        playing = 1;
    }
    if (!bank_left && f_head == f_tail)
        bank_start((bank_tune + 1) % bank_count);
}

/* incremental packet parser: returns when it needs more bytes or the FIFO is full */
static void parse(void)
{
    static uint8_t pkt[32];
    static uint8_t have, need;
    uint8_t c;

    for (;;) {
        if (have == 0) {
            if ((uint16_t)(f_head - f_tail) >= FIFO_N)
                return;                         /* full: leave data in USB */
            if (usb->read(&c, 1) != 1)
                return;
            pkt[have++] = c;
            need = c == 'F' ? 5 : c == 'T' ? 3 : (c == 'G' || c == 'A') ? 2 : 1;
        }
        while (have < need) {
            if (usb->read(&c, 1) != 1)
                return;
            pkt[have++] = c;
            if (pkt[0] == 'F' && have == 5)
                need = 5 + popcount25(pkt[1] | pkt[2] << 8 | (uint32_t)pkt[3] << 16 |
                                      (uint32_t)pkt[4] << 24);
        }
        switch (pkt[0]) {
        case 'F': {
            frame_t *fr = &fifo[f_head % FIFO_N];
            uint8_t i;
            if (skipping || !stream_mode)
                break;                          /* old tune still arriving */
            stream_last = isr_n;
            fr->mask = pkt[1] | pkt[2] << 8 | (uint32_t)pkt[3] << 16 | (uint32_t)pkt[4] << 24;
            for (i = 0; i < have - 5; i++)
                fr->val[i] = pkt[5 + i];
            f_head++;
            playing = 1;
            break;
        }
        case 'T': {
            uint16_t us = pkt[1] | pkt[2] << 8;
            uint16_t sr = 0;
            __asm__ volatile ("move.w %%sr, %0; move.w #0x2700, %%sr" : "=d"(sr) :: "memory");
            f_head = f_tail = 0;
            sid_reset();
            agc_peak = 0;
            agc_g = AGC_UNITY;
            frame_len = (uint32_t)us << 11;      /* us / 32 samples, 16.16 */
            frame_pos = 0;
            underruns = frames_played = 0;
            playing = 0;
            skipping = 0;
            stream_mode = 1;
            stream_last = isr_n;
            __asm__ volatile ("move.w %0, %%sr" :: "d"(sr) : "memory");
            break;
        }
        case 'X':                               /* host done: back to the bank */
            stop_playback();
            if (stream_mode && bank_count) {
                stream_mode = 0;
                bank_start((bank_tune + 1) % bank_count);
            }
            stream_mode = 0;
            break;
        case 'G':
            gain = pkt[1];
            break;
        case 'A':
            agc_boost = pkt[1];
            break;
        case 'Q':
            status();
            break;
        }
        have = 0;
    }
}

int main(void)
{
    uint32_t led_tick = 0;

    usb = CFBOOT_SVC;
    audio_init();
    led_init();
    button_init();
    if (BANK[0] == 'S' && BANK[1] == 'I' && BANK[2] == 'D' && BANK[3] == 'B')
        bank_count = BANK[6] | BANK[7] << 8;
    if (bank_count)
        bank_start(0);
    usb->usb_init();
    for (;;) {
        usb->usb_poll();
        parse();
        button_poll();
        bank_feed();
        if (stream_mode && f_head == f_tail && isr_n - stream_last > 3 * RATE) {
            stream_mode = 0;                    /* host vanished mid-stream */
            if (bank_count)
                bank_start((bank_tune + 1) % bank_count);
        }
        if (isr_n - led_tick >= RATE / 30) {    /* LED at ~30 Hz */
            led_tick = isr_n;
            led_update();
        }
    }
}
