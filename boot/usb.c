/* Minimal polled USB full-speed device driver + CDC-ACM for the MCF5225x
 * USB OTG module (RM chapter 20). EP0 control, EP1 IN interrupt (declared,
 * never used), EP2 bulk OUT/IN. One BD armed per direction at a time
 * (even/odd alternate), data toggles tracked in software.
 *
 * Also used by applications through the service table (svc.c), so all state
 * lives in the bootloader's RAM (0x2000E000+) and nothing here may depend on
 * bootloader-only code except reboot(). */
#include "mcf52254.h"
#include "boot.h"

#define DBG(s)  do { if (usb_in_bootloader) dbg_puts(s); } while (0)
#define DBGX(v) do { if (usb_in_bootloader) dbg_hex(v); } while (0)

#define VID          0x1209
#define PID_BOOT     0x0001
#define PID_APP      0x0002
#define EP0_SIZE     64
#define BULK_SIZE    64
#define RXRING       512
#define TXRING       1024

uint8_t usb_in_bootloader;
volatile uint8_t usb_reboot_request;   /* 1: requested, 2: request acked -> reboot */

static usb_bd_t bdt[64] __attribute__((aligned(512)));
/* USB DMA buffers must be word aligned: at odd addresses the module drops
 * bytes (a 2-byte OUT arrived with BC = 1). */
static uint8_t ep0_rxbuf[2][EP0_SIZE] __attribute__((aligned(4)));
static uint8_t ep0_txbuf[2][EP0_SIZE] __attribute__((aligned(4)));
static uint8_t ep2_rxbuf[2][BULK_SIZE] __attribute__((aligned(4)));
static uint8_t ep2_txbuf[2][BULK_SIZE] __attribute__((aligned(4)));

static uint8_t rx_odd[3], tx_odd[3], rx_data1[3], tx_data1[3];
static uint8_t configured, dtr, pending_addr, ep2_rx_armed, ep2_tx_busy, first_setup;
static uint8_t pending_config;          /* config+1, applied after the status stage */
static uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };   /* 115200 8N1 */

/* EP0 control transfer state */
static setup_t req;
static uint8_t ctrl_buf[128];
static const uint8_t *in_ptr;
static uint16_t in_left, out_len, out_want;
static uint8_t in_zlp, out_stage;

/* CDC byte rings */
static uint8_t rxring[RXRING], txring[TXRING];
static volatile uint16_t rx_head, rx_tail, tx_head, tx_tail;

/* ---- descriptors (USB little-endian byte order) ------------------------ */
static uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, EP0_SIZE,
    VID & 0xFF, VID >> 8, PID_BOOT & 0xFF, PID_BOOT >> 8,
    0x00, 0x01, 1, 2, 3, 1
};

static const uint8_t cfg_desc[67] = {
    9, 2, 67, 0, 2, 1, 0, 0x80, 50,
    /* interface 0: CDC communication, ACM, no AT protocol */
    9, 4, 0, 0, 1, 0x02, 0x02, 0x00, 0,
    5, 0x24, 0x00, 0x10, 0x01,              /* header, CDC 1.10 */
    5, 0x24, 0x01, 0x00, 0x01,              /* call management */
    4, 0x24, 0x02, 0x02,                    /* ACM: line coding + state */
    5, 0x24, 0x06, 0x00, 0x01,              /* union: master 0, slave 1 */
    7, 5, 0x81, 0x03, 8, 0, 255,            /* EP1 IN interrupt */
    /* interface 1: CDC data */
    9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    7, 5, 0x02, 0x02, BULK_SIZE, 0, 0,      /* EP2 OUT bulk */
    7, 5, 0x82, 0x02, BULK_SIZE, 0, 0,      /* EP2 IN bulk */
};

static const char *const strings[] = {
    0, "ColdFire", "CF52254 bootloader", "0001", "CF52254 app"
};

static uint16_t string_desc(uint8_t idx, uint8_t *buf)
{
    const char *s;
    uint16_t n = 2;
    if (idx == 0) {
        buf[0] = 4; buf[1] = 3; buf[2] = 0x09; buf[3] = 0x04;
        return 4;
    }
    if (idx > 3)
        return 0;
    s = (idx == 2 && !usb_in_bootloader) ? strings[4] : strings[idx];
    while (*s && n < sizeof ctrl_buf - 2) {
        buf[n++] = *s++;
        buf[n++] = 0;
    }
    buf[0] = n;
    buf[1] = 3;
    return n;
}

