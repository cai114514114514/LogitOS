#ifndef LOGIT_POLARIS_PROBE_H
#define LOGIT_POLARIS_PROBE_H
#include <stdint.h>
struct device;

/* 67df spans several Polaris boards; PCI identity does not establish the
 * marketing model, shader count or VRAM size of an RX 580/2048SP board. */
#define POLARIS10_PCI_DEVICE 0x67dfu
enum polaris_probe_result {
    POLARIS_PROBE_NONE, POLARIS_PROBE_OBSERVED, POLARIS_PROBE_ID,
    POLARIS_PROBE_POWER, POLARIS_PROBE_DECODE, POLARIS_PROBE_BAR,
    POLARIS_PROBE_SURFACE, POLARIS_PROBE_MAP, POLARIS_PROBE_READ,
};
struct polaris_info {
    enum polaris_probe_result result;
    uint64_t vram_bytes, cpu_aperture_bytes;
    uint64_t mc_base, mc_end;
    uint32_t mc_location, srbm_status2, vm_context0;
    uint32_t sdma_f32[2], sdma_ring[2];
    uint32_t mmio_reads;
    uint8_t layout_covers_vram;
};
struct polaris_probe_ops {
    void *ctx;
    uint64_t (*map_bar)(void *, int);
    uint32_t (*read32)(void *, uint64_t, uint32_t);
};
/* Read-only bring-up snapshot, not engine readiness. There is deliberately no
 * write/DMA/submission callback. A result of 0 must NEVER install a presenter.
 * command/pmcsr are freshly read PCI config; malformed PM caps use pmcsr=3. */
int polaris_probe_readonly(const struct device *, uint16_t command,
                          uint16_t pmcsr, uint64_t lfb, uint64_t lfb_bytes,
                          uint32_t width, uint32_t height,
                          const struct polaris_probe_ops *, struct polaris_info *);
const char *polaris_probe_result_name(enum polaris_probe_result);
#endif
