#ifndef LOGIT_E1000_H
#define LOGIT_E1000_H

#include <stdint.h>

/* Intel 82540EM (QEMU "e1000"). PCI vendor:device. */
#define E1000_VENDOR 0x8086
#define E1000_DEVICE 0x100E

/* Bring up the NIC: locate it on PCI, map MMIO, reset, set up RX/TX rings.
 * Returns 0 on success, -1 if no NIC was found. */
int e1000_init(void);

/* Our MAC address (valid after a successful e1000_init). */
const uint8_t *e1000_mac(void);

/* Transmit one raw Ethernet frame (already including the 14-byte header).
 * Returns 0 on success, -1 on error. */
int e1000_tx(const void *frame, uint16_t len);

/* Poll the RX ring; for each received frame call cb(frame, len). Returns the
 * number of frames delivered this call. */
int e1000_rx_poll(void (*cb)(const uint8_t *frame, uint16_t len));

/* Interrupt-driven RX: register the handler + unmask RX causes; query the PCI
 * IRQ line; service an interrupt (ack + drain). Polling stays as a backstop. */
void e1000_irq_enable(void (*cb)(const uint8_t *frame, uint16_t len));
int  e1000_irq_line(void);
void e1000_irq(void);

#endif /* LOGIT_E1000_H */
