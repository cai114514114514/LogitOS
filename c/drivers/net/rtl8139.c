#include <stdint.h>
#include <stddef.h>
#include "netdev.h"
#include "netring.h"
#include "pci.h"
#include "pmm.h"
#include "net.h"
#include "io.h"
#include "kprintf.h"

/* Realtek RTL8139 -- the "everything had one of these" 100 Mbit part, and the
 * only non-Intel NIC QEMU emulates (`-device rtl8139`).
 *
 * It is nothing like the e1000: there are no receive descriptors at all. The
 * chip writes packets back to back into one flat 8 KiB ring, each preceded by a
 * 4-byte {status, size} header, and the driver's whole job on receive is to
 * keep an offset and tell the chip how far it has read. Transmit is four fixed
 * registers, not a ring.
 *
 * Registers are reached through the I/O BAR, which is how this chip is normally
 * driven and avoids needing the MMIO BAR mapped.
 */

void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);

#define R_IDR0     0x00      /* MAC, 6 bytes                    */
#define R_MAR0     0x08      /* multicast filter, 8 bytes       */
#define R_TSD0     0x10      /* transmit status  0..3, +4 each  */
#define R_TSAD0    0x20      /* transmit address 0..3, +4 each  */
#define R_RBSTART  0x30      /* receive ring physical base (32-bit!) */
#define R_CR       0x37      /* command                         */
#define R_CAPR     0x38      /* current address of packet read  */
#define R_CBR      0x3A
#define R_IMR      0x3C
#define R_ISR      0x3E
#define R_TCR      0x40
#define R_RCR      0x44
#define R_CFG9346  0x50
#define R_CONFIG1  0x52

#define CR_BUFE    0x01      /* receive buffer EMPTY */
#define CR_TE      0x04
#define CR_RE      0x08
#define CR_RST     0x10

#define ISR_ROK    0x0001
#define ISR_RER    0x0002
#define ISR_TOK    0x0004
#define ISR_TER    0x0008
#define ISR_RXOVW  0x0010
#define ISR_FOVW   0x0040

#define TSD_OWN    0x00002000   /* set by the chip when its DMA of this buffer is done */
#define TSD_TOK    0x00008000
#define TSD_ERTXTH (8u << 16)   /* start transmitting after 8*32 = 256 bytes are in the FIFO */

#define CFG9346_UNLOCK 0xC0
#define CFG9346_LOCK   0x00

#define TX_SLOTS   4            /* the chip has exactly four, in hardware */
#define ETH_MIN    60           /* the 8139 does not pad; a runt is dropped on the wire */
#define FRAME_MAX  1518

static uint16_t io;             /* I/O port base */
static uint8_t *rxbuf;          /* RTL8139_RXBUF_PAD bytes, contiguous, < 4 GiB */
static uint8_t *txbuf[TX_SLOTS];
static uint32_t rx_off;         /* our read position in the 8 KiB ring */
static uint32_t tx_cur;
static int      ready;
static net_rx_cb g_rxcb;

/* Re-arm the receiver after an overflow or a header the chip could not have
 * written. Cheaper than it looks and vastly better than the alternative, which
 * is a receive path that is silently desynchronised from the ring for the rest
 * of the boot. */
static void rx_reset(void)
{
    outb(io + R_CR, CR_TE);                       /* RX off */
    rx_off = 0;
    outw(io + R_CAPR, rtl8139_capr(0));
    outb(io + R_CR, CR_TE | CR_RE);
}

static int rtl_rx_poll(net_rx_cb cb)
{
    if (!ready) return 0;
    uint64_t f = net_lock();
    int n = 0, budget = 64;                       /* bounded: this also runs in the ISR */
    while (budget-- > 0 && !(inb(io + R_CR) & CR_BUFE)) {
        const uint8_t *p = rxbuf + rx_off;
        uint16_t status = rtl8139_rx_status(p);
        uint16_t size   = rtl8139_rx_size(p);

        /* 0xFFF0 is the chip saying "this packet is still being DMA'd" -- not a
         * length. Reading it as one walks the offset into the middle of a frame
         * and every subsequent packet is garbage. */
        if (size == 0xFFF0) break;
        if (size < 4 + 14 || size > FRAME_MAX + 4) { rx_reset(); break; }

        if (rtl8139_rx_ok(status))
            cb(p + 4, (uint16_t)(size - 4));      /* -4: the size includes the CRC */

        rx_off = rtl8139_next_off(rx_off, size);
        outw(io + R_CAPR, rtl8139_capr(rx_off));  /* CAPR trails by 16 -- see netring.h */
        n++;
    }
    net_unlock(f);
    return n;
}

/* Contract: callers hold net_lock (IF=0). */
static int rtl_tx(const void *frame, uint16_t len)
{
    if (!ready || len == 0 || len > FRAME_MAX) return -1;
    uint32_t i = tx_cur;
    uint16_t port = (uint16_t)(io + R_TSD0 + 4 * i);
    int spins = 0;
    while (!(inl(port) & TSD_OWN)) {              /* previous send still in DMA */
        if (++spins > 1000000) return -1;
    }
    uint16_t n = len;
    if (n < ETH_MIN) {                            /* pad: no hardware short-packet pad */
        memset(txbuf[i], 0, ETH_MIN);
        n = ETH_MIN;
    }
    memcpy(txbuf[i], frame, len);
    outl(io + R_TSAD0 + 4 * i, (uint32_t)(uintptr_t)txbuf[i]);
    /* Writing TSD with a length clears OWN and starts the transmit. */
    outl(port, TSD_ERTXTH | n);
    tx_cur = ring_next(i, TX_SLOTS);
    return 0;
}

