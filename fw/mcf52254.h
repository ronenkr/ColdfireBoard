/*
 * mcf52254.h - register definitions for the NXP/Freescale MCF52254 (ColdFire V2,
 * MCF5225x family, 100-LQFP), as needed by a bare-metal USB DFU bootloader.
 *
 * Every address and bit below was taken from the "MCF52259 ColdFire Integrated
 * Microcontroller Reference Manual, Rev. 4" (MCF52259RM). Section numbers in the
 * comments refer to that manual. Items that could NOT be confirmed from the manual
 * are marked "NOT IN RM".
 *
 * All module offsets are relative to IPSBAR, reset value 0x4000_0001 (RM 14.5.1,
 * Table 14-1), i.e. the IPS space is at 0x4000_0000.
 *
 * MCF52254 = 512 KB flash, 64 KB SRAM, 100 LQFP (RM Table 1-2, Table 13-7).
 */
#ifndef MCF52254_H
#define MCF52254_H

#include <stdint.h>

#define IPSBAR_BASE         0x40000000u

#define REG8(off)   (*(volatile uint8_t  *)(IPSBAR_BASE + (off)))
#define REG16(off)  (*(volatile uint16_t *)(IPSBAR_BASE + (off)))
#define REG32(off)  (*(volatile uint32_t *)(IPSBAR_BASE + (off)))

/* ==========================================================================
 * Core (MOVEC) registers - RM 3.2, 12.2.1, 18.3.2
 * ========================================================================== */
#define MOVEC_VBR           0x801
#define MOVEC_FLASHBAR      0xC04   /* RM 18.3.2: write-only via MOVEC          */
#define MOVEC_RAMBAR        0xC05   /* RM 12.2.1: write-only via MOVEC          */

/* Core RAMBAR (movec 0xC05) - RM 12.2.1, Fig 12-1 */
#define RAMBAR_BA(a)        ((uint32_t)(a) & 0xFFFF0000u)   /* 64K aligned    */
#define RAMBAR_PRIU         (1u << 11)  /* 1 = CPU priority in upper 32K bank  */
#define RAMBAR_PRIL         (1u << 10)  /* 1 = CPU priority in lower 32K bank  */
#define RAMBAR_SPV          (1u << 9)   /* secondary port valid: DMA/USB/FEC
                                           access. SCM RAMBAR[BDE] must ALSO
                                           be set (RM 12.2.1 note).           */
#define RAMBAR_WP           (1u << 8)
#define RAMBAR_CI           (1u << 5)
#define RAMBAR_SC           (1u << 4)
#define RAMBAR_SD           (1u << 3)
#define RAMBAR_UC           (1u << 2)
#define RAMBAR_UD           (1u << 1)
#define RAMBAR_V            (1u << 0)

/* FLASHBAR (movec 0xC04) - RM 18.3.2, Fig 18-3 / Table 18-2.
 * Reset: valid bit V set according to chip mode (single-chip mode => flash
 * valid at address 0); Fig 18-3 shows WP=1 and C/I=1 at reset, Table 3-1 lists
 * 0x0000_0000 (manual is inconsistent). In single-chip boot, flash is already
 * valid at 0, so nothing has to be written. WP is "always set": the core can
 * never write flash through the FLASHBAR mapping; program/erase writes must go
 * through the backdoor (CFM_BACKDOOR below). */
#define FLASHBAR_BA(a)      ((uint32_t)(a) & 0xFFF80000u)   /* 512K aligned   */
#define FLASHBAR_WP         (1u << 8)   /* read-only, always 1                 */
#define FLASHBAR_AFS        (1u << 6)   /* 1 = disable address-fetch speculation */
#define FLASHBAR_CI         (1u << 5)
#define FLASHBAR_SC         (1u << 4)
#define FLASHBAR_SD         (1u << 3)
#define FLASHBAR_UC         (1u << 2)
#define FLASHBAR_UD         (1u << 1)
#define FLASHBAR_V          (1u << 0)

/* ==========================================================================
 * System Control Module (SCM) - RM 14.4, Table 14-1
 * ========================================================================== */
#define SCM_IPSBAR          REG32(0x000000) /* 14.5.1, reset 0x4000_0001       */
#define SCM_RAMBAR          REG32(0x000008) /* 14.5.2, reset 0x0000_0000       */
#define SCM_RAMBAR_BA(a)    ((uint32_t)(a) & 0xFFFF0000u)
#define SCM_RAMBAR_BDE      (1u << 9)   /* back-door enable: non-core masters
                                           (USB DMA!) may access SRAM. Cleared
                                           at reset. Needs core RAMBAR[SPV]=1. */
#define SCM_CWCR            REG8 (0x000011) /* 14.5.4 core watchdog control
                                               (disabled at reset)             */
#define SCM_CWSR            REG8 (0x000013) /* 14.5.5 write 0x55 then 0xAA     */
#define SCM_MPR             REG8 (0x000020) /* 14.7.3.1 master privilege:
                                               bit1 = USB, bit0 = core; reset 0x03 */

/* ==========================================================================
 * Power management - RM chapter 10
 * PPMRx bit n = 1 disables the module clock (CDn); 0 = clock enabled.
 * Reset: PPMRH = 0x0000_0000. PPMRL reset is given inconsistently
 * (Table 10-1: 0x0040_0004, Table 14-1: 0x0000_0001, Table 7-4: 0x01,
 * Fig 10-1: all 0) - but none of those set bits is UART1/PIT/CFM, so every
 * module this bootloader needs is CLOCKED BY DEFAULT.
 * The USB module has NO PPMR bit (not listed in Tables 10-2/10-3; Fig 7-1
 * shows the USB clock gated only by USB_CTRL).
 * ========================================================================== */
