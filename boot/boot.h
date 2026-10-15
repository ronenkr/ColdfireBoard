/* ColdFire USB bootloader: shared definitions. */
#ifndef BOOT_H
#define BOOT_H
#include <stdint.h>

#define APP_BASE      0x00004000u
#define FLASH_END     0x00080000u      /* 512 KB */
#define FLASH_PAGE    4096u            /* CFM logical page (erase unit) */
#define MAX_XFER      1024u            /* largest W/R payload */
#define BOOT_MAGIC    0xB0071DADu      /* in .noinit RAM: stay in bootloader */

/* USB SETUP packet, fields already converted to host (big-endian) order */
typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} setup_t;

/* uart.c */
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex(uint32_t v, int digits);
/* non-blocking debug log: buffered, drained one char per dbg_flush() */
void dbg_puts(const char *s);
void dbg_hex(uint32_t v);
void dbg_flush(void);
void dbg_drain(void);

/* timing (main.c): free-running millisecond counter, call often */
uint32_t millis(void);

/* flash.c: run from SRAM; return 0 or a negative error */
void flash_init(void);
int  flash_erase_page(uint32_t addr);
int  flash_program(uint32_t addr, const uint32_t *words, unsigned nwords);

/* usb.c */
void usb_init(void);
void usb_poll(void);
void usb_stop(void);

/* cdc.c side of usb.c: byte streams over the CDC-ACM bulk endpoints */
int  cdc_read(uint8_t *buf, unsigned max);        /* non-blocking, returns count */
void cdc_write(const uint8_t *buf, unsigned len); /* blocks (polls USB) until queued */
int  cdc_connected(void);                         /* host opened the port (DTR) */
int  usb_configured(void);

/* set by usb.c when the host sets 1200 baud and drops DTR ("1200-baud touch") */
extern volatile uint8_t usb_reboot_request;

/* main.c */
void reboot(int stay_in_bootloader);

void jump_to_app(uint32_t base);

#endif
