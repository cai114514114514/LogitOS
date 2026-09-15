/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_PCNET_RING_H
#define LOGIT_PCNET_RING_H
#include <stdint.h>
#include <stddef.h>

/* AMD Am79C970A data sheet 19436, Software Style 2, Initialization Block
 * and RMD/TMD formats. These are 16-byte descriptors, not the reset-default
 * eight-byte LANCE format with 24-bit addresses. BCR20 must select style 2
 * before CSR1/CSR2 publish any of these addresses.
 * https://www.amd.com/content/dam/amd/en/documents/archived-tech-docs/datasheets/19436.pdf
 */
#define PCNET_RING_LOG2 5u
#define PCNET_RING_COUNT (1u << PCNET_RING_LOG2)
#define PCNET_BUF_SIZE 2048u
#define PCNET_FRAME_MAX 1518u
#define PCNET_OWN 0x8000u
#define PCNET_ERR 0x4000u
#define PCNET_STP 0x0200u
#define PCNET_ENP 0x0100u

struct pcnet_desc {
    uint32_t addr;
    uint16_t bcnt;
    uint16_t status;
    uint32_t misc;
    uint32_t reserved;
};
struct pcnet_init_block {
    uint16_t mode;
    uint8_t rlen, tlen;
    uint8_t mac[6];
    uint16_t reserved;
    uint32_t multicast[2];
    uint32_t rx_ring, tx_ring;
};
_Static_assert(sizeof(struct pcnet_desc) == 16, "PCnet style2 descriptor size");
_Static_assert(offsetof(struct pcnet_desc, status) == 6, "PCnet OWN field");
_Static_assert(sizeof(struct pcnet_init_block) == 28, "PCnet init block size");
_Static_assert(offsetof(struct pcnet_init_block, rx_ring) == 20, "PCnet ring bases");

static inline unsigned pcnet_next(unsigned n)
{ return (n + 1u) & (PCNET_RING_COUNT - 1u); }

/* BCNT is a negative 12-bit count with its top four bits REQUIRED to be one.
 * A positive length looks innocuous but the NIC treats it as another buffer
 * size. Payload buffers are 2048 bytes so a normal Ethernet packet never
 * needs descriptor chaining; a chain is rejected rather than truncated. */
static inline uint16_t pcnet_bcnt(unsigned bytes)
{ return (uint16_t)(0xf000u | ((0u - bytes) & 0xfffu)); }

static inline unsigned pcnet_rx_length(uint16_t status, uint32_t misc)
{
    if (status & PCNET_OWN) return 0;
    if ((status & (PCNET_STP | PCNET_ENP)) !=
        (PCNET_STP | PCNET_ENP)) return 0;
#ifndef PCNET_NEGCTL_IGNORE_ERROR
    if (status & PCNET_ERR) return 0;
#endif
    unsigned n = misc & 0xfffu;
    /* MCNT includes the four-byte FCS, even when the emulator supplied it.
     * Refuse impossible lengths before the network stack sees any buffer. */
    if (n < 14u + 4u || n > PCNET_FRAME_MAX + 4u) return 0;
    return n - 4u;
}
#endif
