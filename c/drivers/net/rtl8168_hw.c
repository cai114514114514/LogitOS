#include "rtl8168.h"
#include <stddef.h>

/* Register programs and indirect protocols checked against Linux v6.12:
 * https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/realtek/r8169_main.c
 * rtl_eri_{read,write}, r8168_mac_ocp_{read,write}, rtl_ephy_{read,write},
 * rtl_hw_start_8168g{,_1,_2}, rtl_hw_start_8411_2, rtl_hw_init_8168g.
 * Correction: earlier numeric ranges here used web-renderer lines; function
 * names refer to the source file without depending on that renumbering.
 * Model evidence only:
 * not verified on X79-VG4 hardware. Each failed indirect completion aborts
 * probe; treating a timeout's fabricated 0xffff as PHY data silently programs
 * a different chip configuration. */
#define FLAG 0x80000000u
#define TRY(call) do { if ((call) < 0) return -1; } while (0)

int rtl8168_wait(const struct rtl_io *io, uint32_t reg, uint32_t mask,
                 int high, unsigned delay, unsigned attempts)
{
    while (attempts--) {
        io->delay(delay);
        if (!!(io->read32(reg) & mask) == high) return 0;
    }
    return -1;
}

static int eri_write(const struct rtl_io *io, unsigned reg, unsigned bytes, uint32_t data)
{
    io->write32(0x70, data);
    io->write32(0x74, FLAG | (bytes << 12) | reg);
    return rtl8168_wait(io, 0x74, FLAG, 0, 100, 100);
}
static int eri_modify(const struct rtl_io *io, unsigned reg, uint32_t clear, uint32_t set)
{
    io->write32(0x74, 0xf000 | reg);
    TRY(rtl8168_wait(io, 0x74, FLAG, 1, 100, 100));
    return eri_write(io, reg, 15, (io->read32(0x70) & ~clear) | set);
}
static void mac_write(const struct rtl_io *io, unsigned reg, uint16_t data)
{ io->write32(0xb0, FLAG | (reg << 15) | data); }
static void mac_modify(const struct rtl_io *io, unsigned reg, uint16_t clear, uint16_t set)
{
    io->write32(0xb0, reg << 15);
    mac_write(io, reg, (io->read32(0xb0) & ~clear) | set);
}

struct ephy_entry { uint8_t reg; uint16_t clear, set; };
static const struct ephy_entry g1[] = {
    {0x00, 0x0008, 0x0000}, {0x0c, 0x3ff0, 0x0820},
    {0x1e, 0x0000, 0x0001}, /* G1 */
    {0x19, 0x8000, 0x0000},
};
static const struct ephy_entry g2[] = {
    {0x00, 0x0008, 0x0000}, {0x0c, 0x3ff0, 0x0820},
    {0x19, 0xffff, 0x7c00}, {0x1e, 0xffff, 0x20eb},
    {0x0d, 0xffff, 0x1666}, {0x00, 0xffff, 0x10a3},
    {0x06, 0xffff, 0xf050}, {0x04, 0x0000, 0x0010},
    {0x1d, 0x4000, 0x0000},
};
static const struct ephy_entry b8411[] = {
    {0x00, 0x0008, 0x0000}, {0x0c, 0x37d0, 0x0820},
    {0x1e, 0x0000, 0x0001}, {0x19, 0x8021, 0x0000},
    {0x1e, 0x0000, 0x2000}, {0x0d, 0x0100, 0x0200},
    {0x00, 0x0000, 0x0080}, {0x06, 0x0000, 0x0010},
    {0x04, 0x0000, 0x0010}, {0x1d, 0x0000, 0x4000},
};
static int ephy_init(const struct rtl_io *io, const struct ephy_entry *table, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        uint32_t address = (uint32_t)table[i].reg << 16;
        io->write32(0x80, address);
        TRY(rtl8168_wait(io, 0x80, FLAG, 1, 10, 100));
        uint16_t data = (io->read32(0x80) & ~table[i].clear) | table[i].set;
        io->write32(0x80, FLAG | address | data);
        TRY(rtl8168_wait(io, 0x80, FLAG, 0, 10, 100));
        io->delay(10);
    }
    return 0;
}

static int wait_fifo(const struct rtl_io *io)
{
    for (unsigned i = 0; i < 42; i++) {
        if (io->read8(0xd3) & 2) return 0;
        io->delay(100);
    }
    return -1;
}
int rtl8168_prepare(const struct rtl_io *io)
{
    io->write32(0xf0, io->read32(0xf0) | (1u << 19));
    io->delay(2000);
    /* Linux rtl_wait_txrx_fifo_empty waits BOTH TxConfig.EMPTY and MCU.RXTX_EMPTY.
     * MCU alone can look idle while the TX FIFO still owns pending traffic. */
    TRY(rtl8168_wait(io, 0x40, 1u << 11, 1, 100, 42));
    for (unsigned i = 0; ; i++) {
        if ((io->read8(0xd3) & 0x30) == 0x30) break;
        if (i == 41) return -1;
        io->delay(100);
    }
    io->write8(0x37, io->read8(0x37) & ~0x0c);
    io->delay(1000);
    io->write8(0xd3, io->read8(0xd3) & ~0x80);
    mac_modify(io, 0xe8de, 0x4000, 0);
    TRY(wait_fifo(io));
    mac_modify(io, 0xe8de, 0, 0x8000);
    return wait_fifo(io);
}

