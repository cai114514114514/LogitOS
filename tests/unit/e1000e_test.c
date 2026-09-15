/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include "e1000e_ring.h"
static unsigned count, failed;
#define CHECK(c, text) do { count++; if (!(c)) { failed++; printf("FAIL: %s\n", text); } } while (0)
int main(void)
{
    uint32_t ok = E1K_MDIC_READY | (1u << 21) | (2u << 16) | 0x141;
    CHECK(e1k_mdic_valid(ok, 2), "accept completed PHY ID1 transaction");
    CHECK(!e1k_mdic_valid(ok & ~E1K_MDIC_READY, 2), "wait for MDIC completion");
    CHECK(!e1k_mdic_valid(ok | E1K_MDIC_ERROR, 2), "reject completed MDIO hardware error");
    CHECK(!e1k_mdic_valid(ok, 3), "reject stale MDIO register completion");
    CHECK(!e1k_mdic_valid(ok & ~(1u << 21), 2), "reject another PHY address");
    CHECK(e1k_rx_valid(3, 0, 14), "minimum Ethernet header delivered");
    CHECK(e1k_rx_valid(3, 0, 1518), "maximum untagged or VLAN frame delivered");
    CHECK(!e1k_rx_valid(2, 0, 64), "device-owned descriptor not delivered");
    CHECK(!e1k_rx_valid(1, 0, 64), "split receive descriptor not delivered");
    CHECK(!e1k_rx_valid(3, 1, 64), "receive CRC error rejected");
    CHECK(!e1k_rx_valid(3, 0, 13), "truncated Ethernet header rejected");
    CHECK(!e1k_rx_valid(3, 0, 1519), "oversize receive rejected");
    CHECK(!e1k_rx_valid(3, 0, 65535), "DMA length beyond buffer rejected");
    printf("E1000E_RING: %u checks, %u failures\n", count, failed);
    return failed != 0;
}
