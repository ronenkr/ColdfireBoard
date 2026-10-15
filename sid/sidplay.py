#!/usr/bin/env python3
"""Play a C64 .sid tune on the board (fw/sidplay.c must be the running app).

  sidplay.py TUNE.sid [--song N] [--seconds S] [--boost B] [--gain G]
  sidplay.py --list DIR              list tunes (name, author, songs)

The tune's 6502 code runs on the PC (sid2log.py); the SID register frames are
streamed over USB and the board emulates the SID chip. Conversions are cached
in build/sidl/. Ctrl-C stops playback.
"""
import argparse
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "boot"))
sys.path.insert(0, HERE)
import cfboot   # noqa: E402
import sid2log  # noqa: E402

CACHE = os.path.join(HERE, "..", "build", "sidl")


def get_log(path, song, seconds):
    os.makedirs(CACHE, exist_ok=True)
    key = "%s.s%s.%ds.sidl" % (os.path.splitext(os.path.basename(path))[0], song or 0, seconds)
    cached = os.path.join(CACHE, key)
    if os.path.exists(cached) and os.path.getmtime(cached) >= os.path.getmtime(path):
        tune = sid2log.PSID(open(path, "rb").read())
        return tune, open(cached, "rb").read()
    t0 = time.time()
    tune, log = sid2log.convert(path, song, seconds)
    open(cached, "wb").write(log)
    print("converted in %.1f s" % (time.time() - t0))
    return tune, log


def frames(log):
    if log[:4] != b"SIDL":
        raise ValueError("not a SIDL log")
    frame_us, n = struct.unpack("<HI", log[6:12])
    p = 12
    for _ in range(n):
        mask = struct.unpack("<I", log[p:p + 4])[0]
        k = bin(mask & 0x1FFFFFF).count("1")
        yield log[p:p + 4 + k]
        p += 4 + k


class Board:
    def __init__(self, wait=5):
        dev, pid = cfboot.find((cfboot.PID_APP,), timeout=wait)
        if not dev:
            sys.exit("board app not found on USB (flash fw/sidplay.c: make APP=1 PROG=sidplay)")
        self.s = cfboot.open_port(dev, timeout=1)
        self.s.write_timeout = 10             # board not reading for 10 s = stuck
        self.s.write(b"X")
        self.s.reset_input_buffer()
        st = self.status()
        if not st.startswith("SIDPLAY"):
            sys.exit("running app is not the SID player (got %r)" % st)

        self.button = False

    def poll(self):
        """Handle lines from the board; returns True if the button was pressed."""
        while self.s.in_waiting:
            line = self.s.readline().decode(errors="replace").strip()
            if line == "BTN":
                self.button = True
        b, self.button = self.button, False
        return b

    def status(self):
        self.s.write(b"Q")
        while True:
            line = self.s.readline().decode(errors="replace").strip()
            if line == "BTN":
                self.button = True
            elif line.startswith("SIDPLAY") or not line:
                return line


def play(b, path, song, seconds, gain, boost):
    """Stream one tune. Returns 'done', 'button' or raises KeyboardInterrupt."""
    tune, log = get_log(path, song, seconds)
    frame_us, n = struct.unpack("<HI", log[6:12])
    print("%s - %s  (song %d/%d, %.1f Hz frames)" % (
        tune.name, tune.author, song or tune.start or 1, tune.songs, 1e6 / frame_us))
    b.poll()                                  # forget presses during conversion
    b.s.write(b"G" + bytes([gain]) + b"A" + bytes([boost]))
    b.s.write(b"T" + struct.pack("<H", frame_us))
    buf = bytearray()
    for i, fr in enumerate(frames(log)):
        buf += b"F" + fr
        if len(buf) > 256:
            b.s.write(buf)                    # blocks while the board's FIFO is full
            buf.clear()
            if b.poll():
                print("\r  [button] next tune")
                return "button"
        if i % 250 == 0:
            print("\r  %3d s" % (i * frame_us // 1_000_000), end="", flush=True)
    b.s.write(buf)
    while True:                               # let the FIFO drain
        st = b.status()
        if b.poll():
            print("\r  [button] next tune")
            return "button"
        if " fifo=0 " in st:
            break
        time.sleep(0.2)
    print("\r  done: " + st)
    return "done"


def tune_files(args):
    out = []
    for a in args:
        if os.path.isdir(a):
            out += [os.path.join(a, f) for f in sorted(os.listdir(a)) if f.lower().endswith(".sid")]
        else:
            out.append(a)
    return out


def list_tunes(d):
    for f in sorted(os.listdir(d)):
        if f.lower().endswith(".sid"):
            try:
                t = sid2log.PSID(open(os.path.join(d, f), "rb").read())
                print("%-40s %-32s %-24s %d" % (f, t.name[:32], t.author[:24], t.songs))
            except ValueError as e:
                print("%-40s (%s)" % (f, e))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tunes", nargs="*", help=".sid files and/or directories")
    ap.add_argument("--shuffle", action="store_true")
    ap.add_argument("--song", type=int)
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--boost", type=int, default=12,
                    help="AGC: max boost for quiet passages, 0 = off (default 12)")
    ap.add_argument("--gain", type=int, default=64, help="fixed gain when --boost 0 (default 64)")
    ap.add_argument("--list", metavar="DIR")
    a = ap.parse_args()
    if a.list:
        return list_tunes(a.list)
    files = tune_files(a.tunes)
    if not files:
        ap.error("give .sid files or a directory (or --list DIR)")
    if a.shuffle:
        import random
        random.shuffle(files)
    gain, boost = max(1, min(255, a.gain)), max(0, min(40, a.boost))
    cfboot.keep_hubs_awake()
    b = Board()
    try:
        while True:
            for f in files:
                try:
                    play(b, f, a.song, a.seconds, gain, boost)
                except (ValueError, RuntimeError) as e:
                    print("skip %s: %s" % (os.path.basename(f), e))
                except (OSError, cfboot.serial.SerialException):
                    print("\r  board disconnected - waiting for it to come back")
                    try:
                        b.s.close()
                    except OSError:
                        pass
                    b = Board(wait=3600)
            if len(files) == 1:
                break
    except KeyboardInterrupt:
        print("\r  stopped")
    finally:
        try:
            b.s.write(b"X")
            b.s.close()
        except (OSError, cfboot.serial.SerialException):
            pass


if __name__ == "__main__":
    main()