#define PPMRH               REG32(0x00000C) /* 10.2.1 */
#define PPMRL               REG32(0x000018) /* 10.2.1 */
#define PPMRS               REG8 (0x000021) /* 10.2.3 write n (0..63) sets CDn  */
#define PPMRC               REG8 (0x000022) /* 10.2.4 write n (0..63) clears CDn*/
#define LPICR               REG8 (0x000012) /* 10.2.2 */
#define LPCR                REG8 (0x110007) /* 10.2.4.1 */

/* PPMRL bits (Table 10-2) */
#define PPMRL_CD1_GLOBAL    (1u << 1)   /* "global space" (footnote: registers) */
#define PPMRL_CD3_FLEXBUS   (1u << 3)
#define PPMRL_CD4_DMA       (1u << 4)
#define PPMRL_CD5_UART0     (1u << 5)
#define PPMRL_CD6_UART1     (1u << 6)
#define PPMRL_CD7_UART2     (1u << 7)
#define PPMRL_CD9_I2C0      (1u << 9)
#define PPMRL_CD10_QSPI     (1u << 10)
#define PPMRL_CD11_I2C1     (1u << 11)
#define PPMRL_CD13_DTIM0    (1u << 13)
#define PPMRL_CD14_DTIM1    (1u << 14)
#define PPMRL_CD15_DTIM2    (1u << 15)
#define PPMRL_CD16_DTIM3    (1u << 16)
#define PPMRL_CD17_INTC0    (1u << 17)
#define PPMRL_CD18_INTC1    (1u << 18)
#define PPMRL_CD21_FEC      (1u << 21)
/* PPMRH bits (Table 10-3); CDn with n >= 32 lives in PPMRH bit (n-32) */
#define PPMRH_CD32_GPIO     (1u << 0)
#define PPMRH_CD33_EPORT    (1u << 1)
#define PPMRH_CD35_PIT0     (1u << 3)
#define PPMRH_CD36_PIT1     (1u << 4)
#define PPMRH_CD39_ADC      (1u << 7)
#define PPMRH_CD40_GPT      (1u << 8)
#define PPMRH_CD41_PWM      (1u << 9)
#define PPMRH_CD42_FLEXCAN  (1u << 10)
#define PPMRH_CD43_CFM      (1u << 11)
#define PPMRH_CD46_RTC      (1u << 14)

/* ==========================================================================
 * Clock module - RM chapter 7, Table 7-4 (supervisor access only)
 * ========================================================================== */
#define SYNCR               REG16(0x120000) /* 7.7.1.1, reset 0x1002 (+PLLEN,
                                               CLKSRC = 1 when CLKMOD1 = 1)    */
#define SYNSR               REG8 (0x120002) /* 7.7.1.2, read-only              */
#define ROCR                REG16(0x120004) /* 7.7.1.3 relaxation osc trim     */
#define LPDR                REG8 (0x120007) /* 7.7.1.4, reset 0x00             */
#define CCHR                REG8 (0x120008) /* 7.7.1.5, reset 0x05 (/6)        */
#define CCLR                REG8 (0x120009) /* 7.7.1.6 */
#define OCHR                REG8 (0x12000A) /* 7.7.1.7 */
#define OCLR                REG8 (0x12000B) /* 7.7.1.8 */
#define RTCCR               REG8 (0x120012) /* 7.7.1.9 */
#define BWCR                REG8 (0x120013) /* 7.7.1.10 */

/* SYNCR bits (Fig 7-2, Table 7-5) */
#define SYNCR_LOLRE         (1u << 15)          /* loss-of-lock reset enable  */
#define SYNCR_MFD(x)        (((x) & 7u) << 12)  /* 0:4x 1:6x 2:8x ... 7:18x   */
#define SYNCR_MFD_MASK      (7u << 12)
#define SYNCR_LOCRE         (1u << 11)          /* loss-of-clock reset enable */
#define SYNCR_RFD(x)        (((x) & 7u) << 8)   /* divide by 2^RFD            */
#define SYNCR_RFD_MASK      (7u << 8)
#define SYNCR_LOCEN         (1u << 7)           /* loss-of-clock enable       */
#define SYNCR_DISCLK        (1u << 6)           /* 1 = CLKOUT held low        */
#define SYNCR_FWKUP         (1u << 5)           /* fast wakeup                */
                                                /* bits 4..3 reserved, write 0 */
#define SYNCR_CLKSRC        (1u << 2)           /* 1 = PLL output drives fsys */
#define SYNCR_PLLMODE       (1u << 1)           /* must be 1 on this device   */
#define SYNCR_PLLEN         (1u << 0)           /* PLL enable                 */

/* SYNSR bits (Fig 7-3, Table 7-6) */
#define SYNSR_EXTOSC        (1u << 7)
#define SYNSR_OCOSC         (1u << 6)
#define SYNSR_CRYOSC        (1u << 5)
#define SYNSR_LOCKS         (1u << 4)           /* sticky lock                */
#define SYNSR_LOCK          (1u << 3)           /* PLL locked                 */
#define SYNSR_LOCS          (1u << 2)           /* sticky loss of clock       */

/* LPDR (Fig 7-5): fsys divided by 2^LPD[3:0] */
#define LPDR_LPD(x)         ((x) & 0x0Fu)
/* CCHR (Fig 7-6): PLL reference = input / (CCHR[2:0] + 1); PLL ref range is
 * 2..10 MHz (Table 7-3 footnote 1). Changing CCHR makes the PLL lose lock. */