static void rtl_irq_on(net_rx_cb cb)
{
    if (!ready) return;
    g_rxcb = cb;
    outw(io + R_ISR, 0xFFFF);                     /* ISR is write-1-to-clear */
    outw(io + R_IMR, ISR_ROK | ISR_RER | ISR_RXOVW | ISR_FOVW);
}

static void rtl_isr(void)
{
    if (!ready) return;
    uint16_t isr = inw(io + R_ISR);
    outw(io + R_ISR, isr);                        /* ack before draining, so a frame
                                                   * arriving mid-drain still raises
                                                   * a fresh edge */
    if (isr & (ISR_RXOVW | ISR_FOVW)) rx_reset();
    /* Ack here, drain on SOFTIRQ_NET -- see c/net/core/net.c. */
    if (g_rxcb) net_rx_schedule();
}

static struct netdev rtl_dev = {
    .name = "rtl8139", .irq_line = -1,
    .tx = rtl_tx, .rx_poll = rtl_rx_poll,
    .irq_enable = rtl_irq_on, .irq = rtl_isr,
};

/* This chip's registers are behind its I/O BAR. The device model already sized
 * and classified every BAR at enumeration, so this is a lookup rather than the
 * hand-rolled config-space read it used to be -- and it gets the I/O-BAR mask
 * right (2 low bits, not 4), which the legacy pci_dev->bar0 did not. */
static uint16_t find_io_bar(const struct device *dev)
{
    for (int b = 0; b < DEV_NRES; b++)
        if ((dev->res[b].flags & DEV_RES_IO) && dev->res[b].start)
            return (uint16_t)dev->res[b].start;
    return 0;
}

int rtl8139_probe(struct device *dev)
{
    if (ready) return -1;                         /* one NIC bound at a time */
    dev_enable(dev, 1);                           /* I/O decode + bus master (DMA) */
    io = find_io_bar(dev);
    if (!io) { kprintf("[rtl8139] no I/O BAR\n"); return -1; }

    /* The receive ring must be physically contiguous, and RBSTART is a 32-BIT
     * register: this chip cannot DMA above 4 GiB at all. Refusing here is the
     * difference between "no network" and "the chip DMAs into the truncated
     * low-32-bit alias of our buffer", which is memory corruption elsewhere in
     * the kernel with no hint that the NIC caused it. */
    uint64_t phys = pmm_alloc_contig((RTL8139_RXBUF_PAD + 4095) / 4096);
    if (!phys) { kprintf("[rtl8139] rx ring alloc failed\n"); return -1; }
    if (phys + RTL8139_RXBUF_PAD > 0x100000000ull) {
        kprintf("[rtl8139] rx ring above 4 GiB (%p): chip cannot address it\n", (void *)phys);
        return -1;
    }
    rxbuf = (uint8_t *)(uintptr_t)phys;
    memset(rxbuf, 0, RTL8139_RXBUF_PAD);

    for (int i = 0; i < TX_SLOTS; i++) {
        uint64_t t = pmm_alloc();
        if (!t || t + 4096 > 0x100000000ull) { kprintf("[rtl8139] tx buffer alloc failed\n"); return -1; }
        txbuf[i] = (uint8_t *)(uintptr_t)t;
    }

    /* Power on, then soft reset and wait for the chip to clear RST itself. */
    outb(io + R_CFG9346, CFG9346_UNLOCK);
    outb(io + R_CONFIG1, 0x00);
    outb(io + R_CFG9346, CFG9346_LOCK);
    outb(io + R_CR, CR_RST);
    int spins = 0;
    while (inb(io + R_CR) & CR_RST) {
        if (++spins > 1000000) { kprintf("[rtl8139] reset timeout\n"); return -1; }
    }

    for (int i = 0; i < 6; i++) rtl_dev.mac[i] = inb((uint16_t)(io + R_IDR0 + i));
    rtl_dev.irq_line = dev->irq_line;

    outb(io + R_CFG9346, CFG9346_UNLOCK);
    outb(io + R_CR, CR_TE | CR_RE);               /* enable BEFORE writing RCR/TCR */
    /* RCR: accept physical-match + multicast + broadcast (not promiscuous, not
     * runts, not errored), WRAP set so a packet crossing the end of the ring is
     * written contiguously into the pad instead of split, max DMA burst, and no
     * receive FIFO threshold (hand us whole packets). */
    outl(io + R_RCR, 0x0Eu | 0x80u | (7u << 8) | (7u << 13));
    outl(io + R_TCR, 0x03000700u);                /* normal interframe gap, max DMA burst */
    outl(io + R_MAR0, 0xFFFFFFFFu);               /* accept all multicast groups */
    outl(io + R_MAR0 + 4, 0xFFFFFFFFu);
    outl(io + R_RBSTART, (uint32_t)phys);
    rx_off = 0;
    outw(io + R_CAPR, rtl8139_capr(0));
    outb(io + R_CFG9346, CFG9346_LOCK);

    outw(io + R_ISR, 0xFFFF);
    outw(io + R_IMR, 0);                          /* polled until irq_enable */
    outb(io + R_CR, CR_TE | CR_RE);
    tx_cur = 0;
    ready = 1;

    kprintf("[rtl8139] up: io=%x rxring=%p (%d B)\n", io, (void *)phys, RTL8139_RXBUF_PAD);
    dev_set_drvdata(dev, &rtl_dev);
    return 0;
}
