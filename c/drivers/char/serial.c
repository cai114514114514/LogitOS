#include <stdint.h>
#include "../../kernel/sched/observe/flight_recorder.h"
#include "serial.h"
#include "io.h"
#include "io_lock.h"
#include "kprintf.h"

/* NS16550A register map, offsets from the COM1 base (TI TL16C550C datasheet
 * SLLS177, table 2; Linux include/uapi/linux/serial_reg.h). */
#define COM1 0x3F8
#define UART_THR (COM1 + 0)     /* transmit holding register (write) */
#define UART_RBR (COM1 + 0)     /* receiver buffer register (read) */
#define UART_DLL (COM1 + 0)     /* divisor latch low (DLAB=1) */
#define UART_IER (COM1 + 1)     /* interrupt enable */
#define UART_DLM (COM1 + 1)     /* divisor latch high (DLAB=1) */
#define UART_IIR (COM1 + 2)     /* interrupt identification (read) */
#define UART_FCR (COM1 + 2)     /* FIFO control (write) */
#define UART_LCR (COM1 + 3)     /* line control */
#define UART_MCR (COM1 + 4)     /* modem control */
#define UART_LSR (COM1 + 5)     /* line status */
#define LSR_DATA_READY 0x01     /* receiver has a byte */
#define LSR_THRE       0x20     /* transmit holding / FIFO empty */
#define LSR_TEMT       0x40     /* holding register AND shifter empty */
#define IIR_FIFO_USABLE 0xC0    /* IIR[7:6]=11: 16550A FIFOs enabled and working */

/* 115200 / divisor = baud (the 1.8432 MHz UART clock divided by 16 and by the
 * divisor). Divisor 1 is 115200 baud: 8N1 is 10 bit times, so ~87 us a byte.
 * The old divisor 3 (38400 baud, ~260 us a byte) is what made the real X79-VG4
 * desktop crawl, because every byte was waited for with interrupts off. */
#define SERIAL_BAUD_DIVISOR 1

/* The transmit ring. kprintf, the syscall console and the panic reporters all
 * funnel bytes through serial_putc(); the ring lets them hand the bytes over
 * and carry on while the 16550 clocks them out at wire speed. 64 KiB (a power
 * of two so an index wraps with a mask) holds a whole verbose boot -- a QEMU
 * boot to desktop is ~16 KB, real hardware with ACPI/PCI chatter is more --
 * because bytes that overflow it are lost, not waited for. */
#define SERIAL_TX_RING_SIZE 65536u
#define SERIAL_TX_RING_MASK (SERIAL_TX_RING_SIZE - 1u)

/* The 16550A transmit FIFO is 16 bytes (SLLS177 section 1: "16-byte FIFOs").
 * In FIFO mode THRE (LSR bit 5) is set only when the transmit FIFO is EMPTY
 * (SLLS177 table 4, LSR bit 5), so one THRE reading is permission to write a
 * whole FIFO without reading the status again. Linux 8250 relies on the same
 * rule: serial8250_tx_chars() writes up to tx_loadsz bytes per THRE. */
#define SERIAL_FIFO_DEPTH 16u

/* How many status polls one blocking drain makes before it decides the UART is
 * wedged or absent. The same generous bound the old per-byte wait used. */
#define SERIAL_WAIT_SPINS 1000000u

/* Room required before the "bytes were dropped" notice is queued, so the notice
 * can never itself be dropped: it is at most 11 + 20 digits + 44 characters. */
#define SERIAL_DROP_NOTICE_ROOM 96u

/* Sentinel returned by tx_lock_enter() when the ring lock was NOT taken
 * because the machine is panicking. RFLAGS never has all 64 bits set. */
#define TX_LOCK_NOT_TAKEN UINT64_MAX

/* tx_lock guards the ring indices, the counters and the UART's THR/LSR pair:
 * checking "ready" and writing the byte must be one transaction or two CPUs
 * overrun the FIFO. It is a leaf held for microseconds (formatting no longer
 * happens under it, and nothing under it waits). rx_lock is the receive side.
 * Panic has already stopped peers before takeover, so the panic path skips the
 * lock rather than wait for an owner that is never coming back. */
static io_lock_t tx_lock = IO_LOCK_INIT, rx_lock = IO_LOCK_INIT;
static unsigned serial_panicking;

/* Panic calls this once peers are stopped: from now on the ring lock is never
 * taken and serial_putc() drains synchronously, so the report reaches the wire
 * even if the dead owner of tx_lock never releases it. */
