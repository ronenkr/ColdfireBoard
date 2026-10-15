/* Services the USB bootloader offers to applications (include from the app).
 *
 * The bootloader leaves the PLL at fsys = 48 MHz (USB needs it) and its USB
 * state in RAM 0x2000E000-0x2000FFFF, so applications must link their RAM
 * below 0x2000E000 and keep fsys at 48 MHz while using USB.
 *
 *   const struct cfboot_svc *b = CFBOOT_SVC;
 *   if (b->magic == CFBOOT_SVC_MAGIC) { b->usb_init(); ... b->usb_poll(); }
 *
 * Call usb_poll() at least every few ms. It also handles the host's
 * "1200-baud touch", which reboots into the bootloader for a new upload.
 */
#ifndef CFBOOT_H
#define CFBOOT_H
#include <stdint.h>

#define CFBOOT_SVC_MAGIC 0x43464253u     /* 'CFBS' */

struct cfboot_svc {
    uint32_t magic;
    uint32_t version;
    void (*usb_init)(void);              /* (re)enumerate as the app's CDC port */
    void (*usb_poll)(void);
    int  (*read)(uint8_t *buf, unsigned max);        /* non-blocking */
    void (*write)(const uint8_t *buf, unsigned len); /* drops data if no host */
    int  (*connected)(void);             /* host has the port open */
    void (*reboot)(int stay_in_bootloader);
};

#define CFBOOT_SVC ((const struct cfboot_svc *)0x00000418)

#endif
