#!/usr/bin/env python3
"""Hardware self-test of the USB bootloader (needs the RP2040 on CON5 for
resets and the square-wave check). Run from the project root:

    .venv/bin/python boot/selftest.py

Uses the images in build/ (app_usbdemo, app_hwtest, app_nousb, app_crash,
big_random) and boot/boot.bin for the bootloader-integrity check.
"""
import glob
import subprocess
import sys
import time
import zlib

sys.path.insert(0, "boot")
import cfboot  # noqa: E402

PY = ".venv/bin/python"
MPREMOTE = ".venv/bin/mpremote"
B = "build/"
results = []


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    log("%s  %s %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


def pico(code, timeout=60):
    dev = glob.glob("/dev/serial/by-id/usb-MicroPython*")[0]
    r = subprocess.run([MPREMOTE, "connect", dev, "exec", code],
                       capture_output=True, text=True, timeout=timeout)
    return r.stdout.strip()


def board_reset():
    """Pulse RSTI (simulated power cycle); Pico pins left as inputs."""
    pico("from machine import Pin\nimport time\n"
         "for g in (2,3,4,5): Pin(g, Pin.IN, pull=None)\n"
         "r = Pin(6, Pin.OUT, value=0); time.sleep_ms(30); r.init(Pin.IN, pull=None)")


def wave_hz():
    out = pico("import ezport\nprint(ezport.measure_wave(4, 2))")
    try:
        return float(out.splitlines()[-1])
    except (ValueError, IndexError):
        return 0.0


def usb_mode():
    dev, pid = cfboot.find((cfboot.PID_BOOT, cfboot.PID_APP))
    return {cfboot.PID_BOOT: "boot", cfboot.PID_APP: "app"}.get(pid)


def wait_mode(want, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if usb_mode() == want:
            return time.time() - t0
        time.sleep(0.1)
    return None


def cli(*args, timeout=120):
    r = subprocess.run([PY, "boot/cfboot.py", "--wait", "20", *args],
                       capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def app_talks(kind):
    """Open the app's port: usbdemo prints ticks, hwtest answers '?'."""
    import serial
    dev, _ = cfboot.find((cfboot.PID_APP,), timeout=5)
    if not dev:
        return False
    with cfboot.open_port(dev, timeout=0.2) as s:
        buf = b""
        if kind == "hwtest":
            time.sleep(0.3)
            s.write(b"?\r")
        t0 = time.time()
        while time.time() - t0 < 2.5:
            buf += s.read(256)
    return (b"app tick" in buf) if kind == "usbdemo" else (b"hwtest" in buf)


def raw_session():
    dev, pid = cfboot.find((cfboot.PID_BOOT,), timeout=20)
    b = cfboot.Boot(dev)
    b.info()
    return b


def raw(b, line):
    b.s.write(line.encode() + b"\n")
    return b.line()


def boot_region_crc(b):
    img = open("boot/boot.bin", "rb").read()
    want = zlib.crc32(img + b"\xff" * (0x4000 - len(img)))
    return b.crc(0, 0x4000) == want


def ensure_test_images():
    """big_random.bin (fills the whole app area) is generated, not stored."""
    import os
    path = B + "big_random.bin"
    if not os.path.exists(path):
        open(path, "wb").write(os.urandom(cfboot.FLASH_END - cfboot.APP_BASE))
    for name in ("app_usbdemo", "app_hwtest", "app_nousb", "app_crash"):
        if not os.path.exists(B + name + ".bin"):
            sys.exit("missing %s%s.bin (see boot/README.md, Testing)" % (B, name))


def main():
    t_start = time.time()
    ensure_test_images()
    cfboot.keep_hubs_awake()
    if "--recovery-only" in sys.argv:
        return recovery_only(t_start)
    # ---- 1. bootloader reachable, integrity, protection -------------------
    rc, out = flash_with_reset(B + "app_hwtest.bin")
    check("start from a known app (reset-assisted)", rc == 0, out.strip()[-80:])
    rc, out = cli("boot")
    check("enter bootloader from any state", rc == 0, out.strip().splitlines()[-1] if out else "")
    b = raw_session()
    check("bootloader region CRC matches boot.bin", boot_region_crc(b))
    for line, label in (("E 0 800", "erase bootloader page refused"),
                        ("E 3000 2000", "erase overlapping bootloader refused"),
                        ("W 3ffc 4", "write below 0x4000 refused"),
                        ("E 7f000 2000", "erase past end of flash refused"),
                        ("R 80000 100", "read past end refused"),
                        ("W 4001 4", "misaligned write refused"),
                        ("W 4000 404", "oversized write refused"),
                        ("Q", "unknown command refused")):
        r = raw(b, line)
        check(label, r.startswith("ERR"), r)
    check("bootloader region intact after abuse", boot_region_crc(b))
    peek = b.read(0x40000000, 4)
    check("register peek (IPSBAR)", peek[:3] == b"\x40\x00\x00", peek.hex())
    b.s.close()

    # ---- 2. full-size image, boundaries, read-back ------------------------
    big = open(B + "big_random.bin", "rb").read()
    t0 = time.time()
    rc, out = cli("flash", B + "big_random.bin", "--no-run", timeout=600)
    dt = time.time() - t0
    check("496 KB image write + CRC verify", rc == 0 and "verified" in out, "%.1f s" % dt)
    b = raw_session()
    got = bytearray()
    for off in range(0, len(big), 1024):
        got += b.read(0x4000 + off, min(1024, len(big) - off))
    check("496 KB byte-for-byte read-back", bytes(got) == big)
    check("last flash word written", b.read(0x7FFFC, 4) == big[-4:])
    check("bootloader region intact after full-size write", boot_region_crc(b))
    b.s.close()

    # ---- 3. repeated update cycles from a running app ---------------------
    for i in range(10):
        kind = ("usbdemo", "hwtest")[i % 2]
        rc, out = cli("flash", B + "app_%s.bin" % kind)
        up = wait_mode("app", 10)
        talks = up is not None and app_talks(kind)
        check("update cycle %2d -> %s" % (i + 1, kind), rc == 0 and talks,
              "app up after %.1fs" % up if up else out.strip()[-120:])

    # ---- 4. idle-host power-ups --------------------------------------------
    for i in range(5):
        board_reset()
        t0 = time.time()
        saw_boot = wait_mode("boot", 8) is not None
        up = wait_mode("app", 10)
        check("idle power-up %d: bootloader then app" % (i + 1),
              saw_boot and up is not None and app_talks("hwtest"),
              "app %.1fs after reset" % (time.time() - t0) if up else "no app")

    recovery_tests()
    summary(t_start)


def recovery_tests():
    # ---- 5. recovery from apps that can't talk USB ---------------------------
    for kind in ("nousb", "crash"):
        rc, out = cli("flash", B + "app_%s.bin" % kind)
        time.sleep(3)
        mode = usb_mode()
        hz = wave_hz()
        if kind == "nousb":
            check("no-USB app runs (no port, 2 kHz wave)", mode is None and 1900 < hz < 2100,
                  "usb=%s wave=%.0f Hz" % (mode, hz))
        else:
            # the fault resets the CPU: it loops bootloader -> crash, never a usable app
            check("crashing app never comes up as an app", mode != "app",
                  "usb=%s wave=%.0f Hz" % (mode, hz))
        p = subprocess.Popen([PY, "-u", "boot/cfboot.py", "--wait", "25", "flash",
                              B + "app_hwtest.bin"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        time.sleep(1.5)
        log("  resetting board (= user re-plugs it)")
        board_reset()                         # = user re-plugs the board
        log("  reset done")
        out = ""
        for line in iter(p.stdout.readline, ""):
            out += line
            log("  cfboot: " + line.rstrip())
        p.wait()
        up = wait_mode("app", 10)
        check("recover from %s app via boot window" % kind,
              p.returncode == 0 and up is not None and app_talks("hwtest"), out.strip()[-100:])

    # ---- 6. interrupted upload, then power loss ----------------------------
    p = subprocess.Popen([PY, "boot/cfboot.py", "--wait", "20", "flash", B + "big_random.bin"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(4)
    p.kill()
    p.wait()
    time.sleep(0.5)
    check("bootloader still answers after aborted upload", usb_mode() == "boot")
    board_reset()                             # power loss mid-update
    rc, out = cli("info")
    check("after power loss: bootloader reachable", rc == 0, out.strip().splitlines()[-1] if out else "")
    rc, out = cli("flash", B + "app_hwtest.bin")
    up = wait_mode("app", 10)
    check("re-flash after interrupted upload", rc == 0 and up is not None and app_talks("hwtest"))


def flash_with_reset(image):
    """Flash even if the current app has no USB: reset while cfboot waits."""
    p = subprocess.Popen([PY, "-u", "boot/cfboot.py", "--wait", "25", "flash", image],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(1.5)
    if p.poll() is None and usb_mode() != "boot":
        board_reset()
    out = p.communicate(timeout=120)[0]
    return p.returncode, out


def recovery_only(t_start):
    rc, out = flash_with_reset(B + "app_hwtest.bin")
    check("start from a known app", rc == 0, out.strip()[-80:])
    recovery_tests()
    summary(t_start)


def summary(t_start):
    failed = [r for r in results if not r[1]]
    print("\n%d checks, %d failed, %.0f s" % (len(results), len(failed), time.time() - t_start))
    for name, _, detail in failed:
        print("  FAILED:", name, detail)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