#define CCHR_DIV(n)         (((n) - 1u) & 7u)
/* CCLR (Fig 7-7) */
#define CCLR_OSCSEL1        (1u << 1)
#define CCLR_OSCSEL0        (1u << 0)
/* OCHR (Fig 7-8) */
#define OCHR_OCOEN          (1u << 7)
#define OCHR_STBY           (1u << 6)
/* OCLR (Fig 7-9) */
#define OCLR_OSCEN          (1u << 7)
#define OCLR_REFS           (1u << 6)   /* 1 = external crystal mode          */
#define OCLR_LPEN           (1u << 5)
#define OCLR_RANGE          (1u << 4)   /* 1 = 1..16 MHz crystal              */

/*
 * fsys = fref * 2 * (MFD + 2) / 2^RFD,  fref = fin / (CCHR + 1)   (Table 7-16)
 * "In normal PLL clock mode, the default system frequency is six times the
 * reference frequency after reset" (7.8.3).
 *
 * This board: 12 MHz crystal, CLKMOD[1:0] = 11 (PLL normal mode, crystal).
 *   After reset: CCHR = 5 (/6) -> fref = 2 MHz, MFD = 001 (6x), RFD = 0
 *                -> fsys = 12 MHz.  (Booting with PLL needs fin >= 12 MHz,
 *                Table 7-3 footnote 1; 12 MHz is exactly the edge.)
 *   Target:      CCHR = 1 (/2) -> fref = 6 MHz (inside 2..10 MHz)
 *                MFD = 010 (8x)  -> 48 MHz,  RFD = 000 (/1)
 *                fsys = 6 * 2 * (2 + 2) / 1 = 48 MHz.
 *
 * PLL change procedure, RM 7.8.3 (to absorb the overshoot when MFD changes):
 *   1. Choose MFD/RFD (max MFD for least jitter).
 *   2. Write RFD = (target RFD + 1) into SYNCR.
 *   3. Write the MFD value into SYNCR.
 *   4. Poll SYNSR[LOCK]; when locked, write the target RFD.
 * Also: LOLRE must be 0 before writing MFD (Table 7-5) - it is 0 at reset;
 * RFD should be written only while LOCK = 1 (Table 7-5); changing CCHR also
 * causes loss of lock (7.7.1.5).
 *
 * Suggested sequence for this board (values derived from the bit map above):
 *   SYNCR &= ~SYNCR_CLKSRC;          optional: run from PLL bypass clock
 *   CCHR   = CCHR_DIV(2);            0x01
 *   SYNCR  = SYNCR_MFD(2) | SYNCR_RFD(1) | SYNCR_PLLMODE | SYNCR_PLLEN
 *            (| SYNCR_CLKSRC if not bypassed)              0x2103 / 0x2107
 *   while (!(SYNSR & SYNSR_LOCK)) ;
 *   SYNCR  = SYNCR_MFD(2) | SYNCR_RFD(0) | SYNCR_CLKSRC | SYNCR_PLLMODE
 *            | SYNCR_PLLEN;                                 0x2007
 */
#define SYNCR_48MHZ_STEP    (SYNCR_MFD(2) | SYNCR_RFD(1) | SYNCR_PLLMODE | SYNCR_PLLEN)
#define SYNCR_48MHZ_FINAL   (SYNCR_MFD(2) | SYNCR_RFD(0) | SYNCR_CLKSRC | \
                             SYNCR_PLLMODE | SYNCR_PLLEN)
#define CCHR_48MHZ          0x01u

/* ==========================================================================
 * GPIO - RM chapter 15, Table 15-1
 * Port QS: PQS0..PQS3, PQS5, PQS6 (no PQS4).
 * ========================================================================== */
#define PORTQS              REG8 (0x10000C) /* 15.6.1 output data              */
#define DDRQS               REG8 (0x100024) /* 15.6.2 1 = output               */
#define PORTQSP             REG8 (0x10003C) /* 15.6.3 read: pin state          */
#define SETQS               REG8 (0x10003C) /* 15.6.3 write 1: set PORTQS bit  */
#define PINQS               PORTQSP         /* alias                           */
#define CLRQS               REG8 (0x100054) /* 15.6.4 write 0: clear PORTQS bit*/
#define PQSPAR              REG16(0x10006C) /* 15.6.5.2 quad-function, 16-bit  */
#define PNQPAR              REG16(0x100068) /* 15.6.5.3 */
#define PUAPAR              REG8 (0x100071) /* 15.6.5.2 */

/* Quad-function PnPAR field encoding (Table 15-7):
 *   00 GPIO, 01 primary, 10 alternate 1, 11 alternate 2 */
#define PAR_GPIO            0u
#define PAR_PRIMARY         1u
#define PAR_ALT1            2u
#define PAR_ALT2            3u

/* PQSPAR field positions (Fig 15-25): bits 15:14 and 9:8 reserved */
#define PQSPAR_PQS0(x)      (((x) & 3u) << 0)
#define PQSPAR_PQS1(x)      (((x) & 3u) << 2)
#define PQSPAR_PQS2(x)      (((x) & 3u) << 4)
#define PQSPAR_PQS3(x)      (((x) & 3u) << 6)
#define PQSPAR_PQS5(x)      (((x) & 3u) << 10)
#define PQSPAR_PQS6(x)      (((x) & 3u) << 12)

/* Pin functions (Table 2-1, 100 LQFP pins 19 / 18):
 *   PQS0: QSPI_DOUT (prim) / I2C_SCL1 (alt1) / UTXD1 (alt2) / PQS0 (GPIO)
 *   PQS1: QSPI_DIN  (prim) / I2C_SDA1 (alt1) / URXD1 (alt2) / PQS1 (GPIO)
 * => UART1 on QSPI pins: PQSPAR fields PQS0 = PQS1 = 11b (alt 2). */
