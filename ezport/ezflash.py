#!/usr/bin/env python3
"""PC side of the RP2040 EzPort programmer. Needs `pip install mpremote` and a
Pico running MicroPython.

  ezflash.py info                    enter EzPort, print status (secured? etc.)
  ezflash.py clk                     measure MCU CLKOUT (pin 31) -> fsys
  ezflash.py dump backup.bin         read the whole 512 KB flash (unsecured only)
  ezflash.py flash fw.bin --fsys HZ  bulk erase, program, verify, run
  ezflash.py check                   test firmware running? measures fsys
  ezflash.py console [--secs N]      MCU UART1 debug output, 57600 8N1
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VENV_MPREMOTE = os.path.join(HERE, "..", ".venv", "bin", "mpremote")
MPREMOTE = VENV_MPREMOTE if os.path.exists(VENV_MPREMOTE) else "mpremote"


def mp(*args):
    cmd = [MPREMOTE, "connect", os.environ.get("EZ_PORT", "auto"), *args]
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit(r.returncode)


def upload_module():
    mp("cp", os.path.join(HERE, "ezport.py"), ":ezport.py")


def run(code):
    mp("exec", "import ezport\n" + code)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    sub.add_parser("clk")
    sub.add_parser("check", help="verify the fw/ test firmware runs, report fsys")
    c = sub.add_parser("console", help="show MCU UART1 debug output (CON5-7)")
    c.add_argument("--secs", type=int, default=10)
    d = sub.add_parser("dump")
    d.add_argument("out")
    f = sub.add_parser("flash")
    f.add_argument("image", help="raw binary, linked at 0x00000000")
    f.add_argument("--fsys", type=int, required=True,
                   help="MCU system clock in Hz while in EzPort mode (see `clk`)")
    f.add_argument("--no-run", action="store_true", help="stay in reset after flashing")
    a = ap.parse_args()

    upload_module()
    if a.cmd == "info":
        run("ezport.do_info()")
    elif a.cmd == "clk":
        run("ezport.do_clk()")
    elif a.cmd == "check":
        run("ezport.do_check()")
    elif a.cmd == "console":
        run("ezport.do_console(%d)" % a.secs)
    elif a.cmd == "dump":
        run("ezport.do_dump('/dump.bin')")
        mp("cp", ":/dump.bin", a.out)
        mp("rm", ":/dump.bin")
    elif a.cmd == "flash":
        mp("cp", a.image, ":/fw.bin")
        run("ezport.do_flash('/fw.bin', %d, run=%s)" % (a.fsys, not a.no_run))
        mp("rm", ":/fw.bin")


if __name__ == "__main__":
    main()