static int aspm_latency(const struct rtl_io *io, uint8_t function)
{
    /* CSI reaches extended config even when the platform has no ECAM.
     * Linux v6.12 r8169_main.c:2831-2850 encodes PCI_FUNC in BOTH
     * commands. Omitting it silently modifies function 0 on a multifunction
     * card, even though every direct MMIO access targets the bound function.
     * https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/realtek/r8169_main.c#L2831-L2850
     * Byte 0x70f=0x27 selects L0 7us, L1 16us. */
    uint32_t command = ((uint32_t)function << 16) | 0xf70c;
    io->write32(0x68, command);
    TRY(rtl8168_wait(io, 0x68, FLAG, 1, 10, 100));
    io->write32(0x64, (io->read32(0x64) & 0xffffff) | 0x27000000);
    io->write32(0x68, FLAG | command);
    return rtl8168_wait(io, 0x68, FLAG, 0, 10, 100);
}

static void fix_8411(const struct rtl_io *io)
{
    /* Linux v6.12 rtl8411b_fix_phy_down: MAC RX-after-PHY-powerdown fix.
     * This is the inline MAC program, not the optional external PHY firmware.
     * Omitting it while advertising VER_44 would leave warm boots broken. */
    static const uint16_t program[] = {
        0xe008,0xe00a,0xe00c,0xe00e,0xe027,0xe04f,0xe05e,0xe065,
        0xc602,0xbe00,0x0000,0xc502,0xbd00,0x074c,0xc302,0xbb00,
        0x080a,0x6420,0x48c2,0x8c20,0xc516,0x64a4,0x49c0,0xf009,
        0x74a2,0x8ca5,0x74a0,0xc50e,0x9ca2,0x1c11,0x9ca0,0xe006,
        0x74f8,0x48c4,0x8cf8,0xc404,0xbc00,0xc403,0xbc00,0x0bf2,
        0x0c0a,0xe434,0xd3c0,0x49d9,0xf01f,0xc526,0x64a5,0x1400,
        0xf007,0x0c01,0x8ca5,0x1c15,0xc51b,0x9ca0,0xe013,0xc519,
        0x74a0,0x48c4,0x8ca0,0xc516,0x74a4,0x48c8,0x48ca,0x9ca4,
        0xc512,0x1b00,0x9ba0,0x1b1c,0x483f,0x9ba2,0x1b04,0xc508,
        0x9ba0,0xc505,0xbd00,0xc502,0xbd00,0x0300,0x051e,0xe434,
        0xe018,0xe092,0xde20,0xd3c0,0xc50f,0x76a4,0x49e3,0xf007,
        0x49c0,0xf103,0xc607,0xbe00,0xc606,0xbe00,0xc602,0xbe00,
        0x0c4c,0x0c28,0x0c2c,0xdc00,0xc707,0x1d00,0x8de2,0x48c1,
        0xc502,0xbd00,0x00aa,0xe0c0,0xc502,0xbd00,0x0132,
    };
    static const uint16_t jumps[] = {0x0743,0x0801,0x0be9,0x02fd,0x0c25,0x00a9,0x012d};
    for (unsigned reg = 0xfc28; reg <= 0xfc36; reg += 2) mac_write(io, reg, 0);
    io->delay(3000);
    mac_write(io, 0xfc26, 0);
    for (unsigned i = 0; i < sizeof(program) / sizeof(*program); i++)
        mac_write(io, 0xf800 + 2 * i, program[i]);
    mac_write(io, 0xfc26, 0x8000);
    for (unsigned i = 0; i < sizeof(jumps) / sizeof(*jumps); i++)
        mac_write(io, 0xfc2a + 2 * i, jumps[i]);
}

int rtl8168_start(enum rtl_mac_ver ver, const struct rtl_io *io, uint8_t function)
{
    /* ASPM remains off: the OS has no runtime PCIe power policy. Linux also
     * refuses chip ASPM on VER_42 (rtl_hw_aspm_clkreq_enable).
     * Do this before ePHY access. */
    io->write8(0x53, io->read8(0x53) & ~0x80);
    io->write8(0x56, io->read8(0x56) & ~1);
    io->write8(0xec, 0x27); /* EarlySize, not the old 8169 0x3b */
    TRY(eri_write(io, 0xc8, 15, 0x00080002));
    TRY(eri_write(io, 0xe8, 15, 0x00100006));
    TRY(eri_write(io, 0xcc, 1, 0x38));
    TRY(eri_write(io, 0xd0, 1, 0x48));
    TRY(aspm_latency(io, function));
    TRY(eri_modify(io, 0xdc, 1, 0));
    TRY(eri_modify(io, 0xdc, 0, 1));
    TRY(eri_write(io, 0x2f8, 3, 0x1d8f));
    io->write32(0xf0, io->read32(0xf0) & ~(1u << 19));
    TRY(eri_write(io, 0xc0, 3, 0));
    TRY(eri_write(io, 0xb8, 3, 0));
    io->write8(0x1b, io->read8(0x1b) & ~7);
    TRY(eri_modify(io, 0x1b0, 0, 3));
    TRY(eri_modify(io, 0x2fc, 6, 1));
    TRY(eri_modify(io, 0x1b0, 1u << 12, 0));
    io->write8(0x54, io->read8(0x54) & ~2);
    switch (ver) {
    case RTL_GIGA_MAC_VER_40: TRY(ephy_init(io, g1, sizeof(g1) / sizeof(*g1))); break;
    case RTL_GIGA_MAC_VER_42: TRY(ephy_init(io, g2, sizeof(g2) / sizeof(*g2))); break;
    case RTL_GIGA_MAC_VER_44:
        TRY(ephy_init(io, b8411, sizeof(b8411) / sizeof(*b8411)));
        fix_8411(io);
        break;
    default: return -1;
    }
    mac_modify(io, 0xc0ac, 0, 0x1f80);
    io->write16(0xe2, 0); /* no interrupt coalescing */
    return rtl8168_phy_init(ver, io);
}
#undef TRY