/* ---- buffer descriptors -------------------------------------------------- */
static usb_bd_t *bd(int ep, int tx, int odd)
{
    return &bdt[USB_BDT_INDEX(ep, tx, odd)];
}

/* EP0 IN watchdog state (see ep0_tx_watchdog) */
static uint8_t ep0_tx_armed;
static uint8_t *ep0_tx_buf;
static uint16_t ep0_tx_len, ep0_tx_since;

static uint16_t frame(void)
{
    return USB_FRM_NUML | (USB_FRM_NUMH & 7) << 8;
}

static void arm(int ep, int tx, uint8_t *buf, uint16_t len, int toggle_check)
{
    int odd = tx ? tx_odd[ep] : rx_odd[ep];
    int d1 = tx ? tx_data1[ep] : rx_data1[ep];
    usb_bd_t *b = bd(ep, tx, odd);
    b->addr = USB_BD_FROM_CPU((uint32_t)buf);
    b->ctl = USB_BD_FROM_CPU(USB_BD_BC(len) | (d1 ? USB_BD_DATA1 : 0) |
                             (toggle_check ? USB_BD_DTS : 0) | USB_BD_OWN);
    if (ep == 0 && tx) {
        ep0_tx_armed = 1;
        ep0_tx_buf = buf;
        ep0_tx_len = len;
        ep0_tx_since = frame();
    }
}

/* The SIE alternates EP0 IN between the even and odd BD. Rarely (about 1 in
 * 3 enumerations, timing dependent) our idea of the next BD differs from the
 * hardware's: the reply sits in the BD the SIE is not looking at and every IN
 * is NAKed until the host times out (seen as a lost SET_CONFIGURATION status).
 * If an EP0 IN has been pending for more than 4 frames, move it to the other
 * BD. Only one BD is ever armed, so nothing can be sent twice; the host keeps
 * retrying the IN, so the reply goes out on its next attempt. */
static void ep0_tx_watchdog(void)
{
    if (!ep0_tx_armed || ((frame() - ep0_tx_since) & 0x7FF) <= 4)
        return;
    bd(0, 1, tx_odd[0])->ctl = 0;
    tx_odd[0] ^= 1;
    DBG("F");
    arm(0, 1, ep0_tx_buf, ep0_tx_len, 1);
}

/* EP0 keeps BOTH receive BDs armed (no toggle check), so a SETUP that
 * arrives right after a status/data OUT always finds a buffer: a device must
 * never drop a SETUP. Each BD is re-armed as soon as its packet is consumed. */
static void ep0_arm_rx(int odd)
{
    usb_bd_t *b = bd(0, 0, odd);
    b->addr = USB_BD_FROM_CPU((uint32_t)ep0_rxbuf[odd]);
    b->ctl = USB_BD_FROM_CPU(USB_BD_BC(EP0_SIZE) | USB_BD_OWN);
}

static void ep0_send_chunk(void)
{
    uint16_t n = in_left > EP0_SIZE ? EP0_SIZE : in_left, i;
    uint8_t *buf = ep0_txbuf[tx_odd[0]];
    for (i = 0; i < n; i++)
        buf[i] = in_ptr[i];
    in_ptr += n;
    in_left -= n;
    if (n < EP0_SIZE)
        in_zlp = 0;                         /* short packet ends the transfer */
    arm(0, 1, buf, n, 1);
}

static void ep0_send(const uint8_t *data, uint16_t len)
{
    if (len > req.wLength)
        len = req.wLength;
    in_ptr = data;
    in_left = len;
    in_zlp = (len < req.wLength) && (len % EP0_SIZE == 0);
    ep0_send_chunk();
}

static void ep0_status_in(void)             /* zero-length status packet */
{
    in_left = 0;
    in_zlp = 0;
    tx_data1[0] = 1;
    arm(0, 1, ep0_txbuf[tx_odd[0]], 0, 1);
}

static void ep0_stall(void)
{
    DBG(" X");
    DBGX(req.bRequest);
    USB_ENDPT0 |= USB_ENDPT_EP_STALL;
}

