#include "rtl8168.h"

/* Linux v6.12 r8169_main.c:2268-2274, extraction at 5422.
 * Correction: the previous 2134-2140/5103 references were web-renderer line
 * numbers, not source lines. Keep source anchors for the next reader.
 * https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/realtek/r8169_main.c#L2268-L2274
 * A PCI 8168 ID/revision does not identify its PHY program. Deliberately omit
 * E/H and every other MAC until their own startup path is implemented. */
enum rtl_mac_ver rtl_identify(uint32_t txconfig)
{
    uint32_t xid = (txconfig >> 20) & 0xfcf;
    switch (xid & 0x7cf) {
    case 0x4c0: return RTL_GIGA_MAC_VER_40;
    case 0x509: return RTL_GIGA_MAC_VER_42;
    case 0x5c8: return RTL_GIGA_MAC_VER_44;
    default: return RTL_MAC_UNKNOWN;
    }
}
