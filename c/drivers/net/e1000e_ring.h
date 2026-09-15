/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_E1000E_RING_H
#define LOGIT_E1000E_RING_H
#include <stdint.h>
#define E1K_RX_COUNT 64u
#define E1K_TX_COUNT 32u
#define E1K_BUFFER 2048u
#define E1K_MAX_FRAME 1518u
#define E1K_MDIC_READY (1u << 28)
#define E1K_MDIC_ERROR (1u << 30)
struct e1k_rx_desc {
    uint64_t addr;
    uint16_t length, checksum;
    uint8_t status, errors;
    uint16_t special;
};
struct e1k_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso, cmd, status, css;
    uint16_t special;
};
_Static_assert(sizeof(struct e1k_rx_desc) == 16, "82574 legacy RX descriptor");
_Static_assert(sizeof(struct e1k_tx_desc) == 16, "82574 legacy TX descriptor");
static inline int e1k_rx_valid(uint8_t status, uint8_t errors, unsigned length)
{
    return (status & 3) == 3 && !errors && length >= 14 && length <= E1K_MAX_FRAME;
}
/* MDIC is an asynchronous mailbox. READY alone does not make ERROR data valid,
 * and checking the echoed register catches a stale completion before a PHY
 * reset or link decision is based on another register's contents. */
static inline int e1k_mdic_valid(uint32_t value, unsigned reg)
{
    if (!(value & E1K_MDIC_READY)) return 0;
#ifndef E1000E_NEGCTL_IGNORE_MDIC_ERROR
    if (value & E1K_MDIC_ERROR) return 0;
#endif
    return ((value >> 16) & 31u) == reg && ((value >> 21) & 31u) == 1;
}
#endif
