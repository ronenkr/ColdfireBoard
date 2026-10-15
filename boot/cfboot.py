#!/usr/bin/env python3
"""Host tool for the ColdFire USB bootloader (CDC-ACM serial on the board's USB).

  cfboot.py info                 bootloader version, app present?
  cfboot.py flash app.bin        reboot into bootloader, erase, write, verify, run
  cfboot.py verify app.bin       compare flash with a file (CRC32)
  cfboot.py read out.bin [LEN]   read application flash
  cfboot.py run                  leave the bootloader, start the app
  cfboot.py console              show debug output from bootloader / app
  cfboot.py boot                 reboot a running app into the bootloader

The app must be linked at 0x4000 (fw/: make APP=1). Needs pyserial.
"""
import argparse
import glob
import os
import sys
import time
import zlib

import serial
import serial.tools.list_ports

VID = 0x1209
PID_BOOT, PID_APP = 0x0001, 0x0002
APP_BASE, FLASH_END, PAGE, CHUNK = 0x4000, 0x80000, 4096, 1024


_hub_handles = []


def keep_hubs_awake():
    """Linux runtime-suspends idle USB hubs (some with a 0 ms delay), and a
    suspended hub may not report a newly attached device for seconds. Holding a
    hub's usbfs node open keeps it resumed; read-only access needs no root.
    Opening a suspended hub blocks until it has resumed (seconds), so this runs
    in a background thread: the tool must start watching for the bootloader
    immediately, its boot window is only ~1 s."""
    import threading
    threading.Thread(target=_open_hubs, daemon=True).start()


def _open_hubs():
    for d in glob.glob("/sys/bus/usb/devices/*"):
        try:
            if open(d + "/bDeviceClass").read().strip() != "09":
                continue
            bus = int(open(d + "/busnum").read())
            dev = int(open(d + "/devnum").read())
            _hub_handles.append(open("/dev/bus/usb/%03d/%03d" % (bus, dev), "rb"))
        except OSError:
            pass


def find(pids, timeout=0.0):
    t0 = time.time()
    while True:
        for p in serial.tools.list_ports.comports():
            if p.vid == VID and p.pid in pids:
                return p.device, p.pid
        if time.time() - t0 >= timeout:
            return None, None
        time.sleep(0.05)


def open_port(dev, baud=115200, timeout=2, wait=3.0):
    """Open a freshly enumerated tty. udev applies the dialout group a moment
    after the node appears (it starts out root-only), so retry EACCES/EBUSY."""
    t0 = time.time()
    while True:
        try:
            return serial.Serial(dev, baud, timeout=timeout)
        except (serial.SerialException, OSError):
            if time.time() - t0 > wait:
                raise
            time.sleep(0.05)


class Boot:
    def __init__(self, dev):
        self.s = open_port(dev)
        self.s.reset_input_buffer()

    def line(self):
        """Next protocol line; '#' lines are bootloader log output."""
        while True:
            ln = self.s.readline()
            if not ln:
                raise IOError("timeout waiting for bootloader")
            ln = ln.decode(errors="replace").rstrip("\r\n")
            if ln.startswith("#"):
                print("  [board]", ln[1:].strip())
                continue
            if ln:
                return ln

    def cmd(self, text, payload=b"", timeout=2.0):
        self.s.timeout = timeout
        self.s.write(text.encode() + b"\n" + payload)
        try:
            r = self.line()
        finally:
            self.s.timeout = 2.0
        if not r.startswith("OK"):
            raise IOError("%s -> %s" % (text, r))
        return r[2:].strip()

    def info(self):
        return self.cmd("I")

    def erase(self, addr, length):
        # ~20-40 ms per 2 KB step on the board, no USB service meanwhile
        self.cmd("E %x %x" % (addr, length), timeout=5 + length / 2048 * 0.1)

    def write(self, addr, data):
        self.cmd("W %x %x" % (addr, len(data)), data)

    def read(self, addr, length):
        self.cmd("R %x %x" % (addr, length))
        data = self.s.read(length)
        if len(data) != length:
            raise IOError("short read")
        return data

    def crc(self, addr, length):
        return int(self.cmd("C %x %x" % (addr, length), timeout=5 + length / 100000), 16)