void serial_panic_takeover(void)
{
    __atomic_store_n(&serial_panicking, 1, __ATOMIC_RELEASE);
}

/* Free-running producer and consumer indices (they wrap at 2^32, and a slot is
 * index & MASK), so head - tail is the fill level with no ambiguity between
 * "full" and "empty". Written only with tx_lock held (or while panicking);
 * serial_tx_tick() reads them lock-free as a cheap "anything to do?" test. */
static char tx_ring[SERIAL_TX_RING_SIZE];
static unsigned tx_head;
static unsigned tx_tail;

/* Bytes one THRE reading may write: the FIFO depth once serial_init() has
 * proven a 16550A, otherwise 1 so a 16450 (single holding register) or a
 * UART that never set up is never overrun. */
static unsigned tx_burst = 1;

static uint64_t tx_queued;
static uint64_t tx_sent;
static uint64_t tx_dropped;
static uint64_t tx_dropped_reported;

/* Set when a blocking drain gave up on the UART, so the next blocking drain
 * makes one poll instead of burning SERIAL_WAIT_SPINS again for every byte of
 * a panic report on a machine with no working serial port. */
static unsigned tx_wedged;

/* Program the line (115200 8N1, FIFOs on) and learn whether the FIFO can be
 * burst-filled. Idempotent: several early boot paths call it before kmain
 * does. The ring and its queued bytes are deliberately left alone. */
void serial_init(void)
{
    outb(UART_IER, 0x00);   /* disable interrupts */
    outb(UART_LCR, 0x80);   /* enable DLAB (set baud divisor) */
    outb(UART_DLL, SERIAL_BAUD_DIVISOR & 0xFF);     /* divisor low  -> 115200 baud */
    outb(UART_DLM, SERIAL_BAUD_DIVISOR >> 8);       /* divisor high */
    outb(UART_LCR, 0x03);   /* 8 bits, no parity, one stop bit */
    outb(UART_FCR, 0xC7);   /* enable + clear FIFO, 14-byte threshold */
    outb(UART_MCR, 0x0B);   /* RTS/DSR set, OUT2 (needed for IRQs) */

    /* Linux 8250 autoconfig (drivers/tty/serial/8250/8250_port.c): after
     * FCR enable, IIR[7:6]=11 identifies a 16550A whose FIFOs work; 10 is the
     * original 16550 with a broken FIFO; 00 has no FIFO at all. Only the first
     * may be filled 16 bytes at a time. */
    uint8_t identification = inb(UART_IIR);
    tx_burst = (identification & IIR_FIFO_USABLE) == IIR_FIFO_USABLE
        ? SERIAL_FIFO_DEPTH : 1;
}

/* Line status: transmitter holding register / FIFO empty. */
static int tx_ready(void)
{
    return inb(UART_LSR) & LSR_THRE;
}

/* Bytes queued and not yet handed to the UART. */
static unsigned tx_used(void)
{
    return tx_head - tx_tail;
}

/* "Is there anything to send?" without the subtraction. */
static int tx_pending(void)
{
    return tx_head != tx_tail;
}

/* Take the ring lock, except while panicking (see tx_lock). The returned value
 * is the saved IRQ state, or TX_LOCK_NOT_TAKEN, which callers also use to pick
 * the blocking drain: a panic report must reach the wire before the halt. */
static uint64_t tx_lock_enter(void)
{
    if (__atomic_load_n(&serial_panicking, __ATOMIC_ACQUIRE))
        return TX_LOCK_NOT_TAKEN;
    return io_lock_enter(&tx_lock);
}

/* Counterpart of tx_lock_enter(): releases only what it actually took. */
static void tx_lock_leave(uint64_t flags)
{
    if (flags != TX_LOCK_NOT_TAKEN)
        io_lock_leave(&tx_lock, flags);
}

/* Append one byte, or drop it and count it when the ring is full. The NEW byte
 * is the one lost: the bytes already queued are older and form the lines the
 * reader is part-way through, so the stream loses its tail, not its middle.
 * tx_lock held (or panicking). */
static void tx_put(char value)
{
    if (tx_used() >= SERIAL_TX_RING_SIZE) {
        __atomic_store_n(&tx_dropped, tx_dropped + 1, __ATOMIC_RELAXED);
        return;
    }
    tx_ring[tx_head & SERIAL_TX_RING_MASK] = value;
    __atomic_store_n(&tx_head, tx_head + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&tx_queued, tx_queued + 1, __ATOMIC_RELAXED);
}

