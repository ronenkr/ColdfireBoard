/* Interactive hardware test over the board's USB console (cfboot.py console,
 * or any serial terminal on the "CF52254 app" port).
 *
 *   s                  scan I2C1 (MCU pins 64 SCL / 65 SDA) for devices
 *   r AA RR [N]        read N (default 1) registers from I2C device AA at reg RR
 *   W AA RR VV         write VV to register RR of device AA
 *   t F                tone of F Hz on the piezo (MCU pin 5, DTOUT0); t 0 = off
 *   w                  short melody
 *   ?                  help
 * Numbers for r/W are hex, the tone frequency is decimal.
 */
#include <stdint.h>
#include "../boot/cfboot.h"

#define REG8(a)   (*(volatile uint8_t  *)(0x40000000u + (a)))
#define REG16(a)  (*(volatile uint16_t *)(0x40000000u + (a)))
#define REG32(a)  (*(volatile uint32_t *)(0x40000000u + (a)))

#define FSYS 48000000u

/* GPIO (RM ch. 15) */
#define PORTTC  REG8(0x10000F)
#define DDRTC   REG8(0x100027)
#define PTCPAR  REG8(0x10006F)          /* PTC0 = pin 5: 00 GPIO, 10 DTOUT0, 11 PWM0 */
#define PUCPAR  REG8(0x100073)          /* PUC2 = pin 65, PUC3 = pin 64: 10 = I2C1 */

/* DMA timer 0 (RM ch. 26), clocked at fsys */
#define DTMR0   REG16(0x000400)
#define DTER0   REG8(0x000403)
#define DTRR0   REG32(0x000404)
#define DTCN0   REG32(0x00040C)
#define DTMR_OM_TOGGLE  (1u << 5)
#define DTMR_FRR        (1u << 3)
#define DTMR_CLK_BUS    (1u << 1)
#define DTMR_RST        (1u << 0)

/* I2C1 (RM ch. 29) */
#define I2FDR1  REG8(0x000384)
#define I2CR1   REG8(0x000388)
#define I2SR1   REG8(0x00038C)
#define I2DR1   REG8(0x000390)
#define I2CR_IEN  0x80
#define I2CR_MSTA 0x20
#define I2CR_MTX  0x10
#define I2CR_TXAK 0x08
#define I2CR_RSTA 0x04
#define I2SR_IBB  0x20
#define I2SR_IAL  0x10
#define I2SR_IIF  0x02
#define I2SR_RXAK 0x01

static const struct cfboot_svc *usb;

/* ---- console ------------------------------------------------------------- */
static void print(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    usb->write((const uint8_t *)s, n);
}

static void print_hex(uint32_t v, int digits)
{
    char b[9];
    int i;
    for (i = digits - 1; i >= 0; i--, v >>= 4)
        b[i] = "0123456789ABCDEF"[v & 15];
    b[digits] = 0;
    print(b);
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

static void delay_ms(uint32_t ms)       /* busy wait that keeps USB serviced */
{
    while (ms--) {
        uint32_t i;
        for (i = 0; i < 4000; i++)
            if ((i & 255) == 0)
                usb->usb_poll();
    }
}

/* ---- piezo on DTOUT0 ------------------------------------------------------- */
static void tone(uint32_t hz)
{
    DTMR0 = 0;                          /* reset timer */
    if (!hz) {
        PTCPAR &= ~0x03;                /* pin back to GPIO, driven low */
        PORTTC &= ~0x01;
        DDRTC |= 0x01;
        return;
    }
    DTRR0 = FSYS / (2 * hz) - 1;        /* toggle twice per period */
    DTCN0 = 0;
    DTER0 = 0x03;                       /* clear REF/CAP events */
    PTCPAR = (PTCPAR & ~0x03) | 0x02;   /* PTC0 = DTOUT0 */
    DTMR0 = DTMR_OM_TOGGLE | DTMR_FRR | DTMR_CLK_BUS | DTMR_RST;
}

/* ---- I2C1 master, polled ---------------------------------------------------- */
static int i2c_wait(void)               /* 0 = byte done, -1 = timeout/arb lost */
{
    uint32_t n = 200000;
    while (!(I2SR1 & I2SR_IIF))
        if (!--n)
            return -1;
    I2SR1 = 0;                          /* IIF/IAL clear by writing 0 */
    return (I2SR1 & I2SR_IAL) ? -1 : 0;
}

static void i2c_init(void)
{
    PUCPAR = (PUCPAR & ~0xF0) | 0xA0;   /* PUC2, PUC3 -> I2C_SDA1, I2C_SCL1 */
    I2CR1 = 0;
    I2FDR1 = 0x13;                      /* /480: 100 kHz at 48 MHz, 50 kHz at 24 MHz */
    I2CR1 = I2CR_IEN;
    I2SR1 = 0;
}

static void i2c_stop(void)
{
    uint32_t n = 100000;
    I2CR1 &= ~(I2CR_MSTA | I2CR_MTX | I2CR_TXAK);
    while ((I2SR1 & I2SR_IBB) && --n)
        ;
}

/* send START (or repeated START) + address; 0 = ACK */
static int i2c_start(uint8_t addr7, int read, int repeated)
{
    if (repeated) {
        I2CR1 |= I2CR_RSTA;
    } else {
        uint32_t n = 100000;
        while ((I2SR1 & I2SR_IBB) && --n)
            ;
        if (!n) {                       /* bus stuck: reset the module */
            i2c_init();
            return -2;
        }
        I2CR1 = I2CR_IEN | I2CR_MTX;
        I2CR1 = I2CR_IEN | I2CR_MTX | I2CR_MSTA;
    }
    I2DR1 = (uint8_t)(addr7 << 1 | (read ? 1 : 0));
    if (i2c_wait())
        return -2;
    return (I2SR1 & I2SR_RXAK) ? -1 : 0;
}

static int i2c_write_byte(uint8_t b)
{
    I2DR1 = b;
    if (i2c_wait())
        return -2;
    return (I2SR1 & I2SR_RXAK) ? -1 : 0;
}

static int i2c_read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, unsigned n)
{
    unsigned i;
    if (i2c_start(addr, 0, 0) || i2c_write_byte(reg) || i2c_start(addr, 1, 1)) {
        i2c_stop();
        return -1;
    }
    I2CR1 &= ~I2CR_MTX;                 /* receive */
    if (n == 1)
        I2CR1 |= I2CR_TXAK;             /* NAK the only byte */
    (void)I2DR1;                        /* dummy read starts the first byte */
    for (i = 0; i < n; i++) {
        if (i2c_wait()) {
            i2c_stop();
            return -1;
        }
        if (i == n - 1)
            I2CR1 &= ~I2CR_MSTA;        /* STOP before reading the last byte */
        else if (i == n - 2)
            I2CR1 |= I2CR_TXAK;
        buf[i] = I2DR1;
    }
    I2CR1 &= ~(I2CR_MTX | I2CR_TXAK);
    return 0;
}

