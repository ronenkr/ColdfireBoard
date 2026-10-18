/* ColdFire MCF52254 USB bootloader: CDC-ACM serial command protocol.
 *
 * Stays in the bootloader if: the app area is empty/invalid, the previous
 * reset was a "reboot to bootloader" (magic word in .noinit RAM), or the host
 * sends cfboot's "I" within the boot window (1 s after USB enumeration, or 8 s
 * if no host ever enumerates the board). Otherwise starts the app.
 *
 * Protocol (one command per line, replies "OK ..." or "ERR ...", log lines "#"):
 *   I              info
 *   E addr len     erase app flash covering [addr, addr+len)
 *   W addr len     followed by len raw bytes (len <= 1024, multiple of 4)
 *   R addr len     reply "OK", then len raw bytes (any address if len <= 64:
 *                  register peek for debugging)
 *   C addr len     CRC-32 (zlib) of flash region
 *   G              start the application
 *   B              reboot and stay in the bootloader
 * Numbers are hex. Writes/erases are only allowed in [APP_BASE, FLASH_END).
 */
#include "mcf52254.h"
#include "boot.h"

#define WINDOW_AFTER_CONFIG_MS 1000     /* host enumerated us: wait for cfboot's "I" */
#define WINDOW_MAX_MS          8000     /* never enumerated (no host): start the app */
#define RCR             REG8(0x110000)       /* RM 11.5.1 */
#define RCR_SOFTRST     0x80u

extern uint8_t usb_in_bootloader;
__attribute__((section(".noinit"))) volatile uint32_t boot_magic;

static uint8_t xfer[MAX_XFER];

/* ---- clock: 12 MHz crystal, PLL -> fsys 48 MHz (RM 7.8.3) ---------------- */
static void clock_init(void)
{
    SYNCR &= ~SYNCR_CLKSRC;                  /* run from the reference while relocking */
    CCHR = CCHR_48MHZ;                       /* /2 -> fref 6 MHz */
    SYNCR = SYNCR_MFD(2) | SYNCR_RFD(1) | SYNCR_PLLMODE | SYNCR_PLLEN;
    while (!(SYNSR & SYNSR_LOCK))
        bwt_service();
    SYNCR = SYNCR_MFD(2) | SYNCR_RFD(1) | SYNCR_CLKSRC | SYNCR_PLLMODE | SYNCR_PLLEN;
    SYNCR = SYNCR_48MHZ_FINAL;               /* RFD /1 -> 48 MHz */
}

/* ---- PIT0 free-running at fsys/2/2048 = 11718.75 Hz ---------------------- */
#define PCSR0  REG16(0x150000)
#define PCNTR0 REG16(0x150004)
static uint32_t ticks;
static uint16_t last_cnt;

uint32_t millis(void)
{
    /* PCNTR0 counts in the PIT clock domain; a read can catch it mid-update
     * and return garbage, which looked like a ~5 s jump and started the app
     * the moment USB was configured. Accept a value only when two consecutive
     * reads agree (the counter only changes every ~85 us). */
    uint16_t c = PCNTR0, c2;
    while ((c2 = PCNTR0) != c)
        c = c2;
    ticks += (uint16_t)(last_cnt - c);       /* down-counter */
    last_cnt = c;
    return ticks * 32u / 375u;
}

static void timer_init(void)
{
    PCSR0 = 0;
    PCSR0 = (11u << 8) | 1u;                 /* PRE = 2^11, free-running, EN */
    last_cnt = PCNTR0;
}

/* ---- reset / app ------------------------------------------------------- */
static void delay_spin(uint32_t n)           /* timer-free: apps call reboot() */
{
    while (n--) {
        __asm__ volatile ("nop");
        if (!(n & 0xFFFF))
            bwt_service();                   /* every few ms: a leftover watchdog */
    }
}

