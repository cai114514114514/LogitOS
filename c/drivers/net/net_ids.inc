/* PCI match tables for the NIC drivers, in device-model form
 * (struct dev_match, c/drivers/core/driver.h).
 *
 * They live in one included file rather than beside each driver so that the
 * host unit test (tests/unit/net_drv_test.c) can read the SAME tables the
 * kernel binds from -- a match table checked only against a copy of itself
 * checks nothing. netdev.c is the only kernel file that includes it.
 *
 * All of these are vendor:device rows. Class matching (DEV_MATCH_CLASS with
 * PCI_CLASS_NETWORK) is deliberately NOT used: "any Ethernet controller" would
 * bind a card whose registers we do not know, and a NIC that enumerates and
 * then silently receives nothing is far harder to diagnose than a NIC that is
 * honestly unclaimed. `dev_dump()` already lists the unclaimed card by class,
 * which is the useful half of what class matching would have given us.
 */

/* virtio-net. 0x1000 is the transitional device QEMU's `virtio-net-pci`
 * presents by default; 0x1041 is the modern (1.0-only) one. Both expose the
 * modern capability structures, which is all our transport uses. */
static const struct dev_match virtio_net_ids[] = {
    DEV_MATCH_VD(0x1AF4, 0x1041),
    DEV_MATCH_VD(0x1AF4, 0x1000),
    DEV_MATCH_END
};

/* Intel 8254x. The first three are what QEMU emulates (`e1000`,
 * `e1000-82544gc`, `e1000-82545em`) and are the only ones verified on device;
 * the rest are the same legacy-descriptor generation and share this register
 * layout, listed so a real 8254x card is driven rather than ignored.  Intel's
 * upstream e1000_hw.c classifies 100e/1015/1016/1017/101e as the SAME
 * e1000_82540 MAC type; those four variants are therefore admitted here rather
 * than inferred from a similar product name.  The source used for that
 * boundary is Linux v6.11 drivers/net/ethernet/intel/e1000/e1000_hw.c.
 * Deliberately NOT here: 82574L / e1000e (0x10D3) and the igb family -- they
 * are a different init sequence, and binding them with this driver would give
 * a card that enumerates and then receives nothing.  82547 is also absent:
 * its half-duplex TX FIFO stall workaround is not implemented in e1000.c. */
static const struct dev_match e1000_ids[] = {
    DEV_MATCH_VD(0x8086, 0x100E),   /* 82540EM  -- QEMU default */
#ifndef NET_IDS_NEGCTL_DROP_82540_VARIANTS
    DEV_MATCH_VD(0x8086, 0x1015),   /* 82540EM LOM */
    DEV_MATCH_VD(0x8086, 0x1016),   /* 82540EP LOM */
    DEV_MATCH_VD(0x8086, 0x1017),   /* 82540EP */
    DEV_MATCH_VD(0x8086, 0x101E),   /* 82540EP low-profile */
#endif
    DEV_MATCH_VD(0x8086, 0x1004),   /* 82544GC  -- QEMU */
    DEV_MATCH_VD(0x8086, 0x100F),   /* 82545EM  -- QEMU */
    DEV_MATCH_VD(0x8086, 0x1010),   /* 82546EB dual-port */
    DEV_MATCH_VD(0x8086, 0x1026),   /* 82545GM */
    DEV_MATCH_VD(0x8086, 0x1076),   /* 82541GI */
    DEV_MATCH_VD(0x8086, 0x1077),   /* 82541GI/PI */
    DEV_MATCH_END
};

/* Realtek RTL8139 and its very common relabels. QEMU emulates `rtl8139`. */
static const struct dev_match rtl8139_ids[] = {
    DEV_MATCH_VD(0x10EC, 0x8139),
    DEV_MATCH_VD(0x10EC, 0x8138),   /* RTL8139B in CardBus dress */
    DEV_MATCH_VD(0x1113, 0x1211),   /* SMC1211TX / Accton relabel */
    DEV_MATCH_END
};

/* Realtek gigabit: RTL8169 and the RTL8168/8111 onboard part that succeeded it,
 * plus the RTL8101/8102 fast-ethernet sibling that speaks the same descriptor
 * protocol. NOTE: QEMU has no rtl8169 device model at all (`-device help`
 * lists rtl8139 and nothing else Realtek), so this driver has NEVER been run
 * against a device -- only its ring arithmetic is tested, on the host. */
static const struct dev_match rtl8169_ids[] = {
    DEV_MATCH_VD(0x10EC, 0x8169),   /* RTL8169   */
    DEV_MATCH_VD(0x10EC, 0x8168),   /* RTL8168/8111 */
    DEV_MATCH_VD(0x10EC, 0x8161),   /* RTL8168 variant */
    DEV_MATCH_VD(0x10EC, 0x8136),   /* RTL8101/8102 */
    DEV_MATCH_VD(0x1186, 0x4300),   /* D-Link DGE-528T (8169 inside) */
    DEV_MATCH_END
};

/* AMD PCnet PCI ID family; probe verifies Am79C970A chip version.
 * Style-2 32-bit DMA descriptors. This is a separate controller
 * protocol, not an e1000 alias. QEMU exposes this ID with -device pcnet. */
static const struct dev_match pcnet_ids[] = {
    DEV_MATCH_VD(0x1022, 0x2000),
    DEV_MATCH_END
};

/* The 82574 family has its own PCIe/NVM/PHY initialization in e1000e.c.
 * These IDs do not imply support for ICH/PCH integrated LAN or igb/igc. */
static const struct dev_match e1000e_ids[] = {
    DEV_MATCH_VD(0x8086, 0x10D3), /* 82574L/IT: QEMU e1000e also models this */
    DEV_MATCH_VD(0x8086, 0x10F6), /* 82574LA: physical-hardware validation pending */
    DEV_MATCH_END
};
/* PCH2 + 82579 only. A match is eligibility for the guarded probe, not proof
 * of ME coexistence or a physical link. Later I217/I219 have other contracts. */
static const struct dev_match e1000_pch2_ids[] = {
    DEV_MATCH_VD(0x8086, 0x1502), /* 82579LM */
    DEV_MATCH_VD(0x8086, 0x1503), /* 82579V */
    DEV_MATCH_END,
};

/* The current broad-hardware boundary is intentional, not a missing ID sweep:
 *
 * - I217/I218/I219 use several LPT/SPT/CNP/TGP/ADP/MTP PCH profiles in
 *   upstream e1000e, rather than the PCH2 sequence above.
 * - I225/I226 use the igc I225 MAC/PHY/NVM contract.
 * - RTL8125 uses 32-bit interrupt status/mask registers and a 16-bit TX
 *   doorbell at different offsets, plus revision-specific OCP/EPHY setup and
 *   optional firmware; rtl8169.c uses the earlier 16/16/8-bit interface.
 *
 * Adding their PCI IDs to an existing table would make an unsupported card
 * look claimed while programming the wrong registers.  Keep them unmatched
 * until their protocol backends and rejection controls land together. */