#define PQSPAR_UART1_MASK   (PQSPAR_PQS0(3) | PQSPAR_PQS1(3))
#define PQSPAR_UART1_VAL    (PQSPAR_PQS0(PAR_ALT2) | PQSPAR_PQS1(PAR_ALT2))  /* 0x000F */

/* ==========================================================================
 * UART1 - RM chapter 28, Table 28-2. Base IPSBAR + 0x240.
 * "UART registers are accessible only as bytes." (28.3 note)
 * ========================================================================== */
#define UART1_BASE          0x000240u
#define UMR1_1              REG8 (UART1_BASE + 0x00) /* 28.3.1 */
#define UMR2_1              REG8 (UART1_BASE + 0x00) /* 28.3.2 same address */
/* Mode-register pointer: points to UMR1 after reset or after the RESET MODE
 * REGISTER POINTER command; any access to UMR1 moves it to UMR2; UMR2
 * accesses do not move it. UMR1/UMR2/UCSR must only be changed after the
 * receiver/transmitter got a software reset (Table 28-2 footnote 1). */
#define USR1                REG8 (UART1_BASE + 0x04) /* 28.3.3 read  */
#define UCSR1               REG8 (UART1_BASE + 0x04) /* 28.3.4 write */
#define UCR1                REG8 (UART1_BASE + 0x08) /* 28.3.5 write */
#define URB1                REG8 (UART1_BASE + 0x0C) /* 28.3.6 read  */
#define UTB1                REG8 (UART1_BASE + 0x0C) /* 28.3.7 write */
#define UIPCR1              REG8 (UART1_BASE + 0x10) /* 28.3.8 read  */
#define UACR1               REG8 (UART1_BASE + 0x10) /* 28.3.9 write */
#define UISR1               REG8 (UART1_BASE + 0x14) /* 28.3.10 read */
#define UIMR1               REG8 (UART1_BASE + 0x14) /* 28.3.10 write*/
#define UBG1_1              REG8 (UART1_BASE + 0x18) /* 28.3.11 divider MSB, W/O */
#define UBG2_1              REG8 (UART1_BASE + 0x1C) /* 28.3.11 divider LSB, W/O */
#define UIP1                REG8 (UART1_BASE + 0x34) /* 28.3.12 */
#define UOP1_1              REG8 (UART1_BASE + 0x38) /* 28.3.13 assert RTS  */
#define UOP0_1              REG8 (UART1_BASE + 0x3C) /* 28.3.13 negate RTS  */

/* UMR1 (Table 28-3) */
#define UMR1_RXRTS          (1u << 7)
#define UMR1_RXIRQ_FFULL    (1u << 6)
#define UMR1_ERR            (1u << 5)
#define UMR1_PM(x)          (((x) & 3u) << 3)   /* 00 with, 01 force, 10 none, 11 multidrop */
#define UMR1_PM_NONE        UMR1_PM(2)
#define UMR1_PT             (1u << 2)
#define UMR1_BC(x)          ((x) & 3u)          /* 00=5 .. 11=8 bits */
#define UMR1_BC_8           UMR1_BC(3)
/* UMR2 (Table 28-4) */
#define UMR2_CM(x)          (((x) & 3u) << 6)   /* 00 normal, 01 echo, 10 local loop, 11 remote loop */
#define UMR2_TXRTS          (1u << 5)
#define UMR2_TXCTS          (1u << 4)
#define UMR2_SB(x)          ((x) & 0x0Fu)
#define UMR2_SB_1           UMR2_SB(7)          /* 1.000 stop bit (6-8 bit chars) */
#define UMR2_SB_2           UMR2_SB(15)         /* 2.000 stop bits */
/* USR (Table 28-5) */
#define USR_RB              (1u << 7)
#define USR_FE              (1u << 6)
#define USR_PE              (1u << 5)
#define USR_OE              (1u << 4)
#define USR_TXEMP           (1u << 3)
#define USR_TXRDY           (1u << 2)
#define USR_FFULL           (1u << 1)
#define USR_RXRDY           (1u << 0)
/* UCSR (Table 28-6): 1101 = prescaled internal bus clock for RX and TX */
#define UCSR_RCS(x)         (((x) & 0xFu) << 4)
#define UCSR_TCS(x)         ((x) & 0xFu)
#define UCSR_BUSCLK         0xDDu
/* UCR (Table 28-7) - one MISC command per write; RESET TX + ENABLE TX
 * cannot be combined in one write. */
#define UCR_MISC_NONE       (0u << 4)
#define UCR_RESET_MR        (1u << 4)   /* reset mode-register pointer */
#define UCR_RESET_RX        (2u << 4)
#define UCR_RESET_TX        (3u << 4)
#define UCR_RESET_ERROR     (4u << 4)
#define UCR_RESET_BKCHGINT  (5u << 4)
#define UCR_START_BREAK     (6u << 4)
#define UCR_STOP_BREAK      (7u << 4)
#define UCR_TX_ENABLE       (1u << 2)
#define UCR_TX_DISABLE      (2u << 2)
#define UCR_RX_ENABLE       (1u << 0)
#define UCR_RX_DISABLE      (2u << 0)
/* UACR (Table 28-9) */
#define UACR_IEC            (1u << 0)
/* UISR / UIMR (Table 28-10) */
#define UIMR_COS            (1u << 7)
#define UIMR_DB             (1u << 2)
#define UIMR_FFULL_RXRDY    (1u << 1)
#define UIMR_TXRDY          (1u << 0)

