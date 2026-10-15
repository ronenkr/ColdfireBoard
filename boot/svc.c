/* Bootloader service table at a fixed flash address (0x418) for applications. */
#include "boot.h"
#include "cfboot.h"

__attribute__((section(".svc"), used))
const struct cfboot_svc cfboot_svc = {
    .magic     = CFBOOT_SVC_MAGIC,
    .version   = 1,
    .usb_init  = usb_init,
    .usb_poll  = usb_poll,
    .read      = cdc_read,
    .write     = cdc_write,
    .connected = cdc_connected,
    .reboot    = reboot,
};
