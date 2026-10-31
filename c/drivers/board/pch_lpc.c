/* Intel PCH LPC bridge: TCO watchdog, Super I/O identification, CF9 reset.
 *
 * Register facts are from the Intel 6 Series/C200 Series Chipset datasheet
 * (document 324645): LPC config ACPIBASE 0x40, ACPI_CNTL 0x44, LPC_EN 0x82
 * (section 13.1); TCO registers at ACPIBASE + 0x60 (13.9); RST_CNT at 0xCF9
 * (13.7.5). The C600 (X79) and 7 Series PCHs keep the same layout, as does the
 * ICH9 that QEMU's q35 machine models, which is why one table covers them.
 * The device ID ranges match Linux drivers/mfd/lpc_ich.c. */
#include <stddef.h>
#include "pch_lpc.h"
#include "board_io.h"

#define LPC_SLOT 0x1f
#define LPC_ACPIBASE  0x40
#define LPC_ACPI_CNTL 0x44
#define  ACPI_CNTL_EN 0x80
#define LPC_EN        0x82
#define  LPC_EN_CNF1  0x1000    /* forward 0x2e/0x2f */
#define  LPC_EN_CNF2  0x2000    /* forward 0x4e/0x4f */

#define TCO_OFFSET    0x60
#define TCO1_STS      0x04
#define TCO2_STS      0x06
#define  TCO2_SECOND_TO 0x0002
#define TCO1_CNT      0x08
#define  TCO_TMR_HLT  0x0800
#define TCO_TMR       0x12

static const struct { uint16_t lo, hi; const char *family; } lpc_ids[] = {
    { 0x2910, 0x2919, "ICH9" },
    { 0x1c40, 0x1c5f, "6 Series/C200" },
    { 0x1d40, 0x1d41, "C600/X79" },
    { 0x1e40, 0x1e5f, "7 Series/C216" },
};

int pch_lpc_find(struct pch_lpc *out)
{
    out->device = 0; out->acpi_base = 0; out->lpc_en = 0; out->family = NULL;
    uint32_t id = bio_cfg32(LPC_SLOT, 0, 0x00);
    if ((id & 0xffff) != 0x8086) return -1;
    uint16_t dev = (uint16_t)(id >> 16);
    for (size_t i = 0; i < sizeof lpc_ids / sizeof lpc_ids[0]; i++)
        if (dev >= lpc_ids[i].lo && dev <= lpc_ids[i].hi) {
            out->device = dev;
            out->family = lpc_ids[i].family;
        }
    if (!out->device) return -1;
    if (bio_cfg8(LPC_SLOT, 0, LPC_ACPI_CNTL) & ACPI_CNTL_EN)
        out->acpi_base = bio_cfg16(LPC_SLOT, 0, LPC_ACPIBASE) & 0xff80;
    out->lpc_en = bio_cfg16(LPC_SLOT, 0, LPC_EN);
    return 0;
}

void tco_quiesce(const struct pch_lpc *lpc, struct tco_report *out)
{
    out->state = TCO_ABSENT; out->tcobase = 0; out->ticks = 0; out->second_timeout = 0;
    if (!lpc->device || !lpc->acpi_base) return;
    uint16_t base = (uint16_t)(lpc->acpi_base + TCO_OFFSET);
    out->tcobase = base;
    out->ticks = bio_inw(base + TCO_TMR) & 0x3ff;
    out->second_timeout = (bio_inw(base + TCO2_STS) & TCO2_SECOND_TO) != 0;
    uint16_t cnt = bio_inw(base + TCO1_CNT);
    if (cnt & TCO_TMR_HLT) { out->state = TCO_HALTED; return; }
    /* read-modify-write, exactly iTCO_wdt_stop(): the other TCO1_CNT bits
     * (NMI2SMI_EN, TCO_LOCK) belong to firmware and are written back as read */
#ifndef BOARD_NEGCTL_TCO_NO_HALT
    bio_outw(base + TCO1_CNT, cnt | TCO_TMR_HLT);
#endif
    out->state = (bio_inw(base + TCO1_CNT) & TCO_TMR_HLT) ? TCO_WAS_RUNNING : TCO_STUCK;
}

/* ---- Super I/O --------------------------------------------------------- */

static const struct { uint16_t id, mask; const char *name; } nuvoton_ids[] = {
    /* Linux drivers/hwmon/nct6775-platform.c (SIO_ID_MASK 0xfff8) and
     * w83627ehf.c (0xfff0) */
    { 0xb470, 0xfff8, "NCT6775" }, { 0xc330, 0xfff8, "NCT6776" },
    { 0xc560, 0xfff8, "NCT6779" }, { 0xc800, 0xfff8, "NCT6791" },
    { 0xc910, 0xfff8, "NCT6792" }, { 0xd120, 0xfff8, "NCT6793" },
    { 0xd350, 0xfff8, "NCT6795" }, { 0xd420, 0xfff8, "NCT6796" },
    { 0xd428, 0xfff8, "NCT6798" },
    { 0xa020, 0xfff0, "W83627DHG" }, { 0xa510, 0xfff0, "W83667HG" },
    { 0xb350, 0xfff0, "W83667HG-B" },
};
static const struct { uint16_t id; const char *name; } ite_ids[] = {
    /* Linux drivers/hwmon/it87.c */
    { 0x8603, "IT8603E" }, { 0x8620, "IT8620E" }, { 0x8628, "IT8628E" },
    { 0x8728, "IT8728F" }, { 0x8772, "IT8772E" }, { 0x8786, "IT8786E" },
};
static const struct { uint16_t id; const char *name; } fintek_ids[] = {
    /* Linux drivers/hwmon/f71882fg.c */
    { 0x0541, "F71882FG" }, { 0x0723, "F71889FG" }, { 0x0814, "F71869" },
    { 0x0901, "F71808E" },
};

