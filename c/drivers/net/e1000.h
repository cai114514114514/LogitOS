#ifndef LOGIT_E1000_H
#define LOGIT_E1000_H

#include <stdint.h>
#include "netdev.h"

/* COMPATIBILITY FACADE -- these are no longer the Intel driver.
 *
 * The e1000_* names below are what `c/net/link/eth.c`, `c/net/core/net.c`,
 * `c/kernel/cpu/interrupts.c` and `c/kernel/cpu/smp.c` call, from when the OS
 * had exactly one NIC driver. They are now thin forwards (defined in netdev.c)
 * to whichever card the registry bound -- virtio-net, e1000, rtl8139, rtl8169.
 * Calling e1000_tx() on a machine with a Realtek card transmits on the Realtek
 * card.
 *
 * New code should include "netdev.h" and call netdev_*; see the rename note at
 * the bottom of netdev.c for what it takes to retire this header.
 */

/* Intel 82540EM (QEMU "e1000"). Kept because the driver still names them; the
 * full match table lives in net_ids.inc. */
#define E1000_VENDOR 0x8086
#define E1000_DEVICE 0x100E

int  e1000_init(void);                       /* -> netdev_init()       */
const uint8_t *e1000_mac(void);              /* -> netdev_mac()        */
int  e1000_tx(const void *frame, uint16_t len);           /* -> netdev_tx()        */
int  e1000_rx_poll(void (*cb)(const uint8_t *frame, uint16_t len)); /* -> netdev_rx_poll() */
void e1000_irq_enable(void (*cb)(const uint8_t *frame, uint16_t len)); /* -> netdev_irq_enable() */
int  e1000_irq_line(void);                   /* -> netdev_irq_line()   */
void e1000_irq(void);                        /* -> netdev_irq()        */

#endif /* LOGIT_E1000_H */
