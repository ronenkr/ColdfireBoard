# RP2040 EzPort programmer for the SafeQ MCF52254 board

EzPort = the MCF5225x SPI-flash-compatible programming port (MCF52259RM ch. 23).
Holding EZPCS (=RCON) low while the MCU leaves reset puts it in EzPort mode;
the core stays halted and the flash answers SPI commands.

## Wiring (board powered from its own USB, Pico only shares GND)

| CON5 | MCU pin | Signal          | Pico         |
|------|---------|-----------------|--------------|
| 1    | 97      | RSTI            | GP6          |
| 2    | 4       | RCON / EZPCS    | GP5          |
| 3    | –       | **GND**         | **GND**      |
| 4    | 21      | QSPI_CS0 (not EzPort) | not connected |
| 5    | 20      | QSPI_CLK / EZPCK| GP2          |
| 6    | 18      | QSPI_DIN / EZPD | GP3          |
| 7    | 19      | QSPI_DOUT / EZPQ| GP4          |
| 8    | –       | **VCC 3.3 V**   | **not connected** (wiring it to Pico GND shorts the board supply) |
| –    | 31      | CLKOUT (optional, solder to chip) | GP7 |

Put 100–220 Ω series resistors in the signal lines.

## Use

1. Flash MicroPython onto the Pico, `pip install mpremote`.
2. `./ezflash.py info` – must report a status, not "no EzPort response".
3. `./ezflash.py dump safeq_backup.bin` – only works if the flash is not secured.
4. Find fsys (needed for the flash clock divider, which is write-once per reset
   and must give 150–200 kHz – a wrong value can overstress the flash):
   `./ezflash.py clk` if CLKOUT is wired, otherwise crystal Y1 marking + CLKMOD pins 99/100.
5. `./ezflash.py flash fw.bin --fsys <Hz>`

## Notes

- The image must contain the CFM config field at 0x400–0x417 with
  CFMPROT = 0 and CFMSEC != 0x4AC8; `ezflash.py` refuses images that would
  secure or write-protect the flash. An erased config field loads
  CFMPROT = 0xFFFFFFFF (all sectors protected) at the next reset, so erase and
  program always happen in one EzPort session.
- A secured chip is bulk-erased and reset first. If programming then fails with
  "protected sector", the erased config field has protected the flash, and a
  BDM probe is needed once to write CFMPROT = 0.