/*
 * Baud rate (28.4.1.2.1, Eqn 28-1): Baud = fclk / (32 * Divider),
 * Divider = UBG1:UBG2 (16-bit, integer only - NO fractional divider on this
 * part). Minimum divider 0x0002; UBG2 reset value 0x00 is invalid and must
 * be written before enabling TX/RX (28.3.11 note).
 *
 * WHICH CLOCK: the RM says "prescaled internal bus clock (fsys)" (Table 28-6)
 * and uses fsys in Eqn 28-1 with an 80 MHz example (Eqn 28-2). Other modules
 * (PIT, FlexCAN, BWT) are documented as fsys/2. Pass the clock you measure:
 *   fsys = 48 MHz:  115200 -> div 13 (115385, +0.16 %)
 *   fsys/2 = 24 MHz: 115200 -> div 7 (107143, -7 %, unusable); 38400 -> 20 (37500)
 */
#define UART_DIVIDER(fclk, baud)  (((fclk) + 16u * (baud)) / (32u * (baud)))

/* ==========================================================================
 * USB OTG (USB-FS) - RM chapter 20, Table 20-4. Base IPSBAR + 0x1C_0000.
 * All registers are 8 bits wide, on 4-byte strides.
 * Sanity check after power-up: PER_ID == 0x04, ID_COMP == 0xFB (20.4.1.1/2).
 * ========================================================================== */
#define USB_BASE            0x1C0000u
#define USB_PER_ID          REG8 (USB_BASE + 0x000) /* 20.4.1.1  R  0x04 */
#define USB_ID_COMP         REG8 (USB_BASE + 0x004) /* 20.4.1.2  R  0xFB */
#define USB_REV             REG8 (USB_BASE + 0x008) /* 20.4.1.3  R  0x33 */
#define USB_ADD_INFO        REG8 (USB_BASE + 0x00C) /* 20.4.1.4  R  */
#define USB_OTG_INT_STAT    REG8 (USB_BASE + 0x010) /* 20.4.1.5  W1C */
#define USB_OTG_INT_EN      REG8 (USB_BASE + 0x014) /* 20.4.1.6  */
#define USB_OTG_STATUS      REG8 (USB_BASE + 0x018) /* 20.4.1.7  (OTG_STAT) */
#define USB_OTG_CTRL        REG8 (USB_BASE + 0x01C) /* 20.4.1.8  */
#define USB_INT_STAT        REG8 (USB_BASE + 0x080) /* 20.4.1.9  W1C */
#define USB_INT_ENB         REG8 (USB_BASE + 0x084) /* 20.4.1.10 */
#define USB_ERR_STAT        REG8 (USB_BASE + 0x088) /* 20.4.1.11 W1C */
#define USB_ERR_ENB         REG8 (USB_BASE + 0x08C) /* 20.4.1.12 */
#define USB_STAT            REG8 (USB_BASE + 0x090) /* 20.4.1.13 R  */
#define USB_CTL             REG8 (USB_BASE + 0x094) /* 20.4.1.14 */
#define USB_ADDR            REG8 (USB_BASE + 0x098) /* 20.4.1.15 */
#define USB_BDT_PAGE_01     REG8 (USB_BASE + 0x09C) /* 20.4.1.16 BDT addr[15:9] in bits 7:1 */
#define USB_FRM_NUML        REG8 (USB_BASE + 0x0A0) /* 20.4.1.17 R  */
#define USB_FRM_NUMH        REG8 (USB_BASE + 0x0A4) /* 20.4.1.17 R  */
#define USB_TOKEN           REG8 (USB_BASE + 0x0A8) /* 20.4.1.18 host only */
#define USB_SOF_THLD        REG8 (USB_BASE + 0x0AC) /* 20.4.1.19 host only */
#define USB_BDT_PAGE_02     REG8 (USB_BASE + 0x0B0) /* 20.4.1.20 BDT addr[23:16] */
#define USB_BDT_PAGE_03     REG8 (USB_BASE + 0x0B4) /* 20.4.1.21 BDT addr[31:24] */
#define USB_ENDPT(n)        REG8 (USB_BASE + 0x0C0 + 4u * (n)) /* 20.4.1.22, n=0..15 */
#define USB_ENDPT0          USB_ENDPT(0)
#define USB_CTRL            REG8 (USB_BASE + 0x100) /* 20.4.1.23 reset 0xC3 */
#define USB_OTG_OBSERVE     REG8 (USB_BASE + 0x104) /* 20.4.1.24 */
#define USB_OTG_CONTROL     REG8 (USB_BASE + 0x108) /* 20.4.1.25 */
/* There is no USBTRC0 or other register beyond 0x108 in Table 20-4. */

/* OTG_INT_STAT / OTG_INT_EN (Tables 20-15/20-16) - write 1 to clear status */
#define USB_OTGINT_ID_CHG           (1u << 7)
#define USB_OTGINT_1MSEC            (1u << 6)
#define USB_OTGINT_LINE_STATE_CHG   (1u << 5)
#define USB_OTGINT_SESS_VLD_CHG     (1u << 3)
#define USB_OTGINT_B_SESS_CHG       (1u << 2)
#define USB_OTGINT_A_VBUS_CHG       (1u << 0)
/* OTG_STATUS (Table 20-17) */
#define USB_OTGSTAT_ID              (1u << 7)
#define USB_OTGSTAT_LINE_STABLE     (1u << 5)
#define USB_OTGSTAT_SESS_VLD        (1u << 3)
#define USB_OTGSTAT_B_SESS_END      (1u << 2)
#define USB_OTGSTAT_A_VBUS_VLD      (1u << 0)
/* OTG_CTRL (Table 20-18) */
#define USB_OTGCTRL_DP_HIGH         (1u << 7)   /* D+ pull-up (when OTG_EN=1) */
#define USB_OTGCTRL_DP_LOW          (1u << 5)
#define USB_OTGCTRL_DM_LOW          (1u << 4)
#define USB_OTGCTRL_OTG_EN          (1u << 2)   /* 0: pull-ups follow CTL[USB_EN]
                                                   and HOST_MODE_EN            */