static int i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
    int r = i2c_start(addr, 0, 0);
    if (!r)
        r = i2c_write_byte(reg);
    if (!r)
        r = i2c_write_byte(val);
    i2c_stop();
    return r;
}

static void i2c_scan(void)
{
    unsigned a, found = 0;
    print("I2C1 scan:");
    for (a = 0x03; a <= 0x77; a++) {
        int r = i2c_start(a, 0, 0);
        i2c_stop();
        if (r == 0) {
            print(" 0x");
            print_hex(a, 2);
            found++;
        } else if (r == -2) {
            print(" [bus error @");
            print_hex(a, 2);
            print("]");
        }
        usb->usb_poll();
    }
    print(found ? "\r\n" : " none\r\n");
}

/* ---- command line -------------------------------------------------------- */
static const char *num(const char *p, uint32_t *v, int base)
{
    *v = 0;
    while (*p == ' ')
        p++;
    for (;; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (base == 16 && *p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (base == 16 && *p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else return p;
        *v = *v * base + d;
    }
}

static void help(void)
{
    print("\r\nCF52254 hwtest: s=I2C scan, r AA RR [N]=read, W AA RR VV=write, "
          "t HZ=tone (0 off), w=melody, ?=help\r\n");
}

static void melody(void)
{
    static const uint16_t notes[] = { 523, 659, 784, 1047, 784, 659, 523, 0 };
    unsigned i;
    for (i = 0; i < sizeof notes / sizeof notes[0]; i++) {
        tone(notes[i]);
        delay_ms(150);
    }
    tone(0);
}

static void command(const char *line)
{
    uint32_t a, r, v;
    const char *p = line + 1;
    switch (line[0]) {
    case 's':
        i2c_scan();
        return;
    case 'r': {
        uint8_t buf[32];
        unsigned i;
        p = num(p, &a, 16);
        p = num(p, &r, 16);
        num(p, &v, 16);
        if (!v || v > sizeof buf)
            v = 1;
        if (i2c_read_regs(a, r, buf, v)) {
            print("no ACK\r\n");
            return;
        }
        for (i = 0; i < v; i++) {
            print_hex(buf[i], 2);
            print(" ");
        }
        print("\r\n");
        return;
    }
    case 'W':
        p = num(p, &a, 16);
        p = num(p, &r, 16);
        num(p, &v, 16);
        print(i2c_write_reg(a, r, v) ? "no ACK\r\n" : "ok\r\n");
        return;
    case 't':
        num(p, &v, 10);
        tone(v);
        print("tone ");
        print_dec(v);
        print(" Hz\r\n");
        return;
    case 'w':
        melody();
        print("ok\r\n");
        return;
    case '?':
        help();
        return;
    }
    if (line[0])
        print("?\r\n");
}

int main(void)
{
    static char line[40];
    unsigned len = 0, was_connected = 0;

    usb = CFBOOT_SVC;
    if (usb->magic != CFBOOT_SVC_MAGIC)
        for (;;)
            ;
    tone(0);
    i2c_init();
    usb->usb_init();

    for (;;) {
        uint8_t c;
        usb->usb_poll();
        if (usb->connected() && !was_connected) {
            was_connected = 1;
            help();
            print("> ");
        } else if (!usb->connected()) {
            was_connected = 0;
        }
        if (usb->read(&c, 1) != 1)
            continue;
        if (c == '\r' || c == '\n') {
            print("\r\n");
            line[len] = 0;
            command(line);
            len = 0;
            print("> ");
        } else if (len < sizeof line - 1) {
            line[len++] = c;
            usb->write(&c, 1);           /* echo */
        }
    }
}
