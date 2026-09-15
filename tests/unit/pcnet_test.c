/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include "pcnet_ring.h"
static unsigned checks, failures;
#define CHECK(c, label) do { checks++; if (!(c)) { failures++; printf("FAIL: %s\n", label); } } while (0)
int main(void)
{
    CHECK(pcnet_bcnt(2048) == 0xf800, "2048-byte RX buffer encodes negative BCNT");
    CHECK(pcnet_bcnt(60) == 0xffc4, "minimum TX frame encodes negative BCNT");
    CHECK(pcnet_bcnt(1518) == 0xfa12, "maximum TX frame encodes negative BCNT");
    unsigned slot = 0;
    for (unsigned i = 0; i < PCNET_RING_COUNT * 3; i++) slot = pcnet_next(slot);
    CHECK(slot == 0, "ring wraps after three complete laps");
    CHECK(pcnet_rx_length(PCNET_STP | PCNET_ENP, 64) == 60, "strip Ethernet FCS");
    CHECK(pcnet_rx_length(PCNET_STP | PCNET_ENP, 1522) == 1518, "accept maximum frame");
    CHECK(pcnet_rx_length(PCNET_STP | PCNET_ENP, 18) == 14, "accept minimum Ethernet header");
    CHECK(pcnet_rx_length(PCNET_OWN | PCNET_STP | PCNET_ENP, 64) == 0,
          "device-owned descriptor is never delivered");
    CHECK(pcnet_rx_length(PCNET_ERR | PCNET_STP | PCNET_ENP, 64) == 0, "reject device error");
    CHECK(pcnet_rx_length(PCNET_STP, 64) == 0, "reject incomplete first fragment");
    CHECK(pcnet_rx_length(PCNET_ENP, 64) == 0, "reject tail of chained frame");
    CHECK(pcnet_rx_length(0, 64) == 0, "reject absent packet boundary");
    CHECK(pcnet_rx_length(PCNET_STP | PCNET_ENP, 0) == 0, "reject zero length before FCS subtraction");
    CHECK(pcnet_rx_length(PCNET_STP | PCNET_ENP, 17) == 0, "reject truncated Ethernet header");
    CHECK(pcnet_rx_length(PCNET_STP | PCNET_ENP, 1523) == 0, "reject oversize frame");
    CHECK(pcnet_rx_length(PCNET_STP | PCNET_ENP, 4095) == 0, "reject DMA length beyond buffer");
    printf("PCNET_RING: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