def enter_bootloader(wait):
    """Get a bootloader port: already there, or 1200-baud-touch the app."""
    dev, pid = find((PID_BOOT, PID_APP))
    if pid == PID_APP:
        print("app running on %s: rebooting it into the bootloader" % dev)
        try:
            s = open_port(dev, 1200)
            s.dtr = False                # board acks, then detaches and reboots
            s.close()
        except (serial.SerialException, OSError):
            pass
        time.sleep(0.5)
    elif dev is None:
        print("waiting %d s for the board (plug it in / power-cycle it now)" % wait)
    dev, pid = find((PID_BOOT,), timeout=wait)
    if dev is None:
        sys.exit("no bootloader found (USB %04x:%04x)" % (VID, PID_BOOT))
    b = Boot(dev)
    for _ in range(20):                  # first command also stops the boot timeout
        try:
            print("bootloader on %s: %s" % (dev, b.info()))
            return b
        except IOError:
            time.sleep(0.1)
    sys.exit("bootloader on %s does not answer" % dev)


def pad(data):
    return data + b"\xff" * (-len(data) % 4)


def do_flash(b, path):
    data = pad(open(path, "rb").read())
    if APP_BASE + len(data) > FLASH_END:
        sys.exit("image too large: %d bytes" % len(data))
    t0 = time.time()
    b.erase(APP_BASE, len(data))
    for off in range(0, len(data), CHUNK):
        b.write(APP_BASE + off, data[off:off + CHUNK])
        print("\r  write %3d%%" % ((off + CHUNK) * 100 // len(data) if off + CHUNK < len(data) else 100), end="")
    print()
    want = zlib.crc32(data)
    got = b.crc(APP_BASE, len(data))
    if got != want:
        sys.exit("VERIFY FAILED: flash crc %08x, file crc %08x" % (got, want))
    print("  %d bytes written and verified in %.1f s (crc %08x)" % (len(data), time.time() - t0, want))


def do_console():
    print("console: Ctrl-C to quit")
    while True:
        dev, pid = find((PID_BOOT, PID_APP), timeout=3600)
        try:
            with open_port(dev, timeout=0.2) as s:
                print("--- %s (%s) ---" % (dev, "bootloader" if pid == PID_BOOT else "app"))
                while True:
                    d = s.read(256)
                    if d:
                        sys.stdout.write(d.decode(errors="replace"))
                        sys.stdout.flush()
        except (serial.SerialException, OSError):
            print("\n--- disconnected ---")
            time.sleep(0.2)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--wait", type=int, default=30, help="seconds to wait for the board")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    f = sub.add_parser("flash")
    f.add_argument("image")
    f.add_argument("--no-run", action="store_true")
    v = sub.add_parser("verify")
    v.add_argument("image")
    r = sub.add_parser("read")
    r.add_argument("out")
    r.add_argument("length", nargs="?", type=lambda x: int(x, 0), default=FLASH_END - APP_BASE)
    sub.add_parser("run")
    sub.add_parser("console")
    sub.add_parser("boot")
    a = ap.parse_args()

    keep_hubs_awake()
    if a.cmd == "console":
        return do_console()
    b = enter_bootloader(a.wait)
    if a.cmd == "flash":
        do_flash(b, a.image)
        if not a.no_run:
            b.cmd("G")
            print("  app started")
    elif a.cmd == "verify":
        data = pad(open(a.image, "rb").read())
        ok = b.crc(APP_BASE, len(data)) == zlib.crc32(data)
        print("verify", "OK" if ok else "MISMATCH")
        if "app=1" in b.info():
            b.cmd("G")                   # back to the app
        sys.exit(0 if ok else 1)
    elif a.cmd == "read":
        with open(a.out, "wb") as fo:
            for off in range(0, a.length, CHUNK):
                fo.write(b.read(APP_BASE + off, min(CHUNK, a.length - off)))
        print("read %d bytes" % a.length)
        if "app=1" in b.info():
            b.cmd("G")
    elif a.cmd == "run":
        b.cmd("G")


if __name__ == "__main__":
    main()
