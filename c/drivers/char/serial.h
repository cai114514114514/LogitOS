#ifndef LOGIT_SERIAL_H
#define LOGIT_SERIAL_H

#include <stdint.h>

/* COM1 serial driver — primary debug/log channel and test transport.
 *
 * TRANSMIT IS QUEUED, NOT WAITED FOR. serial_putc() appends to a RAM ring and
 * writes to the UART only while the transmitter says it has room; it never
 * spins on the wire. A byte that does not fit the ring is dropped and counted. */

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
int  serial_getc(void);   /* next RX byte, or -1 if none */

/* Called only after panic stops peer CPUs; never waits for a failed owner.
 * From here on serial_putc() drains the ring synchronously (bounded wait). */
void serial_panic_takeover(void);

/* Opportunistic periodic drain for bytes queued while the 16-byte FIFO was
 * still busy. Interrupt-safe: it only try-locks the ring, writes at most one
 * FIFO's worth and never waits, so any timer tick may call it. */
void serial_tx_tick(void);

/* Push every queued byte onto the wire and wait for the shifter to go idle,
 * with a bounded wait. For the moments the machine is about to stop being able
 * to transmit (power-off, reset): the only place a wait is acceptable. */
void serial_flush_blocking(void);

/* Counters for the HWHUD "con" line and the host gate. All are cumulative
 * except ring_used; they are read without the ring lock and so are a
 * best-effort snapshot, not a consistent cut. */
struct serial_tx_stats {
    uint64_t queued;        /* bytes accepted into the ring */
    uint64_t sent;          /* bytes written to the UART transmit register */
    uint64_t dropped;       /* bytes refused because the ring was full */
    unsigned ring_used;     /* bytes queued and not yet sent */
    unsigned ring_size;     /* capacity of the ring in bytes */
};
void serial_tx_stats(struct serial_tx_stats *out);
uint64_t serial_dropped(void);

#endif /* LOGIT_SERIAL_H */
