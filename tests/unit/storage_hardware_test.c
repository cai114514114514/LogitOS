/* SPDX-License-Identifier: MIT */
/* Actual production discovery/format/handoff functions; only MMIO/IDENTIFY
 * delivery is a fixture. This is protocol evidence, not physical-PC testing. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include "driver.h"
#include "pci.h"
#include "storage_hardware_types.inc"
static int checks, failures;
#define CHECK(x, label) do { ++checks; if (!(x)) { ++failures; printf("FAIL: %s\n", label); } } while (0)
static struct device devices[4];
static uint16_t identify[256];
_Alignas(8) static uint8_t registers[0x1200];
static int handoff_stuck, handoff_waits;
static uint16_t pci_command, pci_drop_mask;
static unsigned pci_command_reads, pci_command_writes, bar_maps, abar_reads, abar_writes;
static unsigned pci_fake_read_number, pci_ignore_write_number;
static unsigned bme_before_handoff, bme_with_running_port, port_access_before_handoff;
static unsigned abar_ops_at_bme;
static int handoff_complete, fail_stop_armed, fail_stops;
static jmp_buf fail_stop_env;
struct ahci_port { int index, lba48; uint64_t nsectors; char model[41]; };
void kprintf(const char *fmt, ...) { (void)fmt; }
__attribute__((noreturn)) void panic(const char *fmt, ...)
{
    (void)fmt; ++fail_stops;
    if (fail_stop_armed) longjmp(fail_stop_env, 1);
    __builtin_trap();
}
struct device *dev_find_class(uint8_t cls, uint8_t sub, struct device *from)
{
    unsigned start = from ? (unsigned)(from - devices + 1) : 0;
    for (unsigned i = start; i < 4; ++i)
        if (devices[i].class_code == cls && devices[i].subclass == sub) return &devices[i];
    return NULL;
}
uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off)
{
    (void)bus; (void)slot; (void)func;
    if (off != PCI_CFG_COMMAND) return UINT16_MAX;
    ++pci_command_reads;
    if (pci_fake_read_number == pci_command_reads) return UINT16_MAX;
    return pci_command;
}
void pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off,
                     uint16_t value)
{
    (void)bus; (void)slot; (void)func;
    if (off == PCI_CFG_COMMAND) {
        ++pci_command_writes;
        if (pci_ignore_write_number == pci_command_writes) return;
        uint16_t next = (uint16_t)((value & ~pci_drop_mask) |
                                   (pci_command & pci_drop_mask));
        if (next & PCI_CMD_MASTER) {
            if (!handoff_complete) ++bme_before_handoff;
            uint32_t port_cmd = *(uint32_t *)(void *)(registers + 0x100 + P_CMD);
            if (port_cmd & (CMD_ST | CMD_FRE | CMD_CR | CMD_FR))
                ++bme_with_running_port;
            abar_ops_at_bme = abar_reads + abar_writes;
        }
        pci_command = next;
    }
}
uint64_t dev_bar_map(struct device *dev, int idx)
{
    (void)dev; (void)idx; ++bar_maps;
    return (uint64_t)(uintptr_t)registers;
}
static uint32_t r32(volatile uint8_t *b, int off)
{
    uintptr_t where = (uintptr_t)(b + off) - (uintptr_t)registers;
    ++abar_reads;
    if (where >= 0x100 && !handoff_complete) ++port_access_before_handoff;
    return *(volatile uint32_t *)(b + off);
}
static void w32(volatile uint8_t *b, int off, uint32_t v)
{
    uintptr_t where = (uintptr_t)(b + off) - (uintptr_t)registers;
    ++abar_writes;
    if (where >= 0x100 && !handoff_complete) ++port_access_before_handoff;
    *(volatile uint32_t *)(b + off) = v;
}
static int wait_clear(volatile uint8_t *base, int off, uint32_t mask, long spins, uint32_t ms)
{
    (void)spins; (void)ms; ++handoff_waits;
    if (handoff_stuck) return -1;
    w32(base, off, r32(base, off) & ~mask);
    if (base == registers && off == HBA_BOHC) handoff_complete = 1;
    return 0;
}
static int ahci_run(struct ahci_port *p, const struct ahci_cmdspec *s)
{ (void)p; memcpy(s->buf, identify, sizeof identify); return 0; }
#include "storage_hardware_functions.inc"

static void reset_ahci_fixture(void)
{
    memset(registers, 0, sizeof registers);
    memset(g_ahci_quarantine, 0, sizeof g_ahci_quarantine);
    g_ahci_quarantine_count = 0;
    pci_command = pci_drop_mask = 0;
    pci_command_reads = pci_command_writes = bar_maps = 0;
    abar_reads = abar_writes = 0;
    pci_fake_read_number = pci_ignore_write_number = 0;
    bme_before_handoff = bme_with_running_port = 0;
    port_access_before_handoff = abar_ops_at_bme = 0;
    handoff_complete = handoff_stuck = handoff_waits = 0;
    fail_stop_armed = fail_stops = 0;
}

static void set_running_firmware_port(void)
{
    *(uint32_t *)(void *)(registers + HBA_CAP2) = 1;
    *(uint32_t *)(void *)(registers + HBA_BOHC) = BOHC_BOS | BOHC_BB;
    *(uint32_t *)(void *)(registers + HBA_CAP) = 0; /* NP=0: one port */
    *(uint32_t *)(void *)(registers + HBA_PI) = 1;
    *(uint32_t *)(void *)(registers + HBA_VS) = 0x00010300;
    *(uint32_t *)(void *)(registers + 0x100 + P_CMD) =
        CMD_ST | CMD_FRE | CMD_CR | CMD_FR;
}

