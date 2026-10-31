#ifndef LOGIT_RTL8168_H
#define LOGIT_RTL8168_H
#include <stdint.h>

enum rtl_mac_ver {
    RTL_MAC_UNKNOWN = 0,
    RTL_GIGA_MAC_VER_40 = 40,
    RTL_GIGA_MAC_VER_42 = 42,
    RTL_GIGA_MAC_VER_44 = 44,
};
enum rtl_mac_ver rtl_identify(uint32_t txconfig);

/* Initialization only: packet I/O stays on the driver's direct MMIO path.
 * Keeping indirect-register protocols here lets the host model observe the
 * same commands that the device receives, including completion timeouts. */
struct rtl_io {
    uint8_t (*read8)(uint32_t);
    uint32_t (*read32)(uint32_t);
    void (*write8)(uint32_t, uint8_t);
    void (*write16)(uint32_t, uint16_t);
    void (*write32)(uint32_t, uint32_t);
    void (*delay)(unsigned usec);
};
int rtl8168_start(enum rtl_mac_ver ver, const struct rtl_io *io, uint8_t function);
int rtl8168_prepare(const struct rtl_io *io);
int rtl8168_phy_init(enum rtl_mac_ver ver, const struct rtl_io *io);
int rtl8168_phy_read(const struct rtl_io *io, uint16_t reg);
int rtl8168_phy_write(const struct rtl_io *io, uint16_t reg, uint16_t value);
int rtl8168_wait(const struct rtl_io *io, uint32_t reg, uint32_t mask,
                 int high, unsigned delay, unsigned attempts);
#endif
