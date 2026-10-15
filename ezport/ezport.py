# MicroPython (RP2040) EzPort programmer for the MCF5225x / MCF52254.
# Reference: MCF52259 Reference Manual Rev. 4, chapter 23 (EzPort) and 18 (CFM).
#
# Copy to the Pico and drive it with ../ezport/ezflash.py on the PC, or from the REPL:
#   import ezport; e = ezport.EzPort(); e.enter(); e.info()

from machine import Pin, SPI, mem32
import time
import machine

# Pico GPIO wiring (see README.md)
PIN_SCK = 2      # -> EZPCK  (MCU pin 20, QSPI_CLK,  CON5-5)
PIN_MOSI = 3     # -> EZPD   (MCU pin 18, QSPI_DIN,  CON5-6)
PIN_MISO = 4     # <- EZPQ   (MCU pin 19, QSPI_DOUT, CON5-7)
PIN_CS = 5       # -> EZPCS  (MCU pin 4,  RCON,      CON5-2)
PIN_RST = 6      # -> RSTI   (MCU pin 97, CON5-1), open-drain; None if not wired
PIN_CLKOUT = 7   # <- CLKOUT (MCU pin 31), optional, only for measure_fsys()

# EzPort commands (RM table 23-2)
WREN, WRDI, RDSR, WRCR = 0x06, 0x04, 0x05, 0x01
READ, FAST_READ, PP, SE, BE, RESET = 0x03, 0x0B, 0x02, 0xD8, 0xC7, 0xB9

# Status register bits (RM figure 23-2)
FS, WEF, CRL, WEN, WIP = 0x80, 0x40, 0x20, 0x02, 0x01

FLASH_SIZE = 512 * 1024   # MCF52254
PAGE = 256                # PP max length, must not cross a 256-byte boundary
CFG_FIELD = 0x400         # CFM configuration field 0x400..0x417 (RM table 18-1)
SEC_MAGIC = 0x4AC8        # CFMSEC[15:0] value that secures the flash


class EzPortError(Exception):
    pass


def wrcr_value(fsys_hz):
    """Clock config for a 150-200 kHz flash clock (RM section 23.6)."""
    prdiv8 = 1 if fsys_hz > 25_600_000 else 0
    pre = 8 if prdiv8 else 1
    div = fsys_hz // (2 * 200_000 * pre)
    if not 1 <= div <= 63:
        raise EzPortError("fsys %d Hz out of range for WRCR" % fsys_hz)
    fclk = fsys_hz / (2 * (div + 1) * pre)
    if not 150_000 <= fclk <= 200_000:
        raise EzPortError("fsys %d Hz gives FCLK %.0f Hz (needs 150-200 kHz)" % (fsys_hz, fclk))
    return (prdiv8 << 6) | div, fclk


def measure_fsys(gpio=PIN_CLKOUT, window_ms=50, div=100):
    """Count rising edges of the MCU CLKOUT pin with an RP2040 PWM slice.

    PWM edge counting is limited to clk_sys/2, so clk_sys is raised to 200 MHz
    for the measurement and restored afterwards. Returns Hz (0 = no clock seen).
    """
    if not gpio & 1:
        raise EzPortError("CLKOUT must be on an odd GPIO (PWM channel B)")
    old = machine.freq()
    machine.freq(200_000_000)
    try:
        base = 0x40050000 + 0x14 * ((gpio >> 1) & 7)
        Pin(gpio, Pin.IN)
        mem32[0x40014004 + 8 * gpio] = 4            # IO_BANK0 GPIOn_CTRL.FUNCSEL = PWM
        mem32[base + 0x00] = 0                      # CSR: disable
        mem32[base + 0x04] = div << 4               # DIV.INT
        mem32[base + 0x10] = 0xFFFF                 # TOP
        mem32[base + 0x08] = 0                      # CTR
        t0 = time.ticks_us()
        mem32[base + 0x00] = (2 << 4) | 1           # DIVMODE = rising edges of B, EN
        time.sleep_ms(window_ms)
        mem32[base + 0x00] = 2 << 4
        t1 = time.ticks_us()
        count = mem32[base + 0x08]
        Pin(gpio, Pin.IN, pull=None)                           # back to plain input
    finally:
        machine.freq(old)
    return count * div * 1_000_000 // time.ticks_diff(t1, t0)


