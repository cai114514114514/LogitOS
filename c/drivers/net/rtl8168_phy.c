#include "rtl8168.h"

/* Linux v6.12 r8169_main.c rtl_ocp_gphy_{read,write} and
 * r8169_phy_config.c r8168g_phy_param, rtl8168g_{1,2}_hw_phy_config:
 * https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/realtek/r8169_main.c
 * https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/realtek/r8169_phy_config.c
 * No external firmware is loaded. The G1 non-firmware register adjustments
 * still matter; using the much shorter G2 sequence on G1 loses those fixes.
 * not verified on X79-VG4 hardware. */
#define FLAG 0x80000000u
#define TRY(call) do { if ((call) < 0) return -1; } while (0)

int rtl8168_phy_read(const struct rtl_io *io, uint16_t reg)
{
    io->write32(0xb8, (uint32_t)reg << 15);
    TRY(rtl8168_wait(io, 0xb8, FLAG, 1, 25, 10));
    return io->read32(0xb8) & 0xffff;
}
int rtl8168_phy_write(const struct rtl_io *io, uint16_t reg, uint16_t value)
{
    io->write32(0xb8, FLAG | ((uint32_t)reg << 15) | value);
    return rtl8168_wait(io, 0xb8, FLAG, 0, 25, 10);
}
static int modify(const struct rtl_io *io, uint16_t reg, uint16_t clear, uint16_t set)
{
    int value = rtl8168_phy_read(io, reg);
    TRY(value);
    return rtl8168_phy_write(io, reg, (value & ~clear) | set);
}
/* Linux's paged MII accessor maps reg>=0x10 to page<<4 + (reg-0x10)*2.
 * Direct OCP addresses avoid mutable page state leaking into autonegotiation. */
static uint16_t address(uint16_t page, unsigned reg)
{ return (uint16_t)((page << 4) + (reg - 0x10) * 2); }
static int paged(const struct rtl_io *io, uint16_t page, unsigned reg,
                  uint16_t clear, uint16_t set)
{ return modify(io, address(page, reg), clear, set); }
static int parameter(const struct rtl_io *io, uint16_t reg, uint16_t clear, uint16_t set)
{
    TRY(rtl8168_phy_write(io, 0xa436, reg));
    return modify(io, 0xa438, clear, set);
}
static int g1_init(const struct rtl_io *io)
{
    int value = rtl8168_phy_read(io, address(0xa46, 0x10));
    TRY(value);
    TRY(paged(io, 0xbcc, 0x12, 0x8000, (value & 0x100) ? 0 : 0x8000));
    value = rtl8168_phy_read(io, address(0xa46, 0x13));
    TRY(value);
    TRY(paged(io, 0xc41, 0x15, 2, (value & 0x100) ? 2 : 0));
    TRY(paged(io, 0xa44, 0x11, 0, 0x0c));
    TRY(paged(io, 0xbcc, 0x14, 0x100, 0));
    TRY(paged(io, 0xa44, 0x11, 0, 0xc0));
    TRY(parameter(io, 0x8084, 0x6000, 0));
    TRY(paged(io, 0xa43, 0x10, 0, 0x1003));
    TRY(paged(io, 0xa4b, 0x11, 0, 4));
    TRY(parameter(io, 0x8012, 0, 0x8000));
    TRY(paged(io, 0xc42, 0x11, 0x2000, 0x4000));
    static const uint16_t swr[] = {0x5065,0xd065,0x1065,0x9065,0x1065};
    for (unsigned i = 0; i < sizeof(swr) / sizeof(*swr); i++) {
        TRY(rtl8168_phy_write(io, address(0xbcd, 0x14), swr[i]));
        if (i == 1) TRY(rtl8168_phy_write(io, address(0xbc8, 0x11), 0x5655));
    }
    return paged(io, 0xa43, 0x10, 4, 0); /* disable ALDPS */
}

static int wait_reset(const struct rtl_io *io)
{
    /* OCP FLAG completion only acknowledges the register read, not PHY reset.
     * BMCR.RESET self-clears; writing it clear can silently discard all tuning
     * that preceded it. The old final 0x8c00 clear mask did exactly that.
     * IEEE 802.3 permits 500 ms: poll with 1 ms spacing for 500 attempts,
     * plus each bounded OCP transaction's time. Propagate transport errors
     * instead of treating an unavailable BMCR as a completed reset.
     * https://www.kernel.org/doc/html/v4.14/networking/kapi.html#c.phy_poll_reset
     * Wait for an inherited reset; do not issue another one after MAC/ePHY
     * setup. not verified on X79-VG4 hardware. */
    for (unsigned i = 0; i < 500; i++) {
        int value = rtl8168_phy_read(io, 0xa400);
        TRY(value);
        if (!(value & 0x8000)) return 0;
        io->delay(1000);
    }
    return -1;
}

int rtl8168_phy_init(enum rtl_mac_ver ver, const struct rtl_io *io)
{
    TRY(wait_reset(io));
    /* BMCR power-down/isolate bits must not survive a Windows warm reboot.
     * Standard page 0 maps to OCP 0xa400 (r8169_main.c, r8168g_mdio_write).
     * Linux's VER_40 suspend quirk re-enables PHY MCU interrupts on resume. */
    if (ver == RTL_GIGA_MAC_VER_40) {
        io->write32(0x74, 0xf1a8);
        TRY(rtl8168_wait(io, 0x74, FLAG, 1, 100, 100));
        io->write32(0x70, io->read32(0x70) | 0xfc000000);
        io->write32(0x74, FLAG | 0xf1a8);
        TRY(rtl8168_wait(io, 0x74, FLAG, 0, 100, 100));
        TRY(g1_init(io));
    }
    TRY(paged(io, 0xa43, 0x11, 0, 0x10)); /* EEE PHY configuration */
    /* Advertise 10/100 half/full and 1000 full. Deliberately do not advertise
     * PAUSE: the network stack has no flow-control negotiation policy. */
    TRY(modify(io, 0xa408, 0x0de0, 0x01e1)); /* MII_ADVERTISE */
    TRY(modify(io, 0xa412, 0x0300, 0x0200)); /* MII_CTRL1000 */
    return modify(io, 0xa400, 0x0c00, 0x1200); /* clear PDOWN/ISOLATE; ANENABLE | ANRESTART */
}
#undef TRY