/* USB detach time before reset / handing over to the app. It must be long
 * enough for the host to see a real disconnect: after a ~20 ms blip Linux can
 * keep the old device (old address) while the app re-enumerates at address 0,
 * leaving a dead "bootloader" entry that answers nothing (seen on root ports). */
#define DETACH_SPINS 4000000u                /* ~250 ms at 48 MHz */

void reboot(int stay)
{
    usb_stop();                              /* let the host see the disconnect */
    delay_spin(DETACH_SPINS);
    boot_magic = stay ? BOOT_MAGIC : 0;
    RCR = RCR_SOFTRST;
    for (;;)
        ;
}

/* called from _default_handler with the exception frame (RM 3.8) */
void fault(uint32_t fmt_vec_sr, uint32_t pc)
{
    dbg_drain();
    uart_puts("\n# FAULT vector ");
    uart_puthex((fmt_vec_sr >> 18) & 0xFF, 2);
    uart_puts(" pc ");
    uart_puthex(pc, 8);
    uart_puts(" sr ");
    uart_puthex(fmt_vec_sr & 0xFFFF, 4);
    uart_puts("\n");
    reboot(0);
}

static int app_valid(void)
{
    uint32_t sp = ((const uint32_t *)APP_BASE)[0];
    uint32_t pc = ((const uint32_t *)APP_BASE)[1];
    return sp > 0x20000000u && sp <= 0x20010000u && pc >= APP_BASE + 8 && pc < FLASH_END;
}

static void start_app(void)
{
    dbg_puts(" J\n");
    dbg_drain();
    usb_stop();
    delay_spin(DETACH_SPINS);
    usb_in_bootloader = 0;                   /* app's usb_init() enumerates as the app */
    PCSR0 = 0;
    bwt_service();                           /* the app starts with a full timeout */
    jump_to_app(APP_BASE);
}

/* ---- protocol helpers -------------------------------------------------- */
static void out(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    cdc_write((const uint8_t *)s, n);
}

static void out_hex(uint32_t v)
{
    char b[9];
    int i;
    for (i = 7; i >= 0; i--, v >>= 4)
        b[i] = "0123456789abcdef"[v & 15];
    b[8] = 0;
    out(b);
}

static int rd_byte(uint32_t timeout_ms)
{
    uint8_t c;
    uint32_t t = millis();
    while (!cdc_read(&c, 1)) {
        usb_poll();
        if (millis() - t > timeout_ms)
            return -1;
    }
    return c;
}