/* INT_STAT / INT_ENB (Tables 20-19/20-20) - INT_STAT is write-1-to-clear.
 * Reset value of INT_STAT shows SLEEP = 1 (Fig 20-15). */
#define USB_INT_STALL               (1u << 7)
#define USB_INT_ATTACH              (1u << 6)   /* host mode only */
#define USB_INT_RESUME              (1u << 5)
#define USB_INT_SLEEP               (1u << 4)   /* 3 ms idle */
#define USB_INT_TOK_DNE             (1u << 3)   /* clear AFTER reading STAT */
#define USB_INT_SOF_TOK             (1u << 2)
#define USB_INT_ERROR               (1u << 1)
#define USB_INT_USB_RST             (1u << 0)   /* -> ADDR = 0, ENDPT0 = 0x0D */

/* ERR_STAT / ERR_ENB (Tables 20-21/20-22) - ERR_STAT write-1-to-clear */
#define USB_ERR_BTS_ERR             (1u << 7)
#define USB_ERR_DMA_ERR             (1u << 5)
#define USB_ERR_BTO_ERR             (1u << 4)
#define USB_ERR_DFN8                (1u << 3)
#define USB_ERR_CRC16               (1u << 2)
#define USB_ERR_CRC5_EOF            (1u << 1)
#define USB_ERR_PID_ERR             (1u << 0)

/* STAT (Fig 20-19 / Table 20-23). ENDP is a 4-bit field in bits 7:4 (the
 * table text says "7-5" but also "four-bit field"; figure shows 7:4).
 * STAT is a 4-deep FIFO, advanced by clearing INT_STAT[TOK_DNE]. */
#define USB_STAT_ENDP(s)            (((s) >> 4) & 0x0Fu)
#define USB_STAT_TX                 (1u << 3)   /* 1 = transmit (IN, device mode) */
#define USB_STAT_ODD                (1u << 2)
/* STAT bits 7:2 == BDT index bits 8:3 => byte offset into BDT = (STAT & 0xFC) * 2 */
#define USB_STAT_BDT_INDEX(s)       (((s) >> 2) & 0x3Fu)

/* CTL (Fig 20-20 / Table 20-24); reset 0xC0 */
#define USB_CTL_JSTATE              (1u << 7)
#define USB_CTL_SE0                 (1u << 6)
#define USB_CTL_TXSUSPEND_TOKENBUSY (1u << 5)   /* set by SIE on SETUP; clear to
                                                   resume token processing     */
#define USB_CTL_RESET               (1u << 4)   /* host mode */
#define USB_CTL_HOST_MODE_EN        (1u << 3)
#define USB_CTL_RESUME              (1u << 2)
#define USB_CTL_ODD_RST             (1u << 1)
#define USB_CTL_USB_EN_SOF_EN       (1u << 0)

/* ADDR (Table 20-25) */
#define USB_ADDR_LS_EN              (1u << 7)
#define USB_ADDR_MASK               0x7Fu

/* TOKEN (host mode only). NOTE: Fig 20-25 shows TOKEN_PID in 7:4 and
 * TOKEN_ENDPT in 3:0, Table 20-29 says the opposite; not needed for device. */

/* ENDPTn (Fig 20-29 / Tables 20-33, 20-34) */
#define USB_ENDPT_HOST_WO_HUB       (1u << 7)   /* host, ENDPT0 only */
#define USB_ENDPT_RETRY_DIS         (1u << 6)   /* host, ENDPT0 only */
#define USB_ENDPT_EP_CTL_DIS        (1u << 4)   /* 1 = no SETUP */
#define USB_ENDPT_EP_RX_EN          (1u << 3)
#define USB_ENDPT_EP_TX_EN          (1u << 2)
#define USB_ENDPT_EP_STALL          (1u << 1)
#define USB_ENDPT_EP_HSHK           (1u << 0)
#define USB_ENDPT0_CONTROL          0x0Du       /* RX|TX|HSHK, SETUP allowed (20.4.1.22) */

/* USB_CTRL (Fig 20-30 / Table 20-35); reset 0xC3 = SUSP | PDE | CLK_SRC=11.
 * SUSP and PDE must be cleared to use the transceiver. */
#define USB_CTRL_SUSP               (1u << 7)   /* transceiver suspended */
#define USB_CTRL_PDE                (1u << 6)   /* weak pulldowns on D+/D- */
#define USB_CTRL_CLK_SRC(x)         ((x) & 3u)
#define USB_CTRL_CLK_SRC_ALTCLK     0u  /* USB_ALT_CLK pin (must be 48 MHz, Table 2-9) */
#define USB_CTRL_CLK_SRC_EXTAL      1u  /* external osc on EXTAL */
#define USB_CTRL_CLK_SRC_SYSCLK     3u  /* "System clock source" -> needs fsys = 48 MHz */
/* There is no separate USB divider from the PLL: with CLK_SRC = 11 the USB
 * "48 MHz clock" is the system clock itself, so fsys must be exactly 48 MHz. */

/* USB_OTG_OBSERVE (Table 20-36), read-only observation */
#define USB_OTGOBS_DP_PU            (1u << 7)
#define USB_OTGOBS_DP_PD            (1u << 6)
#define USB_OTGOBS_DM_PD            (1u << 4)