class EzPort:
    def __init__(self, baud=500_000):
        self.baud = baud
        self.cs = Pin(PIN_CS, Pin.OUT, value=1)
        self.rst = Pin(PIN_RST, Pin.IN, pull=None) if PIN_RST is not None else None
        self.spi = None     # created lazily: enter() first holds the MCU in reset so
                            # Pico SPI outputs never fight a running firmware's QSPI

    def _spi_on(self):
        if self.spi is None:
            self.spi = SPI(0, baudrate=self.baud, polarity=0, phase=0, bits=8,
                           firstbit=SPI.MSB, sck=Pin(PIN_SCK), mosi=Pin(PIN_MOSI),
                           miso=Pin(PIN_MISO))
            # EZPQ is tri-stated when idle: pull-up (PUE=1, PDE=0 in PADS_BANK0)
            # so a missing target reads 0xFF instead of a plausible 0x00 status.
            pad = 0x4001C004 + 4 * PIN_MISO
            mem32[pad] = (mem32[pad] | 0x08) & ~0x04

    # --- low level -------------------------------------------------------

    def _xfer(self, out, nread=0):
        self._spi_on()
        self.cs(0)
        try:
            self.spi.write(out)
            return self.spi.read(nread) if nread else None
        finally:
            self.cs(1)

    def _reset_line(self, asserted):
        # Open-drain: pull low, or release and let the board pull-up win.
        if asserted:
            self.rst.init(Pin.OUT, value=0)
        else:
            self.rst.init(Pin.IN, pull=None)

    def status(self):
        return self._xfer(bytes([RDSR]), 1)[0]

    def wait(self, timeout_ms=5000):
        """Poll until WIP clears. Returns final status with any WEF seen OR-ed in
        (WEF self-clears on every RDSR)."""
        seen = 0
        t0 = time.ticks_ms()
        while True:
            s = self.status()
            seen |= s & WEF
            if not s & WIP:
                return s | seen
            if time.ticks_diff(time.ticks_ms(), t0) > timeout_ms:
                raise EzPortError("timeout, status 0x%02x" % s)

    def write_enable(self):
        self._xfer(bytes([WREN]))
        s = self.status()
        if not s & WEN:
            raise EzPortError("WREN not accepted, status 0x%02x" % s)

    # --- mode entry ------------------------------------------------------

    def handshake(self):
        """Prove a live EzPort: WREN must set WEN, WRDI must clear it. A stuck
        EZPQ line (unpowered target, bad wire) cannot fake both. No flash access."""
        self._xfer(bytes([WREN]))
        s1 = self.status()
        self._xfer(bytes([WRDI]))
        s2 = self.status()
        if s1 & 0x1C or s2 & 0x1C or not s1 & WEN or s2 & WEN:
            raise EzPortError("no EzPort response (WREN->0x%02x, WRDI->0x%02x)" % (s1, s2))
        return s2

    def enter(self, power_cycle_s=10):
        """Reset the MCU with EZPCS held low so it comes up in EzPort mode."""
        if self.rst is not None and not self.rst.value():
            raise EzPortError("RSTI reads low with the line released: "
                              "target not powered, or RSTI/GND not connected")
        self.cs(0)
        if self.rst is not None:
            self._reset_line(True)
            self._spi_on()
            time.sleep_ms(20)
            self._reset_line(False)
            time.sleep_ms(100)     # hold EZPCS until RSTO has negated
        else:
            print("EZPCS held low: power-cycle the board within %d s" % power_cycle_s)
            time.sleep(power_cycle_s)
        self.cs(1)
        time.sleep_ms(1)
        return self.handshake()

    def reset_chip(self, stay_in_ezport=False):
        """RESET command; with stay_in_ezport the MCU re-enters EzPort mode."""
        self._xfer(bytes([RESET]))
        if stay_in_ezport:
            self.cs(0)
            time.sleep_ms(100)
            self.cs(1)
            time.sleep_ms(1)
            return self.status()

    def release(self):
        """Return every Pico pin to a plain input so nothing fights the MCU
        once its own firmware runs (QSPI pins may become outputs)."""
        if self.spi is not None:
            self.spi.deinit()
            self.spi = None
        for g in (PIN_SCK, PIN_MOSI, PIN_MISO, PIN_CS):
            Pin(g, Pin.IN, pull=None)
        if self.rst is not None:
            self._reset_line(False)

    def info(self):
        s = self.status()
        print("status 0x%02x: %s, %s, %s" % (
            s,
            "SECURED" if s & FS else "unsecured",
            "clock configured" if s & CRL else "clock not configured",
            "write error" if s & WEF else "no write error"))
        return s

    # --- flash operations ------------------------------------------------

    def configure(self, fsys_hz):
        """Write the flash clock divider. Write-once per reset."""
        if self.status() & CRL:
            return
        val, fclk = wrcr_value(fsys_hz)
        print("WRCR 0x%02x -> FCLK %.1f kHz" % (val, fclk / 1000))
        self.write_enable()
        self._xfer(bytes([WRCR, val]))
        s = self.wait()
        if s & WEF or not s & CRL:
            raise EzPortError("WRCR failed, status 0x%02x" % s)

    def read(self, addr, n):
        """FAST_READ (valid up to fsys/2, unlike READ's fsys/8)."""
        return self._xfer(bytes([FAST_READ, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF,
                                 addr & 0xFF, 0]), n)

    def bulk_erase(self):
        self.write_enable()
        self._xfer(bytes([BE]))
        s = self.wait(timeout_ms=10000)
        if s & WEF:
            raise EzPortError("bulk erase failed, status 0x%02x" % s)

    def program_page(self, addr, data):
        if addr & 3 or len(data) & 3 or not data or (addr & 0xFF) + len(data) > PAGE:
            raise EzPortError("bad PP 0x%06x len %d" % (addr, len(data)))
        self.write_enable()
        self._xfer(bytes([PP, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF]) + data)
        s = self.wait()
        if s & WEF:
            raise EzPortError("program failed at 0x%06x (protected sector?)" % addr)