static uint8_t sio_rd(uint16_t port, uint8_t reg) { bio_outb(port, reg); return bio_inb((uint16_t)(port + 1)); }

static int id_valid(uint16_t id) { return id != 0xffff && id != 0x0000; }
static int ite_id(uint16_t id) { return (id >> 8) == 0x86 || (id >> 8) == 0x87; }

/* Leave configuration mode. Nuvoton: 0xaa, then also register 0x02 bit 1 as
 * Linux nct6775 superio_exit() does; ITE: register 0x02 bit 1 only. */
static void exit_nuvoton(uint16_t port) { bio_outb(port, 0xaa); bio_outb(port, 0x02); bio_outb((uint16_t)(port + 1), 0x02); }
static void exit_ite(uint16_t port)     { bio_outb(port, 0x02); bio_outb((uint16_t)(port + 1), 0x02); }

static void name_ite(struct sio_chip *c)
{
    c->vendor = "ITE";
    for (size_t i = 0; i < sizeof ite_ids / sizeof ite_ids[0]; i++)
        if (ite_ids[i].id == c->id) c->name = ite_ids[i].name;
}

static int probe_port(uint16_t port, struct sio_chip *c)
{
    c->port = port; c->name = NULL; c->vendor = NULL;

    /* Nuvoton, Winbond and Fintek all enter with 0x87 0x87 and leave with 0xaa */
    bio_outb(port, 0x87); bio_outb(port, 0x87);
    uint16_t id = (uint16_t)(sio_rd(port, 0x20) << 8 | sio_rd(port, 0x21));
    uint16_t fvendor = (uint16_t)(sio_rd(port, 0x23) << 8 | sio_rd(port, 0x24));
    if (id_valid(id) && ite_id(id)) {
        /* an ITE chip firmware left in configuration mode answers any key */
        exit_ite(port);
        c->id = id; name_ite(c);
        return 1;
    }
    exit_nuvoton(port);
    if (id_valid(id)) {
        c->id = id;
        if (fvendor == 0x1934) {
            c->vendor = "Fintek";
            for (size_t i = 0; i < sizeof fintek_ids / sizeof fintek_ids[0]; i++)
                if (fintek_ids[i].id == id) c->name = fintek_ids[i].name;
        } else {
            c->vendor = "Nuvoton/Winbond";
            for (size_t i = 0; i < sizeof nuvoton_ids / sizeof nuvoton_ids[0]; i++)
                if ((id & nuvoton_ids[i].mask) == nuvoton_ids[i].id) c->name = nuvoton_ids[i].name;
        }
        return 1;
    }

    /* ITE: 0x87 0x01 0x55 then 0x55 (port 0x2e) or 0xaa (port 0x4e);
     * leave by setting bit 1 of configuration register 0x02 */
    bio_outb(port, 0x87); bio_outb(port, 0x01); bio_outb(port, 0x55);
    bio_outb(port, port == 0x2e ? 0x55 : 0xaa);
    id = (uint16_t)(sio_rd(port, 0x20) << 8 | sio_rd(port, 0x21));
    exit_ite(port);
    if (id_valid(id) && ite_id(id)) {
        c->id = id; name_ite(c);
        return 1;
    }
    return 0;
}

int superio_probe(const struct pch_lpc *lpc, struct sio_chip out[SIO_MAX])
{
    int n = 0;
    if (!lpc->device) return 0;
    /* a configuration port the PCH does not forward reaches nothing, and
     * writing unlock sequences into an undecoded cycle is not a probe */
#ifndef BOARD_NEGCTL_SIO_IGNORE_LPC_EN
    if (lpc->lpc_en & LPC_EN_CNF1)
#endif
        n += probe_port(0x2e, &out[n]);
#ifndef BOARD_NEGCTL_SIO_IGNORE_LPC_EN
    if (lpc->lpc_en & LPC_EN_CNF2)
#endif
        n += probe_port(0x4e, &out[n]);
    return n;
}

/* ---- reset ---------------------------------------------------------------- */

#define RST_CNT   0xcf9
#define  SYS_RST  0x02
#define  RST_CPU  0x04
#define  FULL_RST 0x08

void pch_reset(const struct pch_lpc *lpc)
{
    if (!lpc->device) return;
    /* Linux arch/x86/kernel/reboot.c (BOOT_CF9_FORCE): arm SYS_RST first, then
     * the 0->1 edge on RST_CPU starts the reset. FULL_RST asks for a cold one,
     * which also resets the GPU and NIC rather than only the CPU. */
    uint8_t v = bio_inb(RST_CNT) & (uint8_t)~(SYS_RST | RST_CPU | FULL_RST);
    bio_outb(RST_CNT, v | SYS_RST);
    bio_udelay(50);
    bio_outb(RST_CNT, v | SYS_RST | RST_CPU | FULL_RST);
    bio_udelay(50000);
}
