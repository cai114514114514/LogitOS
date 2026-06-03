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
    while (!tx_ready())
        ;
    outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}