/* Queue a NUL-terminated string without the formatter: this runs inside the
 * serial driver, which kprintf sits on top of. */
static void tx_put_text(const char *text)
{
    while (*text)
        tx_put(*text++);
}

/* Queue an unsigned value in decimal; the drop notice's only number. */
static void tx_put_decimal(uint64_t value)
{
    char digits[20];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        tx_put(digits[--count]);
}

/* Silent loss would look like a corrupt log, so once the ring has room again
 * the first byte after a drop is preceded by a one-line notice. It starts with
 * CRLF because the hole usually ends mid-line. tx_lock held (or panicking). */
static void tx_report_drops(void)
{
    uint64_t unreported = tx_dropped - tx_dropped_reported;
    if (!unreported)
        return;
    if (SERIAL_TX_RING_SIZE - tx_used() < SERIAL_DROP_NOTICE_ROOM)
        return;
    tx_put_text("\r\n[serial] ");
    tx_put_decimal(unreported);
    tx_put_text(" byte(s) lost (TX ring full or UART stuck)\r\n");
    __atomic_store_n(&tx_dropped_reported, tx_dropped_reported + unreported, __ATOMIC_RELAXED);
}

/* Write up to one FIFO's worth of queued bytes. The caller has JUST seen THRE
 * set, which in FIFO mode means all 16 slots are free, so no per-byte status
 * read is needed. A ready reading is also proof the UART is alive, so it ends
 * any earlier "wedged" verdict. tx_lock held (or panicking). */
static void tx_write_burst(void)
{
    unsigned written = 0;
    tx_wedged = 0;
    while (written < tx_burst && tx_pending()) {
        outb(UART_THR, (uint8_t)tx_ring[tx_tail & SERIAL_TX_RING_MASK]);
        /* Tell the scheduler observer that a byte really left for the UART.
         * Counting here, at the transmit register, also covers code that
         * bypasses kprintf and bytes queued before a measurement started
         * and drained during it; counting at enqueue would miss both. */
        flight_count_uart_byte();
        __atomic_store_n(&tx_tail, tx_tail + 1, __ATOMIC_RELEASE);
        written++;
    }
    __atomic_store_n(&tx_sent, tx_sent + written, __ATOMIC_RELAXED);
}

/* The non-waiting drain: one status read, and a burst only if the FIFO is
 * empty. If it is not, the bytes stay queued for the next putc or tick.
 * tx_lock held. */
static void tx_drain_opportunistic(void)
{
    if (!tx_pending())
        return;
    if (!tx_ready())
        return;
    tx_write_burst();
}

/* Give up on a UART that will not take bytes: throw the queue away and count
 * it, because the alternative is waiting SERIAL_WAIT_SPINS again for every
 * byte of every later message. tx_lock held, or panicking. */
static void tx_abandon(void)
{
    __atomic_store_n(&tx_dropped, tx_dropped + tx_used(), __ATOMIC_RELAXED);
    __atomic_store_n(&tx_tail, tx_head, __ATOMIC_RELEASE);
    tx_wedged = 1;
}

/* How many failed status polls a blocking drain tolerates before abandoning:
 * the full bound normally, one when the previous drain already proved the UART
 * dead (a later successful THRE clears that verdict). */
static unsigned tx_wait_limit(void)
{
    return tx_wedged ? 1 : SERIAL_WAIT_SPINS;
}

/* The waiting drain, for panic and the negative control: keep writing bursts
 * until the ring is empty, polling THRE with a bounded wait; abandon the rest
 * if the UART never becomes ready ("drop after a generous timeout" is the old
 * contract). tx_lock held, or panicking. */
static void tx_drain_blocking(void)
{
    unsigned spins = 0;
    unsigned limit = tx_wait_limit();
    while (tx_pending()) {
        if (tx_ready()) {
            tx_write_burst();
            spins = 0;
            continue;
        }
        if (++spins >= limit) {
            tx_abandon();
            return;
        }
    }
}

/* Queue one byte and give the UART whatever it has room for right now. It
 * returns without waiting whatever the UART is doing: a byte that does not fit
 * the FIFO stays in the ring for the next putc or serial_tx_tick(). Only a
 * panicking machine drains synchronously (see tx_drain_blocking). */
