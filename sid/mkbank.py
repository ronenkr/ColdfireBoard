#!/usr/bin/env python3
"""Build a flash image with the SID player plus a bank of tunes, so the board
plays on its own as soon as it has power.

  mkbank.py TUNE.sid|DIR ... [--seconds 120] [--shuffle] [-o build/sidbox.bin]
  mkbank.py --playlist picks.txt [--dir SIDDIR] --fit
  boot/cfboot.py flash build/sidbox.bin

Playlist file: one tune per line, "name.sid [song] [seconds]" ('#' comments);
names are looked up in --dir. --fit keeps every listed tune and shortens all
clips evenly (not below --min-seconds) until they fit.

Layout (flash addresses): player app at 0x4000 (max 48 KB), tune bank at
0x10000 up to the end of flash (448 KB). Each tune is 4 SIDZ streams (voice
1/2/3, filter+volume), see sidz.py. Tunes are added in order until the bank is
full; the rest are listed as skipped.

Bank header: "SIDB", u16 version 1, u16 count, then count x 64-byte entries:
  u32 stream offset[4] (from bank start), u16 frame_us, u16 0, u32 frames,
  char title[24], char author[16]
"""
import argparse
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sid2log  # noqa: E402
import sidz     # noqa: E402

APP_BASE, BANK_BASE, FLASH_END = 0x4000, 0x10000, 0x80000
ENTRY = 64


def split(frames):
    out = [[] for _ in sidz.GROUPS]
    for mask, vals in frames:
        k = 0
        for g, (base, cnt) in enumerate(sidz.GROUPS):
            m = (mask >> base) & ((1 << cnt) - 1)
            n = bin(m).count("1")
            out[g].append((m << base, vals[k:k + n]))
            k += n
    return out


def tune_files(args):
    out = []
    for a in args:
        if os.path.isdir(a):
            out += [os.path.join(a, f) for f in sorted(os.listdir(a)) if f.lower().endswith(".sid")]
        else:
            out.append(a)
    return out


def read_playlist(path, d):
    jobs = []
    for line in open(path):
        line = line.split("#")[0].split()
        if not line:
            continue
        f = line[0] if os.path.isabs(line[0]) or not d else os.path.join(d, line[0])
        if not f.lower().endswith(".sid"):
            f += ".sid"
        song = int(line[1]) if len(line) > 1 else None
        secs = int(line[2]) if len(line) > 2 else None
        jobs.append((f, song, secs))
    return jobs


def pack(jobs, a, capacity, scale):
    """Convert + compress tunes in order until the bank is full.
    Returns entries, data, skipped [(file, why, size or None)]."""
    table_size = 8 + ENTRY * min(len(jobs), a.max_tunes)
    entries, data, skipped = [], bytearray(), []
    misses = 0
    for f, song, secs in jobs:
        secs = int((secs or a.seconds) * scale)
        if len(entries) >= a.max_tunes or (not a.fit and (misses >= 5 or
                capacity - table_size - len(data) < 1500)):
            skipped.append((f, "bank full", None))
            continue
        try:
            tune, log = sid2log.convert(f, song, secs)
        except Exception as e:                     # RSID, packed, broken players
            skipped.append((f, str(e)[:50], None))
            continue
        frame_us, frames = sidz.parse_sidl(log)
        streams = [sidz.compress(p) for p in split(frames)]
        size = sum(len(x) for x in streams)
        if size > a.max_size and not a.fit:
            skipped.append((f, "compresses to %d B (> --max-size)" % size, None))
            continue
        if table_size + len(data) + size > capacity:
            skipped.append((f, "too big for the space left", size))
            misses += 1
            continue
        offs = []
        for x in streams:
            offs.append(len(data))
            data += x
        entries.append((offs, frame_us, len(frames), tune.name, tune.author, size))
        print("  %2d  %-30s %-20s %4d s %6d B  (%.1fx)" % (
            len(entries) - 1, tune.name[:30], tune.author[:20], secs, size, len(log) / size))
    return entries, data, skipped


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tunes", nargs="*")
    ap.add_argument("--playlist", help="file with one tune per line: name.sid [song] [seconds]")
    ap.add_argument("--dir", default="/media/user/data/RetroPlayer/tracker/chiptunes/sid",
                    help="where playlist names are looked up")
    ap.add_argument("--fit", action="store_true",
                    help="keep every tune, shorten all clips until they fit")
    ap.add_argument("--min-seconds", type=int, default=60)
    ap.add_argument("--seconds", type=int, default=120, help="length of each tune")
    ap.add_argument("--shuffle", action="store_true")
    ap.add_argument("--max-tunes", type=int, default=255)
    ap.add_argument("--max-size", type=int, default=16000,
                    help="skip tunes that compress worse than this (bytes, default 16000)")
    ap.add_argument("--app", default=os.path.join(HERE, "..", "fw", "fw.bin"))
    ap.add_argument("-o", "--out", default=os.path.join(HERE, "..", "build", "sidbox.bin"))
    a = ap.parse_args()

    app = open(a.app, "rb").read()
    if len(app) > BANK_BASE - APP_BASE:
        sys.exit("app is %d bytes, max %d" % (len(app), BANK_BASE - APP_BASE))
    files = tune_files(a.tunes)
    if a.shuffle:
        random.shuffle(files)
    capacity = FLASH_END - BANK_BASE
    jobs = [(f, None, None) for f in files]
    if a.playlist:
        jobs = read_playlist(a.playlist, a.dir)
    scale = 1.0
    for attempt in range(6):
        entries, data, skipped = pack(jobs, a, capacity, scale)
        if not a.fit or not skipped or attempt == 5:
            break
        need = sum(e[5] for e in entries) + sum(sz for _, _, sz in skipped if sz)
        new_scale = scale * (capacity - 8 - ENTRY * len(jobs)) / need * 0.97
        if new_scale * a.seconds < a.min_seconds:
            new_scale = a.min_seconds / a.seconds
        if new_scale >= scale:
            break
        scale = new_scale
        print("-- %d tune(s) did not fit, retrying with clips of %d s" % (len(skipped), a.seconds * scale))
    if not entries:
        sys.exit("no tunes fitted")
    table = bytearray(b"SIDB" + struct.pack("<HH", 1, len(entries)))
    base = 8 + ENTRY * len(entries)
    for offs, frame_us, n, title, author, _ in entries:
        e = struct.pack("<4IHHI", *[base + o for o in offs], frame_us, 0, n)
        e += title.encode("latin-1", "replace")[:23].ljust(24, b"\0")
        e += author.encode("latin-1", "replace")[:15].ljust(16, b"\0")
        table += e
    bank = table + data
    image = app + b"\xff" * (BANK_BASE - APP_BASE - len(app)) + bank
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    open(a.out, "wb").write(image)
    print("%d tunes, bank %d of %d bytes (%.0f%%), image %s (%d bytes)" % (
        len(entries), len(bank), capacity, 100 * len(bank) / capacity, a.out, len(image)))
    if skipped:
        print("%d skipped:" % len(skipped))
        for f, why, _ in skipped[:12]:
            print("   %s: %s" % (os.path.basename(f), why))


if __name__ == "__main__":
    main()