# --- high level helpers called by ezflash.py --------------------------------

def check_image(img):
    if len(img) > FLASH_SIZE:
        raise EzPortError("image is %d bytes, flash is %d" % (len(img), FLASH_SIZE))
    if len(img) < CFG_FIELD + 0x18:
        raise EzPortError("image has no CFM config field at 0x400-0x417; "
                          "after reset every sector would be write-protected")
    prot = int.from_bytes(img[0x408:0x40C], "big")
    sec = int.from_bytes(img[0x414:0x418], "big") & 0xFFFF
    if sec == SEC_MAGIC:
        raise EzPortError("image would SECURE the flash (CFMSEC=0x4AC8) - refusing")
    if prot:
        raise EzPortError("image sets CFMPROT=0x%08x; future EzPort flashing "
                          "would fail - use 0" % prot)


def do_info():
    e = EzPort()
    e.enter()
    e.info()
    e.release()      # leave the MCU halted in EzPort mode


def do_dump(path, size=FLASH_SIZE, chunk=4096):
    e = EzPort()
    if e.enter() & FS:
        raise EzPortError("flash is secured - contents cannot be read (only bulk-erased)")
    with open(path, "wb") as f:
        for a in range(0, size, chunk):
            f.write(e.read(a, chunk))
    e.release()
    print("dumped %d bytes to %s" % (size, path))


def do_flash(path, fsys_hz, run=True):
    with open(path, "rb") as f:
        img = f.read()
    check_image(img)
    img += b"\xff" * (-len(img) % 4)

    e = EzPort()
    s = e.enter()
    e.info()
    e.configure(fsys_hz)
    # Erase and program in one session, no reset in between: an erased config
    # field would load CFMPROT = all-protected on the next reset (RM table 18-1,
    # 18-8). BE also clears FS immediately (RM 18.4.3.4 EzPort lockout recovery).
    print("bulk erase%s" % (" (clears flash security)" if s & FS else ""))
    e.bulk_erase()
    s = e.status()
    if s & FS:
        raise EzPortError("still secured after bulk erase, status 0x%02x" % s)

    t0 = time.ticks_ms()
    for a in range(0, len(img), PAGE):
        page = img[a:a + PAGE]
        if page != b"\xff" * len(page):
            e.program_page(a, page)
    print("programmed %d bytes in %d ms" % (len(img), time.ticks_diff(time.ticks_ms(), t0)))

    for a in range(0, len(img), 4096):
        want = img[a:a + 4096]
        got = e.read(a, len(want))
        if got != want:
            for i in range(len(want)):
                if got[i] != want[i]:
                    raise EzPortError("verify failed at 0x%06x: %02x != %02x"
                                      % (a + i, got[i], want[i]))
    print("verify OK")
    if run:
        e.reset_chip()
        print("MCU reset, running new firmware")
    e.release()