/* USB_OTG_CONTROL (Table 20-37) */
#define USB_OTGCON_DPPULLUP_NONOTG  (1u << 4)   /* D+ pull-up in non-OTG device mode */
#define USB_OTGCON_ID               (1u << 3)
#define USB_OTGCON_VBUSVLD          (1u << 2)
#define USB_OTGCON_SESSVLD          (1u << 1)
#define USB_OTGCON_SESSEND          (1u << 0)

/*
 * Buffer Descriptor Table - RM 20.3
 *  - BDT must be on a 512-byte boundary (20.3.1, 20.3.3, Table 20-26);
 *    BDT_PAGE_01 holds address bits 15:9 in its bits 7:1, PAGE_02 bits 23:16,
 *    PAGE_03 bits 31:24.
 *  - Each BD is 8 bytes; 4 BDs per endpoint (RX even, RX odd, TX even, TX odd);
 *    16 bidirectional EPs = 512 bytes.
 *  - BD address = BDT_BASE | ENDPOINT[3:0] << 5 | TX << 4 | ODD << 3  (Fig 20-4).
 *    TX = 1 for transmit; in device mode TX = IN, RX = OUT/SETUP (Table 20-1).
 *  - BD word 0 (Fig 20-5 / Table 20-3), as 32-bit value:
 *      31:26 reserved, 25:16 BC[9:0] byte count, 15:8 reserved,
 *      7 OWN, 6 DATA0/1, 5 KEEP/TOK_PID[3], 4 NINC/TOK_PID[2],
 *      3 DTS/TOK_PID[1], 2 BDT_STALL/TOK_PID[0], 1:0 reserved (0)
 *    On completion (KEEP=0) the SIE writes TOK_PID into bits 5:2:
 *      0x1 OUT, 0x9 IN, 0xD SETUP.
 *    The OWN byte must be the last thing the CPU writes when arming a BD.
 *  - BD word 1: 32-bit buffer address.
 *  - BDT and buffers must be in SRAM reachable by the USB master: core
 *    RAMBAR[SPV]=1 AND SCM RAMBAR[BDE]=1 (12.2.1, 14.5.2).
 *
 * ENDIANNESS: NOT IN RM. The manual never states the byte order the USB DMA
 * uses when it reads/writes the BD words (no "endian" anywhere in the RM).
 * Freescale's own USB stacks for MCF5225x byte-swap the BD control word and
 * buffer address, i.e. the BD is LITTLE-ENDIAN while the ColdFire core is
 * big-endian. With little-endian BDs, byte 0 of the BD (lowest address) holds
 * bits 7:0 (OWN, DATA01, ...), byte 2 holds BC[7:0], byte 3 holds BC[9:8].
 * Verify on hardware: after the first SETUP, BC must read as 8.
 * Set USB_BDT_LITTLE_ENDIAN to 0 if your test shows otherwise.
 */
#ifndef USB_BDT_LITTLE_ENDIAN
#define USB_BDT_LITTLE_ENDIAN 1
#endif

#define USB_BD_OWN                  (1u << 7)
#define USB_BD_DATA1                (1u << 6)
#define USB_BD_KEEP                 (1u << 5)
#define USB_BD_NINC                 (1u << 4)
#define USB_BD_DTS                  (1u << 3)
#define USB_BD_BDT_STALL            (1u << 2)
#define USB_BD_TOK_PID(ctl)         (((ctl) >> 2) & 0x0Fu)
#define USB_BD_BC(n)                (((uint32_t)(n) & 0x3FFu) << 16)
#define USB_BD_GET_BC(ctl)          (((ctl) >> 16) & 0x3FFu)
#define USB_PID_OUT                 0x1u
#define USB_PID_IN                  0x9u
#define USB_PID_SETUP               0xDu

typedef struct {
    volatile uint32_t ctl;      /* in USB (bus) byte order, see USB_BD_TO_CPU */
    volatile uint32_t addr;     /* in USB (bus) byte order                     */
} usb_bd_t;

#if USB_BDT_LITTLE_ENDIAN
#define USB_BD_SWAP32(x)  ((((uint32_t)(x) & 0x000000FFu) << 24) | \
                           (((uint32_t)(x) & 0x0000FF00u) <<  8) | \
                           (((uint32_t)(x) & 0x00FF0000u) >>  8) | \
                           (((uint32_t)(x) & 0xFF000000u) >> 24))
#else
#define USB_BD_SWAP32(x)  ((uint32_t)(x))
#endif
#define USB_BD_TO_CPU(x)    USB_BD_SWAP32(x)
#define USB_BD_FROM_CPU(x)  USB_BD_SWAP32(x)

#define USB_BDT_INDEX(ep, tx, odd)  (((ep) << 2) | ((tx) << 1) | (odd))
#define USB_BDT_ALIGN               512

/* ==========================================================================
 * ColdFire Flash Module (CFM) - RM chapter 18, Table 18-3
 * ========================================================================== */
#define CFMMCR              REG16(0x1D0000) /* 18.3.3.1 */
#define CFMCLKD             REG8 (0x1D0002) /* 18.3.3.2 bits 6:0 write-once */
#define CFMSEC              REG32(0x1D0008) /* 18.3.3.3 read-only, from flash 0x414 */
#define CFMPROT             REG32(0x1D0010) /* 18.3.3.4 writable when LOCK = 0 */
#define CFMSACC             REG32(0x1D0014) /* 18.3.3.5 */
#define CFMDACC             REG32(0x1D0018) /* 18.3.3.6 */
#define CFMUSTAT            REG8 (0x1D0020) /* 18.3.3.7 */
#define CFMCMD              REG8 (0x1D0024) /* 18.3.3.8 */
#define CFMCLKSEL           REG16(0x1D004A) /* 18.3.3.9 read-only, factory */