int main(void)
{
    devices[0] = (struct device){.bus_type=DEV_BUS_PCI,.class_code=1,.subclass=8,.prog_if=1};
    devices[1] = (struct device){.bus_type=DEV_BUS_PCI,.class_code=1,.subclass=8,.prog_if=2,.bus=3,.slot=7,.func=4};
    CHECK(nvme_find() == &devices[1], "NVMe finds bridge-bus multifunction endpoint");
    devices[1].drv=(void *)1;
    CHECK(nvme_find() == NULL, "NVMe does not steal a bound controller");
    devices[1].drv=NULL;
    uint64_t cap = 63 | (UINT64_C(1) << 37);
    CHECK(nvme_controller_ioqs(cap, 8192)==4, "NVMe four queue doorbells fit ordinary BAR");
    CHECK(nvme_controller_ioqs(cap, 4112)==1, "NVMe only creates queues inside small BAR");
    CHECK(nvme_controller_ioqs(cap, 4111)==0, "NVMe rejects truncated first CQ doorbell");
    CHECK(nvme_controller_ioqs(cap | (UINT64_C(8)<<32), 8192)==1, "NVMe honors nonzero doorbell stride");
    CHECK(nvme_controller_ioqs(cap | (UINT64_C(1)<<48), 8192)==0, "NVMe refuses unsupported minimum page size");
    CHECK(nvme_controller_ioqs(63,8192)==0, "NVMe requires NVM command set");
    CHECK(nvme_controller_ioqs(cap-1,8192)==0, "NVMe requires queue depth capacity");
    CHECK(nvme_granted_ioqs(0x00030000,4)==1, "NVMe honors controller SQ queue grant");
    CHECK(nvme_granted_ioqs(0x00000003,4)==1, "NVMe honors controller CQ queue grant");
    CHECK(nvme_granted_ioqs(0xffffffff,4)==4, "NVMe queue grant remains bounded by BAR and local capacity");
    CHECK(nvme_granted_ioqs(0x00030003,1)==1, "NVMe queue grant does not exceed requested count");
    uint8_t ns[4096]={0}; uint64_t sectors=0; uint32_t lba=0;
    ns[1]=0x10; ns[130]=9;
    CHECK(!nvme_namespace_format(ns,&sectors,&lba)&&sectors==4096&&lba==512, "NVMe valid namespace keeps hardware capacity");
    ns[130]=12; CHECK(!nvme_namespace_format(ns,&sectors,&lba)&&sectors==32768&&lba==4096, "NVMe converts native 4Kn capacity to 512B sectors");
    ns[130]=255; CHECK(nvme_namespace_format(ns,&sectors,&lba)<0, "NVMe invalid LBADS cannot invoke undefined shift");
    ns[130]=9;ns[128]=8;CHECK(nvme_namespace_format(ns,&sectors,&lba)<0, "NVMe metadata needs an explicit data path");
    ns[128]=0;ns[29]=1;CHECK(nvme_namespace_format(ns,&sectors,&lba)<0, "NVMe protection information cannot be silently omitted");
    ns[29]=0;ns[26]=1;CHECK(nvme_namespace_format(ns,&sectors,&lba)<0, "NVMe selected format must be advertised");
    ns[25]=16;ns[26]=0x20;ns[128+16*4+2]=9;
    CHECK(!nvme_namespace_format(ns,&sectors,&lba), "NVMe extended FLBAS index decodes format 16");
    ns[128+16*4]=8;CHECK(nvme_namespace_format(ns,&sectors,&lba)<0, "NVMe metadata validation follows extended format");
    memset(ns,0,8);CHECK(nvme_namespace_format(ns,&sectors,&lba)<0, "NVMe empty namespace refused");

    devices[2] = (struct device){
        .bus_type=DEV_BUS_PCI,.class_code=1,.subclass=6,.prog_if=1,
        .bus=0,.slot=31,.func=2,.vendor=0x8086,.device=0x1d02
    };
    struct ahci_hba hba={0};
    reset_ahci_fixture();
    CHECK(!ahci_find(&hba,0) &&
          hba.abar==(uint64_t)(uintptr_t)registers &&
          (pci_command&PCI_CMD_MEM) && bar_maps==1,
          "AHCI confirms MEM decode before BAR mapping");

    reset_ahci_fixture();
    hba=(struct ahci_hba){0};pci_drop_mask=PCI_CMD_MEM;
    CHECK(!ahci_find(&hba,0) && !hba.abar && !pci_command && !bar_maps &&
          hba.quarantined,
          "AHCI refuses MEM-decode drop before BAR mapping");

    /* Start from the dangerous firmware state: BOHC owned, an old CLB/FIS
     * engine running, and BME originally clear. The production transaction
     * must hand ownership over, disable/confirm BME, stop/confirm the port, and
     * only then perform the first BME=1 write. */
    reset_ahci_fixture();
    set_running_firmware_port();
    hba=(struct ahci_hba){0};
    uint32_t hcap=0,hpi=0,hvs=0;
    int prep = ahci_find(&hba,0) ||
        ahci_prepare_controller(&hba,registers,&hcap,&hpi,&hvs);
    CHECK(!prep && handoff_complete && !bme_before_handoff &&
          !bme_with_running_port && !port_access_before_handoff &&
          (pci_command&(PCI_CMD_MEM|PCI_CMD_MASTER))==
              (PCI_CMD_MEM|PCI_CMD_MASTER) &&
          (*(uint32_t *)(void *)(registers+0x100+P_CMD)&
              (CMD_ST|CMD_FRE|CMD_CR|CMD_FR))==0,
          "AHCI enables BME only after BOHC and firmware-port stop");

    /* Firmware may legitimately leave BME set while it owns the controller.
     * We preserve that state until BOHC, then must confirm BME clear before the
     * first port access. An ignored clear leaves old DMA live and is fatal. */
    reset_ahci_fixture();
    set_running_firmware_port();
    pci_command=PCI_CMD_MASTER;
    hba=(struct ahci_hba){0};
    pci_ignore_write_number=2; /* ignore post-handoff BME clear */
    fail_stop_armed=1;
    int firmware_bme_stopped=0;
    if (setjmp(fail_stop_env) == 0) {
        if (!ahci_find(&hba,0))
            (void)ahci_prepare_controller(&hba,registers,&hcap,&hpi,&hvs);
    } else firmware_bme_stopped=1;
    fail_stop_armed=0;
    CHECK(firmware_bme_stopped && fail_stops==1 &&
          (pci_command&(PCI_CMD_MEM|PCI_CMD_MASTER))==
              (PCI_CMD_MEM|PCI_CMD_MASTER) &&
          (*(uint32_t *)(void *)(registers+0x100+P_CMD)&
              (CMD_ST|CMD_FRE|CMD_CR|CMD_FR))==
              (CMD_ST|CMD_FRE|CMD_CR|CMD_FR),
          "AHCI fail-stops if firmware BME cannot be disabled after handoff");

    /* A controller that cannot latch BME is left in the confirmed safe Command
     * state and permanently skipped on a later discovery pass. */
    reset_ahci_fixture();
    set_running_firmware_port();
    hba=(struct ahci_hba){0};pci_drop_mask=PCI_CMD_MASTER;
    prep = ahci_find(&hba,0) ||
        ahci_prepare_controller(&hba,registers,&hcap,&hpi,&hvs);
    unsigned reads_before_reprobe=pci_command_reads;
    unsigned writes_before_reprobe=pci_command_writes;
    unsigned maps_before_reprobe=bar_maps;
    struct ahci_hba reprobe={0};
    int reprobe_rc=ahci_find(&reprobe,0);
    CHECK(prep && hba.quarantined && pci_command==PCI_CMD_MEM &&
          !bme_before_handoff && !bme_with_running_port &&
          !reprobe_rc && reprobe.quarantined && !reprobe.abar &&
          pci_command_reads==reads_before_reprobe &&
          pci_command_writes==writes_before_reprobe &&
          bar_maps==maps_before_reprobe,
          "AHCI refuses bus-master drop after quiescing firmware ports");

    /* The discovery write itself can partially succeed even when its readback
     * is poisoned. If restoration is ignored, do not publish/map anything and
     * never return to continue probing. */
    reset_ahci_fixture();
    hba=(struct ahci_hba){0};
    pci_fake_read_number=2;pci_ignore_write_number=2;
    fail_stop_armed=1;
    int restore_stopped=0;
    if (setjmp(fail_stop_env) == 0) (void)ahci_find(&hba,0);
    else restore_stopped=1;
    fail_stop_armed=0;
    CHECK(restore_stopped && fail_stops==1 && (pci_command&PCI_CMD_MEM) &&
          !bar_maps && !abar_reads && !abar_writes,
          "AHCI fail-stops when partial Command enable cannot be restored");

    /* Also fail-stop when BME reached hardware but the failed-enable cleanup
     * cannot prove it clear. The last BME write happens after the port stop and
     * no ABAR access is allowed after isolation becomes uncertain. */
    reset_ahci_fixture();
    set_running_firmware_port();
    hba=(struct ahci_hba){0};
    pci_fake_read_number=5;pci_ignore_write_number=4;
    fail_stop_armed=1;
    int bme_stopped=0;
    if (setjmp(fail_stop_env) == 0) {
        if (!ahci_find(&hba,0))
            (void)ahci_prepare_controller(&hba,registers,&hcap,&hpi,&hvs);
    } else bme_stopped=1;
    fail_stop_armed=0;
    CHECK(bme_stopped && fail_stops==1 &&
          (pci_command&(PCI_CMD_MEM|PCI_CMD_MASTER))==
              (PCI_CMD_MEM|PCI_CMD_MASTER) &&
          abar_ops_at_bme==abar_reads+abar_writes,
          "AHCI fail-stops if failed BME enable cannot be isolated");

    struct ahci_port port={0}; identify[60]=2048;
    CHECK(!port_identify(&port)&&port.nsectors==2048, "AHCI legacy 512B identify accepted");
    identify[106]=0x6003;
    CHECK(!port_identify(&port), "AHCI 512e accepted despite 4KiB physical grouping");
    identify[106]=0x5000;identify[117]=2048;
    CHECK(port_identify(&port)<0, "AHCI rejects 4Kn before registering a disk");
    identify[117]=0;CHECK(port_identify(&port)<0, "AHCI rejects invalid zero logical-sector size");
    identify[106]=0x9000;identify[117]=2048;
    CHECK(!port_identify(&port), "AHCI invalid word106 uses legacy sector semantics");
    w32(registers,HBA_CAP2,0);handoff_waits=0;
    CHECK(!ahci_bios_handoff(registers)&&!handoff_waits, "AHCI absent handoff capability needs no BIOS wait");
    w32(registers,HBA_CAP2,1);w32(registers,HBA_BOHC,BOHC_BOS|BOHC_BB);handoff_stuck=0;
    CHECK(!ahci_bios_handoff(registers)&&(r32(registers,HBA_BOHC)&BOHC_OOS), "AHCI requests OS ownership and awaits firmware release");
    w32(registers,HBA_BOHC,BOHC_BOS|BOHC_BB);handoff_stuck=1;
    CHECK(ahci_bios_handoff(registers)<0, "AHCI refuses controller while firmware retains ownership");
    printf("storage hardware: %d checks, %d failed\n",checks,failures);
    return failures?1:0;
}