def do_wait_power(timeout_s=300):
    """Hold EZPCS low (no back-powering: only a low level is driven) so the MCU
    boots straight into EzPort mode at power-on; wait for RSTI to go high."""
    cs = Pin(PIN_CS, Pin.OUT, value=0)
    for g in (PIN_SCK, PIN_MOSI, PIN_MISO):
        Pin(g, Pin.IN, pull=None)
    rst = Pin(PIN_RST, Pin.IN, pull=None)
    t0 = time.ticks_ms()
    while not rst.value():
        if time.ticks_diff(time.ticks_ms(), t0) > timeout_s * 1000:
            print("timeout: no power seen on RSTI")
            return False
        time.sleep_ms(20)
    time.sleep_ms(200)          # let reset out negate with EZPCS still low
    cs(1)
    print("target powered, RSTI high after %d ms" % time.ticks_diff(time.ticks_ms(), t0))
    return True


def do_clk():
    hz = measure_fsys()
    print("CLKOUT %d Hz" % hz if hz else "no clock on CLKOUT pin")


def measure_wave(gpio=PIN_MISO, secs=2):
    """Frequency of a square wave on a Pico input (rising edges, IRQ timestamps)."""
    p = Pin(gpio, Pin.IN, pull=None)
    stamps = []
    def cb(_):
        stamps.append(time.ticks_us())
    p.irq(cb, Pin.IRQ_RISING)
    time.sleep(secs)
    p.irq(None)
    if len(stamps) < 3:
        return 0
    return (len(stamps) - 1) * 1_000_000 / time.ticks_diff(stamps[-1], stamps[0])


def do_check(pit_period=6000):
    """Test firmware (fw/main.c) check: PQS0 wave = (fsys/2)/pit_period/2."""
    f = measure_wave(PIN_MISO)
    slow = measure_wave(PIN_SCK, secs=3)
    if not f:
        print("no wave on GP4 (CON5-7): test firmware not running")
        return
    print("GP4 wave %.2f Hz -> fsys = %.3f MHz; GP2 %.2f Hz" % (f, f * 4 * pit_period / 1e6, slow))


# --- debug console: MCU UART1 TX (pin 19, QSPI_DOUT alt2) -> CON5-7 -> GP4 ----

def _uart_rx_prog():
    import rp2

    @rp2.asm_pio(in_shiftdir=rp2.PIO.SHIFT_RIGHT, fifo_join=rp2.PIO.JOIN_RX,
                 autopush=True, push_thresh=8)
    def uart_rx():
        wait(0, pin, 0)             # start bit
        set(x, 7)              [10]  # to the middle of bit 0 (8 cycles/bit)
        label("bitloop")
        in_(pins, 1)
        jmp(x_dec, "bitloop")  [6]
    return uart_rx


def do_console(secs=10, baud=57600, gpio=PIN_MISO):
    """Print what the MCU sends on UART1 for `secs` seconds (8N1, no parity)."""
    import rp2
    import sys
    sm = rp2.StateMachine(1, _uart_rx_prog(), freq=8 * baud,
                          in_base=Pin(gpio, Pin.IN, Pin.PULL_UP))
    sm.active(1)
    t0 = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < secs * 1000:
            if sm.rx_fifo():
                sys.stdout.write(chr((sm.get() >> 24) & 0xFF))
            else:
                time.sleep_ms(1)
    finally:
        sm.active(0)
        Pin(gpio, Pin.IN, pull=None)