/* Flash backdoor: program/erase writes (and non-core reads) must use
 * IPSBAR + 0x0400_0000 + flash offset (18.3.2 note; Table 1-3: 0x4400_0000). */
#define CFM_BACKDOOR        (IPSBAR_BASE + 0x04000000u)
#define CFM_FLASH_SIZE      (512u * 1024u)
#define CFM_SECTOR_SIZE     (16u * 1024u)  /* CFMPROT/SACC/DACC bit M = 16 KB sector M */
#define CFM_PAGE_SIZE       4096u          /* "4-kByte logical pages" (18.1.1).
                                              EzPort SE (23.4.1.8) says 2 KB! */
#define CFM_CONFIG_FIELD    0x400u         /* 0x400..0x417, Table 18-1 */

/* CFMMCR (Table 18-4) */
#define CFMMCR_LOCK         (1u << 10)
#define CFMMCR_PVIE         (1u << 9)
#define CFMMCR_AEIE         (1u << 8)
#define CFMMCR_CBEIE        (1u << 7)
#define CFMMCR_CCIE         (1u << 6)
#define CFMMCR_KEYACC       (1u << 5)
/* CFMCLKD (Table 18-5) */
#define CFMCLKD_DIVLD       (1u << 7)       /* read-only: CFMCLKD was written */
#define CFMCLKD_PRDIV8      (1u << 6)       /* required when input > 12.8 MHz */
#define CFMCLKD_DIV(x)      ((x) & 0x3Fu)
/*
 * 18.4.2.3.1: FCLK = in / (PRDIV8 ? 8 : 1) / (DIV + 1), must be 150..200 kHz.
 * DIV = FCLK_kHz/200 - 1 if that division is exact, else INT(FCLK_kHz/200).
 * "in" = internal (flash) bus clock. Fig 7-1 draws the CFM on the fsys/2
 * branch => with fsys = 48 MHz: in = 24 MHz, /8 = 3 MHz, DIV = 14 -> 200 kHz.
 * (If the CFM were clocked at full fsys this would give 400 kHz = too fast,
 * risking incomplete programming, but not overstress. The opposite mistake,
 * DIV = 29 at 24 MHz -> 100 kHz, "can destroy the flash memory".)
 */
#define CFMCLKD_48MHZ       (CFMCLKD_PRDIV8 | CFMCLKD_DIV(14))  /* 0x4E */
/* CFMSEC (Table 18-6, 18-7) */
#define CFMSEC_KEYEN        (1u << 31)
#define CFMSEC_SECSTAT      (1u << 30)
#define CFMSEC_SEC_MASK     0xFFFFu
#define CFMSEC_SECURED      0x4AC8u
/* CFMUSTAT (Table 18-11). CBEIF/PVIOL/ACCERR/BLANK are write-1-to-clear,
 * CCIF read-only. "Only one CFMUSTAT register bit can be cleared at a time." */
#define CFMUSTAT_CBEIF      (1u << 7)
#define CFMUSTAT_CCIF       (1u << 6)
#define CFMUSTAT_PVIOL      (1u << 5)
#define CFMUSTAT_ACCERR     (1u << 4)
#define CFMUSTAT_BLANK      (1u << 2)
/* CFMCMD (Table 18-13 / 18-16) */
#define CFMCMD_BLANK_CHECK  0x05u
#define CFMCMD_PAGE_ERASE_VERIFY 0x06u
#define CFMCMD_WORD_PROGRAM 0x20u
#define CFMCMD_PAGE_ERASE   0x40u
#define CFMCMD_MASS_ERASE   0x41u
/* CFMCLKSEL (Table 18-15): CLKSEL = 10b single-cycle, else two-cycle */
#define CFMCLKSEL_CLKSEL(r) ((r) & 3u)

/*
 * Command write sequence (18.4.2.3.2, Figs 18-16/18-17):
 *   0. Once after reset: CFMCLKD = CFMCLKD_48MHZ (no command runs while
 *      DIVLD = 0; writing flash before that sets ACCERR).
 *   1. Wait CFMUSTAT[CBEIF] = 1. If ACCERR or PVIOL set, clear them
 *      (write 1, one bit per write).
 *   2. Write ONE 32-bit word to the flash through the backdoor:
 *        *(volatile uint32_t *)(CFM_BACKDOOR + offset) = data;
 *      - word program: offset = target, data = value (longword only; byte /
 *        word writes give a transfer error, 18.4.2.2). Optionally a second
 *        word to offset+4 if offset is a multiple of 8 (even+odd block).
 *      - page erase: any word in the page, data ignored.
 *   3. CFMCMD = CFMCMD_WORD_PROGRAM or CFMCMD_PAGE_ERASE.
 *   4. CFMUSTAT = CFMUSTAT_CBEIF;  (launch)
 *   5. Poll CFMUSTAT[CCIF] = 1, then check PVIOL / ACCERR.
 *   No other CFM register may be written between steps 2 and 4.
 *
 * Executing from flash: "It is not possible to read from any flash logical
 * block while the same logical block is being erased, programmed, or
 * verified" (18.1.1); a read during an active command "returns invalid data"
 * (18.4.2.3.5). The whole 512 KB is one logical block (Fig 18-2), so the
 * command launch + CCIF poll loop (and any ISR/vector fetched while CCIF = 0)
 * must run from SRAM. Do not execute STOP during program/erase (18.4.2.4).
 */

#endif /* MCF52254_H */