/* ---- CDC endpoints ------------------------------------------------------- */
static void ep2_rx_arm(void)
{
    uint16_t used = (uint16_t)(rx_head - rx_tail);
    if (!ep2_rx_armed && RXRING - used >= BULK_SIZE) {
        arm(2, 0, ep2_rxbuf[rx_odd[2]], BULK_SIZE, 1);
        ep2_rx_armed = 1;
    }
}

static void ep2_tx_kick(void)
{
    uint16_t n = 0;
    uint8_t *buf;
    if (ep2_tx_busy || !configured || tx_head == tx_tail)
        return;
    buf = ep2_txbuf[tx_odd[2]];
    while (n < BULK_SIZE && tx_tail != tx_head)
        buf[n++] = txring[tx_tail++ % TXRING];
    arm(2, 1, buf, n, 1);
    ep2_tx_busy = 1;
}

static void configure(int cfg)
{
    configured = cfg;
    rx_odd[1] = tx_odd[1] = rx_odd[2] = tx_odd[2] = 0;
    rx_data1[2] = tx_data1[2] = tx_data1[1] = 0;
    ep2_rx_armed = ep2_tx_busy = 0;
    if (!cfg)
        return;
    USB_ENDPT(1) = USB_ENDPT_EP_TX_EN | USB_ENDPT_EP_HSHK | USB_ENDPT_EP_CTL_DIS;
    USB_ENDPT(2) = USB_ENDPT_EP_TX_EN | USB_ENDPT_EP_RX_EN | USB_ENDPT_EP_HSHK |
                   USB_ENDPT_EP_CTL_DIS;
    ep2_rx_arm();
}

/* ---- control requests ---------------------------------------------------- */
static int std_request(void)
{
    uint8_t type = req.wValue >> 8;
    switch (req.bRequest) {
    case 0:                                 /* GET_STATUS */
        ctrl_buf[0] = ctrl_buf[1] = 0;
        ep0_send(ctrl_buf, 2);
        return 0;
    case 1: case 3:                         /* CLEAR/SET_FEATURE */
        if ((req.bmRequestType & 0x1F) == 2 && (req.wIndex & 0x0F) == 2) {
            if (req.wIndex & 0x80) tx_data1[2] = 0; else rx_data1[2] = 0;
        }
        ep0_status_in();
        return 0;
    case 5:                                 /* SET_ADDRESS (applied after status) */
        pending_addr = req.wValue & 0x7F;
        DBG(" A");
        DBGX(pending_addr);
        ep0_status_in();
        return 0;
    case 6:                                 /* GET_DESCRIPTOR */
        if (type == 1) {
            uint16_t pid = usb_in_bootloader ? PID_BOOT : PID_APP;
            dev_desc[10] = pid & 0xFF;
            dev_desc[11] = pid >> 8;
            ep0_send(dev_desc, sizeof dev_desc);
            return 0;
        }
        if (type == 2) {
            ep0_send(cfg_desc, sizeof cfg_desc);
            return 0;
        }
        if (type == 3) {
            uint16_t n = string_desc(req.wValue & 0xFF, ctrl_buf);
            if (!n)
                return -1;
            ep0_send(ctrl_buf, n);
            return 0;
        }
        return -1;                          /* qualifier etc.: full-speed only */
    case 8:                                 /* GET_CONFIGURATION */
        ctrl_buf[0] = configured;
        ep0_send(ctrl_buf, 1);
        return 0;
    case 9:                                 /* SET_CONFIGURATION */
        /* The configuration (endpoint enables) is applied once the status
         * stage has completed, i.e. when the request has really finished. */
        pending_config = (req.wValue & 0xFF) + 1;
        DBG(" C");
        DBGX(req.wValue & 0xFF);
        ep0_status_in();
        return 0;
    case 10:                                /* GET_INTERFACE */
        ctrl_buf[0] = 0;
        ep0_send(ctrl_buf, 1);
        return 0;
    case 11:                                /* SET_INTERFACE */
        ep0_status_in();
        return 0;
    }
    return -1;
}

