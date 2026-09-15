#include <stdint.h>
#include "serial.h"
#include "io.h"
#include "io_lock.h"
#include "kprintf.h"

/* Serialize only status+register access. The UART wait runs with the leaf
 * unlocked and IRQ state restored, so a slow console cannot exclude unrelated
 * work for an entire line. Panic has already stopped peers before takeover. */
static io_lock_t tx_lock = IO_LOCK_INIT, rx_lock = IO_LOCK_INIT;
static unsigned serial_panicking;
void serial_panic_takeover(void)
{ __atomic_store_n(&serial_panicking, 1, __ATOMIC_RELEASE); }

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
        int panic = __atomic_load_n(&serial_panicking, __ATOMIC_ACQUIRE);
        uint64_t flags = panic ? 0 : io_lock_enter(&tx_lock);
        int ready = tx_ready();
        if (ready) outb(COM1, (uint8_t)c);
        if (!panic) io_lock_leave(&tx_lock, flags);
        if (ready) return;
    }
}

void serial_puts(const char *s)
{
    uint64_t flags = kprintf_console_enter();
    while (*s)
        serial_putc(*s++);
    kprintf_console_leave(flags);
}

/* Non-blocking receive: return the next byte from COM1, or -1 if none waiting.
 * The serial console (F_TTY) polls this; there is no serial RX IRQ. */
int serial_getc(void)
{
    uint64_t flags = io_lock_enter(&rx_lock);
    int value = (inb(COM1 + 5) & 0x01) ? inb(COM1) : -1;
    io_lock_leave(&rx_lock, flags);
    return value;
}
