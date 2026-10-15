#!/usr/bin/env python3
"""Convert a C64 PSID tune into a SID register log for the board's SID emulator.

The tune's own 6502 code runs here (py65): init once, then the play routine
once per frame (50 Hz VBI, or the CIA1 timer A rate for "speed" tunes, or the
IRQ handler the tune installed when play = 0). After each frame the 25 SID
registers $D400-$D418 are compared with the previous frame and the changes are
written out.

Log format (little endian), shared with the board player:
  "SIDL", u8 version=1, u8 flags (bit0 = NTSC), u16 frame_us, u32 frames
  then per frame: u32 mask (bit n = register n changed), changed values in
  register order.

  sid2log.py tune.sid out.sidl [--song N] [--seconds S]
"""
import argparse
import struct
import sys

from py65.devices.mpu6502 import MPU

PAL_CLOCK, NTSC_CLOCK = 985248, 1022727
SID_BASE = 0xD400
KERNAL_IRQ_EXITS = (0xEA31, 0xEA81, 0xEA7E, 0xFEBC, 0xFF6E)
RETURN_SENTINEL = 0x0000          # code never runs at $0000 (CPU port register)


class PSID:
    def __init__(self, data):
        if data[:4] not in (b"PSID", b"RSID"):
            raise ValueError("not a PSID/RSID file (%r)" % data[:4])
        (self.version, off, self.load, self.init, self.play, self.songs,
         self.start, self.speed) = struct.unpack(">HHHHHHHI", data[4:22])
        self.rsid = data[:4] == b"RSID"
        self.name = data[0x16:0x36].split(b"\0")[0].decode("latin-1")
        self.author = data[0x36:0x56].split(b"\0")[0].decode("latin-1")
        self.flags = struct.unpack(">H", data[0x76:0x78])[0] if self.version >= 2 else 0
        body = data[off:]
        if self.load == 0:
            self.load = body[0] | body[1] << 8
            body = body[2:]
        self.body = body
        self.ntsc = ((self.flags >> 2) & 3) == 2


class C64:
    """Just enough C64 for PSID players: 64 KB RAM, a SID write latch and the
    CIA1 timer A latch (read back to find the play rate)."""

    def __init__(self, tune):
        self.mem = bytearray(65536)
        self.mem[tune.load:tune.load + len(tune.body)] = tune.body
        self.mem[0x0001] = 0x37
        self.mpu = MPU(memory=self.mem)
        self.cycles = 0

    def call(self, addr, a=0, limit=2_000_000, irq=False):
        """JSR (or IRQ entry) to addr, run until it returns. Returns cycles."""
        m = self.mpu
        m.a, m.x, m.y = a & 0xFF, 0, 0
        ret = RETURN_SENTINEL - 1 & 0xFFFF            # RTS returns to ret+1
        m.sp = 0xFF
        if irq:                                       # RTI pops P, then PC
            for b in (RETURN_SENTINEL >> 8, RETURN_SENTINEL & 0xFF, 0x20):
                self.mem[0x100 + m.sp] = b
                m.sp = (m.sp - 1) & 0xFF
        else:
            for b in (ret >> 8, ret & 0xFF):
                self.mem[0x100 + m.sp] = b
                m.sp = (m.sp - 1) & 0xFF
        m.pc = addr
        start = m.processorCycles
        while m.pc != RETURN_SENTINEL:
            if m.pc in KERNAL_IRQ_EXITS and irq:      # handler jumped to the ROM exit
                break
            m.step()
            if m.processorCycles - start > limit:
                raise RuntimeError("routine at $%04X did not return" % addr)
        return m.processorCycles - start

    def sid_regs(self):
        return bytes(self.mem[SID_BASE:SID_BASE + 25])


def convert(path, song=None, seconds=120.0, progress=False):
    tune = PSID(open(path, "rb").read())
    if tune.rsid:
        raise ValueError("RSID tunes need a full C64 environment (not supported)")
    song = (song or tune.start or 1)
    if not 1 <= song <= tune.songs:
        raise ValueError("song %d out of range 1..%d" % (song, tune.songs))
    c64 = C64(tune)
    c64.call(tune.init, a=song - 1, limit=20_000_000)

    clock = NTSC_CLOCK if tune.ntsc else PAL_CLOCK
    cia_speed = (tune.speed >> min(song - 1, 31)) & 1
    play, irq = tune.play, False
    if play == 0:                                     # init installed an IRQ handler
        play = c64.mem[0x0314] | c64.mem[0x0315] << 8
        if c64.mem[0x0001] & 0x02 == 0 or play < 0x0400:
            play = c64.mem[0xFFFE] | c64.mem[0xFFFF] << 8
        irq = True
    if cia_speed or irq:
        timer = c64.mem[0xDC04] | c64.mem[0xDC05] << 8
        if timer == 0:                                # init kept the KERNAL default
            timer = 0x4025                            # (~60 Hz on PAL)
        frame_us = round((timer + 1) * 1e6 / clock)
    else:
        frame_us = 16667 if tune.ntsc else 20000

    frames = int(seconds * 1e6 / frame_us)
    out = bytearray()
    prev = bytes(25)
    for f in range(frames):
        c64.call(play, irq=irq)
        regs = c64.sid_regs()
        mask, vals = 0, bytearray()
        for r in range(25):
            if regs[r] != prev[r] or f == 0:
                mask |= 1 << r
                vals.append(regs[r])
        out += struct.pack("<I", mask) + vals
        prev = regs
        if progress and f % 500 == 0:
            print("\r  %s: frame %d/%d" % (tune.name, f, frames), end="", file=sys.stderr)
    if progress:
        print(file=sys.stderr)
    head = b"SIDL" + struct.pack("<BBHI", 1, 1 if tune.ntsc else 0, frame_us, frames)
    return tune, head + out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sid")
    ap.add_argument("out")
    ap.add_argument("--song", type=int)
    ap.add_argument("--seconds", type=float, default=120.0)
    a = ap.parse_args()
    tune, log = convert(a.sid, a.song, a.seconds, progress=True)
    open(a.out, "wb").write(log)
    print("%s by %s: %d bytes" % (tune.name, tune.author, len(log)))


if __name__ == "__main__":
    main()