/* called at SETUP for IN/no-data requests, after the data stage for OUT */
static int class_request(void)
{
    uint8_t i;
    switch (req.bRequest) {
    case 0x20:                              /* SET_LINE_CODING */
        for (i = 0; i < 7; i++)
            line_coding[i] = ctrl_buf[i];
        ep0_status_in();
        return 0;
    case 0x21:                              /* GET_LINE_CODING */
        ep0_send(line_coding, 7);
        return 0;
    case 0x22: {                            /* SET_CONTROL_LINE_STATE */
        uint32_t baud = line_coding[0] | line_coding[1] << 8 |
                        (uint32_t)line_coding[2] << 16 | (uint32_t)line_coding[3] << 24;
        dtr = req.wValue & 1;
        if (!dtr && baud == 1200)
            usb_reboot_request = 1;         /* "1200-baud touch" */
        ep0_status_in();
        return 0;
    }
    case 0x23:                              /* SEND_BREAK */
        ep0_status_in();
        return 0;
    }
    return -1;
}

static void handle_setup(const uint8_t *p)
{
    int r;
    req.bmRequestType = p[0];
    req.bRequest = p[1];
    req.wValue = p[2] | p[3] << 8;
    req.wIndex = p[4] | p[5] << 8;
    req.wLength = p[6] | p[7] << 8;

    /* a SETUP cancels anything pending on EP0 IN */
    bd(0, 1, 0)->ctl = 0;
    bd(0, 1, 1)->ctl = 0;
    ep0_tx_armed = 0;

    USB_ENDPT0 &= ~USB_ENDPT_EP_STALL;
    tx_data1[0] = 1;
    out_stage = 0;

    if (!(req.bmRequestType & 0x80) && req.wLength) {      /* OUT data stage */
        if (req.wLength > sizeof ctrl_buf) {
            ep0_stall();
            return;
        }
        out_stage = 1;
        out_len = 0;
        out_want = req.wLength;
        return;
    }
    if ((req.bmRequestType & 0x60) == 0x00)
        r = std_request();
    else if ((req.bmRequestType & 0x60) == 0x20)
        r = class_request();
    else
        r = -1;
    if (r)
        ep0_stall();
}

static void ep0_token(uint8_t pid, int tx, int odd, uint8_t *buf, uint16_t bc)
{
    if (pid == USB_PID_SETUP) {
        handle_setup(buf);
        ep0_arm_rx(odd);
        USB_CTL &= ~USB_CTL_TXSUSPEND_TOKENBUSY;
        return;
    }
    if (!tx) {                                             /* OUT */
        if (out_stage) {
            uint16_t i;
            for (i = 0; i < bc && out_len < out_want; i++)
                ctrl_buf[out_len++] = buf[i];
            if (out_len >= out_want || bc < EP0_SIZE) {
                out_stage = 0;
                if ((req.bmRequestType & 0x60) != 0x20 || class_request())
                    ep0_stall();
            }
        }
        ep0_arm_rx(odd);
        return;
    }
    /* IN done */
    ep0_tx_armed = 0;
    if (pending_config) {
        configure(pending_config - 1);
        pending_config = 0;
    }
    if (usb_reboot_request == 1)            /* status stage of the 1200-baud */
        usb_reboot_request = 2;             /* touch is done: safe to detach */
    if (pending_addr) {
        USB_ADDR = pending_addr;
        pending_addr = 0;
    }
    if (in_left || in_zlp)
        ep0_send_chunk();
}

/* ---- public API ------------------------------------------------------------ */
static void bus_reset(void)
{
    unsigned i;
    DBG(" R");
    USB_CTL |= USB_CTL_ODD_RST;
    USB_CTL &= ~USB_CTL_ODD_RST;
    for (i = 0; i < 64; i++)
        bdt[i].ctl = 0;
    ep0_tx_armed = 0;
    for (i = 0; i < 3; i++)
        rx_odd[i] = tx_odd[i] = rx_data1[i] = tx_data1[i] = 0;
    for (i = 1; i < 16; i++)
        USB_ENDPT(i) = 0;
    USB_ADDR = 0;
    pending_addr = 0;
    pending_config = 0;
    configured = dtr = 0;
    ep2_rx_armed = ep2_tx_busy = 0;
    USB_ENDPT0 = USB_ENDPT0_CONTROL;
    ep0_arm_rx(0);
    ep0_arm_rx(1);
    USB_ERR_STAT = 0xFF;
    USB_INT_STAT = 0xFF;
}

