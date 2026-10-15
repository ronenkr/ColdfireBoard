# ColdFire SafeQ board: USB bootloader and SID chiptune player

This project repurposes a **Y Soft "SafeQ USB v3.0.2"** printer card-reader
board as a small chiptune player. The board has an NXP/Freescale
**MCF52254** ColdFire V2 MCU, a piezo buzzer and an RGB LED. It now runs:

- **A USB bootloader** that does everything over the board's own micro-USB:
  upload, verify, debug console, and reboot into the bootloader from a running
  app.
- **A C64 SID player** with a 3-voice SID chip emulation: oscillators,
  ring/sync, ADSR, filter and an AGC. It plays a bank of tunes from flash from
  power-up and also accepts tunes streamed from a PC. A button skips to the
  next tune, and the LED follows the voices.

Photos of the board are in [`docs/`](docs/).

## Hardware notes

| Item | Details |
|---|---|
| MCU | MCF52254AF80, 100 LQFP. Runs at 48 MHz from a 12 MHz crystal (CLKMOD = 11, PLL). 512 KB flash, 64 KB SRAM. |
| Piezo | MCU pin 5 (PTC0 / PWM0), used as an 8-bit PWM DAC |
| RGB LED | TI LP5562 at I2C address 0x30 on I2C1 (pin 64 SCL, pin 65 SDA). PWM registers: R `04`, G `03`, B `02`. |
| CON5 (EzPort) | 1 RSTI, 2 RCON/EZPCS, **3 GND**, 4 QSPI_CS0, 5 EZPCK, 6 EZPD, 7 EZPQ (UART1 TX), **8 = 3.3 V** |
| Button | CON5-5 (pin 20, PQS2) to GND, through about 1 kΩ, with the internal pull-up |

## Repository layout

| Path | Contents |
|---|---|
| [`boot/`](boot/) | USB bootloader (CDC-ACM), `cfboot.py` host tool, hardware self-test. See [boot/README.md](boot/README.md). |
| [`fw/`](fw/) | Applications for the bootloader: `sidplay.c` (SID player), `hwtest.c` (I2C, LED and piezo console), `audiotest.c`, `main.c` (test app). Also `mcf52254.h` register definitions. |
| [`sid/`](sid/) | PC side of the SID player. `sid2log.py` runs the C64 tune in a 6502 emulator, `sidz.py` / `sidz_dec.h` handle the compressed register streams, `mkbank.py` builds the tune bank, `sidplay.py` streams over USB, and `picks.txt` is the current playlist. |
| [`ezport/`](ezport/) | RP2040 (MicroPython) EzPort programmer. Use it to install or update the bootloader itself, or to recover the board. See [ezport/README.md](ezport/README.md). |
| [`tools/`](tools/) | `setup_toolchain.sh` fetches the m68k GCC without root. |
| [`build/`](build/) | Prebuilt images: `sidbox.bin` (player + tune bank), plus small test apps used by the self-test |

**Prebuilt binaries:**
- `boot/boot.bin`: bootloader, flashed with EzPort at address 0.
- `fw/fw.bin`: SID player app (0x4000).
- `build/sidbox.bin`: SID player plus 21 tunes, 4 minutes each.

## Quick start

```sh
tools/setup_toolchain.sh                              # m68k-linux-gnu-gcc 11 into toolchain/
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt

# flash the player + tune bank over the board's USB (bootloader installed)
.venv/bin/python boot/cfboot.py flash build/sidbox.bin

# rebuild the bank from a playlist (names looked up in --dir)
.venv/bin/python sid/mkbank.py --playlist sid/picks.txt --fit --seconds 240

# stream any .sid from the PC (the board takes over again afterwards)
.venv/bin/python sid/sidplay.py SOME_TUNE.sid
.venv/bin/python sid/sidplay.py SID_DIRECTORY --shuffle

# build an app
cd fw && make APP=1 PROG=sidplay && ../.venv/bin/python ../boot/cfboot.py flash fw.bin
```

## Memory map

| Range | Contents |
|---|---|
| `0x00000–0x03FFF` | bootloader. CFM configuration at 0x400 (unsecured, unprotected); service table at 0x418. |
| `0x04000–0x0FFFF` | application, 48 KB max when a tune bank is used |
| `0x10000–0x7FFFF` | tune bank, `SIDB` format (`sid/mkbank.py`) |
| SRAM `0x20000000` | the app's vector table, 1 KB (VBR must be 1 MB aligned) |
| SRAM `0x20000400–0x2000DFFF` | app RAM |
| SRAM `0x2000E000–0x2000FFFF` | bootloader USB state |
