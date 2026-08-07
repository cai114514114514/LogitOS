#ifndef LOGIT_NETRING_H
#define LOGIT_NETRING_H

#include <stdint.h>

/* Pure descriptor-ring arithmetic and device-header field extraction, shared by
 * the NIC drivers in this directory. Nothing here touches a device, allocates,
 * or includes a kernel header -- which is the point: this is the part of a NIC
 * driver that is worth unit-testing, and tests/unit/net_drv_test.c includes
 * this file directly and checks every function below against hand-computed
 * values, including the wraparound cases that only show up after minutes of
 * real traffic. */

/* ---------------- generic ring index ---------------- */

static inline uint32_t ring_next(uint32_t i, uint32_t n) { return (i + 1u) % n; }

/* ---------------- virtio split virtqueue ---------------- */

/* used->idx and our last_used are FREE-RUNNING 16-bit counters that wrap at
 * 65536; they are NOT indices into the ring. The number of completions we have
 * not consumed is their unsigned 16-bit difference. Writing `used->idx >
 * last_used` instead breaks exactly once per 65536 buffers -- on a NIC that is
 * minutes of traffic, not never, and the failure is a silently stalled receive
 * path rather than anything that looks like a wraparound bug. */
static inline uint16_t vq_pending(uint16_t used_idx, uint16_t last_used)
{
    return (uint16_t)(used_idx - last_used);
}

/* Which slot of the used ring holds completion number `last_used`. */
static inline uint16_t vq_slot(uint16_t last_used, uint16_t qsize)
{
    return (uint16_t)(last_used % qsize);
}

/* virtio-net: with VIRTIO_F_VERSION_1 negotiated the per-packet header is
 * ALWAYS 12 bytes -- num_buffers is present whether or not MRG_RXBUF was
 * negotiated. Assuming the legacy 10-byte header shifts every frame by two
 * bytes, which does not look like an off-by-two: it looks like the NIC
 * receiving garbage that never parses as Ethernet. */
#define VNET_HDR_LEN 12

/* ---------------- RTL8139 receive ring ---------------- */

/* The 8139 receives into ONE flat ring (not descriptors). We use the 8 KiB
 * setting; the buffer must be 8192 + 16 + 1500 bytes because with the WRAP bit
 * set the chip writes a packet that crosses the end contiguously into the pad
 * instead of splitting it. */
#define RTL8139_RXBUF     8192
/* +16 for the header the chip may write at the very end, +2048 of wrap slack --
 * a full frame's worth, the same margin Linux's 8139too uses. 1500 is NOT
 * enough: an Ethernet frame is up to 1514 bytes plus its 4-byte CRC. */
#define RTL8139_RXBUF_PAD (RTL8139_RXBUF + 16 + 2048)

/* Each packet in the ring is preceded by a 4-byte header: u16 status, u16 size
 * (the size INCLUDES the 4-byte Ethernet CRC). */
static inline uint16_t rtl8139_rx_status(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint16_t rtl8139_rx_size(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
}
/* ROK (bit 0) set and none of FAE/CRC/LONG/RUNT/ISE (bits 1..5). */
static inline int rtl8139_rx_ok(uint16_t status)
{
    return (status & 0x0001u) != 0 && (status & 0x003Eu) == 0;
}
/* Advance past header + frame, round UP to 4 bytes, wrap the ring. */
static inline uint32_t rtl8139_next_off(uint32_t off, uint16_t size)
{
    return ((off + (uint32_t)size + 4u + 3u) & ~3u) % RTL8139_RXBUF;
}
/* CAPR is written 16 bytes BEHIND the offset we are actually up to. This is the
 * single most-reported 8139 driver bug: get it wrong and the chip decides the
 * ring is full after the first 8 KiB and receive stops dead, with no error bit
 * anywhere to say so. The subtraction is deliberately modulo 65536, so offset 0
 * writes 0xFFF0 -- that is what the chip expects, not a bug. */
static inline uint16_t rtl8139_capr(uint32_t off)
{
    return (uint16_t)((off - 16u) & 0xFFFFu);
}

/* ---------------- RTL8169 descriptors ---------------- */

/* 16-byte descriptor: u32 opts1, u32 opts2, u64 addr. Ownership and the
 * end-of-ring marker both live in opts1. */
#define RTL8169_OWN   (1u << 31)   /* set = the NIC owns this descriptor */
#define RTL8169_EOR   (1u << 30)   /* last descriptor of the ring */
#define RTL8169_FS    (1u << 29)   /* first segment */
#define RTL8169_LS    (1u << 28)   /* last segment  */
#define RTL8169_RES   (1u << 21)   /* RX: receive error summary */
#define RTL8169_LEN_MASK 0x3FFFu

static inline int      rtl8169_own(uint32_t opts1) { return (opts1 & RTL8169_OWN) != 0; }
/* RX frame length, which INCLUDES the 4-byte CRC. */
static inline uint16_t rtl8169_len(uint32_t opts1) { return (uint16_t)(opts1 & RTL8169_LEN_MASK); }
/* A usable receive: chip has released it, it is a whole single-descriptor
 * frame (FS and LS both set) and the error summary is clear. */
static inline int rtl8169_rx_ok(uint32_t opts1)
{
    return !rtl8169_own(opts1) && (opts1 & RTL8169_FS) && (opts1 & RTL8169_LS) &&
           !(opts1 & RTL8169_RES);
}
/* Hand a receive descriptor back to the chip. */
static inline uint32_t rtl8169_rx_opts1(uint32_t bufsz, int last)
{
    return RTL8169_OWN | (last ? RTL8169_EOR : 0u) | (bufsz & RTL8169_LEN_MASK);
}
/* A single-descriptor transmit. */
static inline uint32_t rtl8169_tx_opts1(uint32_t len, int last)
{
    return RTL8169_OWN | RTL8169_FS | RTL8169_LS | (last ? RTL8169_EOR : 0u) |
           (len & RTL8169_LEN_MASK);
}

#endif /* LOGIT_NETRING_H */