void usb_init(void)
{
    uint32_t a = (uint32_t)bdt;
    SCM_RAMBAR = 0x20000000u | SCM_RAMBAR_BDE;            /* USB DMA access to SRAM */
    USB_CTL = 0;
    USB_CTRL = USB_CTRL_CLK_SRC(USB_CTRL_CLK_SRC_SYSCLK); /* clears SUSP, PDE */
    USB_OTG_CTRL = 0;
    USB_BDT_PAGE_01 = (a >> 8) & 0xFE;
    USB_BDT_PAGE_02 = a >> 16;
    USB_BDT_PAGE_03 = a >> 24;
    rx_head = rx_tail = tx_head = tx_tail = 0;
    usb_reboot_request = 0;
    first_setup = 0;
    USB_INT_ENB = 0;
    bus_reset();
    USB_CTL = USB_CTL_USB_EN_SOF_EN;
    USB_OTG_CONTROL = USB_OTGCON_DPPULLUP_NONOTG;          /* attach */
}

void usb_stop(void)
{
    USB_OTG_CONTROL = 0;
    USB_CTL = 0;
    configured = dtr = 0;
}

void usb_poll(void)
{
    uint8_t is = USB_INT_STAT;

    if (is & USB_INT_USB_RST) {
        bus_reset();
        return;
    }
    if (is & USB_INT_ERROR) {
        DBG(" E");
        DBGX(USB_ERR_STAT);
        USB_ERR_STAT = 0xFF;
        USB_INT_STAT = USB_INT_ERROR;
    }
    if (is & (USB_INT_SLEEP | USB_INT_RESUME | USB_INT_SOF_TOK | USB_INT_STALL))
        USB_INT_STAT = is & (USB_INT_SLEEP | USB_INT_RESUME | USB_INT_SOF_TOK | USB_INT_STALL);

    while (USB_INT_STAT & USB_INT_TOK_DNE) {
        uint8_t st = USB_STAT;
        int ep = USB_STAT_ENDP(st), tx = !!(st & USB_STAT_TX), odd = !!(st & USB_STAT_ODD);
        usb_bd_t *b = bd(ep, tx, odd);
        uint32_t ctl = USB_BD_TO_CPU(b->ctl);
        uint8_t *buf = (uint8_t *)USB_BD_TO_CPU(b->addr);
        uint16_t bc = USB_BD_GET_BC(ctl);
        uint8_t pid = USB_BD_TOK_PID(ctl);
        USB_INT_STAT = USB_INT_TOK_DNE;                    /* after reading STAT */

        if (ep > 2)
            continue;
        if (tx) {
            tx_odd[ep] ^= 1;
            tx_data1[ep] ^= 1;
        } else {
            rx_odd[ep] ^= 1;
            if (pid != USB_PID_SETUP)
                rx_data1[ep] ^= 1;
        }
        if (ep == 0) {
            ep0_token(pid, tx, odd, buf, bc);
        } else if (ep == 2 && !tx) {
            uint16_t i;
            for (i = 0; i < bc; i++)
                rxring[rx_head++ % RXRING] = buf[i];
            ep2_rx_armed = 0;
            ep2_rx_arm();
        } else if (ep == 2 && tx) {
            ep2_tx_busy = 0;
            ep2_tx_kick();
        }
    }
    ep2_tx_kick();
    ep0_tx_watchdog();
    if (usb_reboot_request == 2)
        reboot(1);
    if (usb_in_bootloader)
        dbg_flush();
}

int usb_configured(void) { return configured; }
int cdc_connected(void) { return configured && dtr; }

int cdc_read(uint8_t *buf, unsigned max)
{
    unsigned n = 0;
    while (n < max && rx_tail != rx_head)
        buf[n++] = rxring[rx_tail++ % RXRING];
    if (n)
        ep2_rx_arm();
    return n;
}

/* No timers here: applications call this too, and the bootloader's PIT is
 * stopped once the app runs. The spin limit is roughly 0.5 s at 48 MHz. */
void cdc_write(const uint8_t *buf, unsigned len)
{
    uint32_t spins = 0;
    while (len) {
        if (!configured)
            return;                         /* no host: drop */
        if ((uint16_t)(tx_head - tx_tail) < TXRING) {
            txring[tx_head++ % TXRING] = *buf++;
            len--;
            spins = 0;
        } else {
            usb_poll();
            if (++spins > 100000)
                return;                     /* host stopped reading: drop */
        }
    }
    ep2_tx_kick();
}