static const char *parse_hex(const char *p, uint32_t *v)
{
    *v = 0;
    while (*p == ' ')
        p++;
    for (;; p++) {
        char c = *p;
        if (c >= '0' && c <= '9')       *v = *v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f')  *v = *v * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')  *v = *v * 16 + (c - 'A' + 10);
        else return p;
    }
}

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    int k;
    while (n--) {
        if (!(n & 0xFFF))
            bwt_service();                   /* ~8 ms between feeds */
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

static int range_ok(uint32_t a, uint32_t n, int write)
{
    if (a + n < a || a + n > FLASH_END)
        return 0;
    return !write || a >= APP_BASE;
}

static void command(char *line)
{
    uint32_t a = 0, n = 0, i;
    const char *p = line + 1;

    p = parse_hex(p, &a);
    parse_hex(p, &n);
    switch (line[0]) {
    case 'I':
        out("OK CFBOOT 1 app=");
        out(app_valid() ? "1" : "0");
        out(" base=4000 end=80000\n");
        return;
    case 'E':
        if (!range_ok(a, n, 1))
            break;
        for (i = a & ~2047u; i < a + n; i += 2048) {
            bwt_service();                   /* a full erase takes ~10 s */
            if (flash_erase_page(i)) {
                out("ERR erase ");
                out_hex(i);
                out("\n");
                return;
            }
        }
        out("OK\n");
        return;
    case 'W':
        if (!range_ok(a, n, 1) || n > MAX_XFER || (a | n) & 3)
            break;
        for (i = 0; i < n; i++) {
            int c = rd_byte(1000);
            if (c < 0) {
                out("ERR timeout\n");
                return;
            }
            xfer[i] = c;
        }
        bwt_service();
        if (flash_program(a, (const uint32_t *)xfer, n / 4)) {
            out("ERR program\n");
            return;
        }
        for (i = 0; i < n; i++)
            if (((const uint8_t *)a)[i] != xfer[i]) {
                out("ERR verify ");
                out_hex(a + i);
                out("\n");
                return;
            }
        out("OK\n");
        return;
    case 'R':
        if ((!range_ok(a, n, 0) && n > 64) || n > MAX_XFER)
            break;
        out("OK\n");
        cdc_write((const uint8_t *)a, n);
        return;
    case 'C':
        if (!range_ok(a, n, 0))
            break;
        out("OK ");
        out_hex(crc32((const uint8_t *)a, n));
        out("\n");
        return;
    case 'G':
        out("OK\n");
        for (i = millis(); millis() - i < 50;)
            usb_poll();
        start_app();
        return;
    case 'B':
        out("OK\n");
        for (i = millis(); millis() - i < 50;)
            usb_poll();
        reboot(1);
        return;
    }
    out("ERR bad command\n");
}

int main(void)
{
    static char line[48];
    unsigned len = 0;
    int stay = boot_magic == BOOT_MAGIC;
#ifndef NO_DEBUG
    uint8_t rsr = REG8(0x110001);            /* reset cause, latched at reset */
#endif
    uint8_t c;

    boot_magic = 0;
    bwt_service();                           /* before the slow clock/PLL setup */
    clock_init();
    timer_init();
    uart_init();
    flash_init();
#ifndef NO_DEBUG
    uart_puts("\n# cfboot: fsys 48 MHz, app ");
    uart_puts(app_valid() ? "valid" : "missing");
    uart_puts(stay ? ", stay requested" : "");
    /* reset cause (RM 11.5.2): 80 BWT, 40 LVD, 20 SOFT, 10 WDR, 08 POR, 04 EXT, 02 LOC, 01 LOL */
    uart_puts(", rsr ");
    uart_puthex(rsr, 2);
    uart_puts(" wcr ");                      /* backup watchdog: control, modulus, count; BWCR */
    uart_puthex(REG16(0x140000), 4);
    uart_puts(" wmr ");
    uart_puthex(REG16(0x140002), 4);
    uart_puts(" wcntr ");
    uart_puthex(REG16(0x140004), 4);
    uart_puts(" bwcr ");
    uart_puthex(REG8(0x120013), 2);
    uart_puts("\n");
#endif
    if (!app_valid())
        stay = 1;

    usb_in_bootloader = 1;
    usb_init();
    for (;;) {
        usb_poll();
        dbg_flush();
        if (!stay) {
            static uint32_t cfg_at;
            static uint8_t cfg_seen;
            if (usb_configured() && !cfg_seen) {
                cfg_seen = 1;
                cfg_at = millis();
                dbg_puts(" W");
                dbg_hex(cfg_at);
            }
            if (cfg_seen ? (int32_t)(millis() - cfg_at) > WINDOW_AFTER_CONFIG_MS
                         : millis() > WINDOW_MAX_MS) {
                dbg_puts(" m");
                dbg_hex(millis());
                start_app();
            }
        }
        if (!cdc_read(&c, 1))
            continue;
        if (c == '\r')
            continue;
        if (c != '\n') {
            if (len < sizeof line - 1)
                line[len++] = c;
            continue;
        }
        line[len] = 0;
        /* Only cfboot's "I" claims the bootloader during the boot window, so a
         * port prober (ModemManager sends "AT") cannot trap the board here. */
        if (!stay && len == 1 && line[0] == 'I') {
            stay = 1;
            dbg_puts(" I");
            out("# host connected, staying in bootloader\n");
        }
        if (len && stay)
            command(line);
        len = 0;
    }
}
