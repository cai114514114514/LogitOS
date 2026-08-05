#include <stdint.h>
#include "serial.h"
#include "io.h"

#define COM1 0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* disable interrupts */
    outb(COM1 + 3, 0x80);   /* enable DLAB (set baud divisor) */
    outb(COM1 + 0, 0x03);   /* divisor low  = 3 -> 38400 baud */
    outb(COM1 + 1, 0x00);   /* divisor high = 0 */
    outb(COM1 + 3, 0x03);   /* 8 bits, no parity, one stop bit */
    outb(COM1 + 2, 0xC7);   /* enable + clear FIFO, 14-byte threshold */
    outb(COM1 + 4, 0x0B);   /* RTS/DSR set, OUT2 (needed for IRQs) */
}

static int tx_ready(void)
{
    return inb(COM1 + 5) & 0x20;  /* line status: transmitter holding empty */
}

void serial_putc(char c)
{
    /* Bounded wait: a wedged UART (or no serial device at all) must not hang
     * the caller forever -- drop the byte after a generous timeout. */
    for (long spins = 0; spins < 1000000; spins++) {
        if (tx_ready()) {
            outb(COM1, (uint8_t)c);
            return;
        }
    }
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

/* Non-blocking receive: return the next byte from COM1, or -1 if none waiting.
 * The serial console (F_TTY) polls this; there is no serial RX IRQ. */
int serial_getc(void)
{
    if (inb(COM1 + 5) & 0x01)        /* line status: data ready */
        return inb(COM1);
    return -1;
}