void serial_putc(char c)
{
    uint64_t flags = tx_lock_enter();
    tx_report_drops();
    tx_put(c);
#ifdef SERIAL_NEGCTL_BLOCKING_PUTC
    /* Negative control for tests/hw/serial: the pre-ring behaviour, a wait on
     * the UART inside every putc. The host gate must go red on it. */
    tx_drain_blocking();
#else
    if (flags == TX_LOCK_NOT_TAKEN)
        tx_drain_blocking();
    else
        tx_drain_opportunistic();
#endif
    tx_lock_leave(flags);
}

/* A whole string as one unit against kprintf and other serial_puts callers:
 * the message lock is held while the bytes are queued, which is now just
 * a memory copy, so the lock is no longer held across any UART wait. */
void serial_puts(const char *s)
{
    uint64_t flags = kprintf_console_enter();
    while (*s)
        serial_putc(*s++);
    kprintf_console_leave(flags);
}

/* Periodic drain, called from the timer tick on every core. A byte queued while
 * the FIFO was busy has no producer to push it later if the system goes quiet,
 * so something has to come back for it. This does exactly what a putc does
 * minus the enqueue: try-lock (a tick must never spin on a lock a preempted
 * putc holds), one status read, at most one burst. It takes neither
 * printf_lock nor any lock a caller of kprintf could be holding. */
void serial_tx_tick(void)
{
    if (__atomic_load_n(&tx_tail, __ATOMIC_RELAXED) == __atomic_load_n(&tx_head, __ATOMIC_RELAXED))
        return;
    if (__atomic_load_n(&serial_panicking, __ATOMIC_RELAXED))
        return;
    uint64_t flags = io_irq_save();
    if (!io_lock_try(&tx_lock)) {
        io_irq_restore(flags);
        return;
    }
    tx_drain_opportunistic();
    io_lock_leave(&tx_lock, flags);
}

/* Drain for real, then wait for the shifter. Each burst is written under the
 * lock but the wait for the next THRE is not, so other CPUs can keep queueing
 * (and the TLB mailboxes keep being served) while this one waits. After the
 * ring is empty TEMT (LSR bit 6) is the "last stop bit has left the pin"
 * signal: THRE alone fires when the last byte moves to the shifter, and a CF9
 * reset or S5 write straight after would cut that byte off. Bounded: a UART
 * that stops answering costs SERIAL_WAIT_SPINS polls once, not forever. */
void serial_flush_blocking(void)
{
    unsigned spins = 0;
    unsigned limit = tx_wait_limit();
    int gave_up = 0;
    for (;;) {
        uint64_t flags = tx_lock_enter();
        int pending = tx_pending();
        if (pending && tx_ready()) {
            tx_write_burst();
            spins = 0;
            pending = tx_pending();
        } else if (pending && ++spins >= limit) {
            tx_abandon();
            gave_up = 1;
            pending = 0;
        }
        tx_lock_leave(flags);
        if (!pending)
            break;
    }
    if (gave_up)
        return;
    for (spins = 0; spins < limit; spins++) {
        if (inb(UART_LSR) & LSR_TEMT)
            return;
    }
}

/* Snapshot of the counters for HWHUD's "con" line and the host gate. The
 * indices are read tail first, then head, so a concurrent producer can only
 * make the fill level look larger, never negative; it is clamped to the ring. */
void serial_tx_stats(struct serial_tx_stats *out)
{
    unsigned tail = __atomic_load_n(&tx_tail, __ATOMIC_RELAXED);
    unsigned head = __atomic_load_n(&tx_head, __ATOMIC_RELAXED);
    unsigned used = head - tail;
    out->queued = __atomic_load_n(&tx_queued, __ATOMIC_RELAXED);
    out->sent = __atomic_load_n(&tx_sent, __ATOMIC_RELAXED);
    out->dropped = __atomic_load_n(&tx_dropped, __ATOMIC_RELAXED);
    out->ring_used = used > SERIAL_TX_RING_SIZE ? SERIAL_TX_RING_SIZE : used;
    out->ring_size = SERIAL_TX_RING_SIZE;
}

/* Bytes refused because the ring was full (plus any abandoned on a dead UART).
 * Nonzero means the serial log has a hole. */
uint64_t serial_dropped(void)
{
    return __atomic_load_n(&tx_dropped, __ATOMIC_RELAXED);
}

/* Non-blocking receive: return the next byte from COM1, or -1 if none waiting.
 * The serial console (F_TTY) polls this; there is no serial RX IRQ. */
int serial_getc(void)
{
    uint64_t flags = io_lock_enter(&rx_lock);
    int value = (inb(UART_LSR) & LSR_DATA_READY) ? inb(UART_RBR) : -1;
    io_lock_leave(&rx_lock, flags);
    return value;
}
