/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGITOS_POLARIS_SDMA_ENGINE_H
#define LOGITOS_POLARIS_SDMA_ENGINE_H
#include "amd/polaris/sdma/queue.h"
#include "amd/polaris/smu/loader.h"

struct polaris_sdma_engine_ops {
    struct polaris_sdma_queue_ops queue;
    int (*write_reg)(void *, uint32_t byte_offset, uint32_t);
    /* Read actual SMU SRAM through the owned indirect port, not cached loader
     * state. Address is a byte address; this read must include posted flush. */
    int (*read_smc_word)(void *, uint32_t byte_address, uint32_t *);
};
struct polaris_sdma_engine {
    unsigned lock, quarantined, configured;
    uint32_t register_writes, load_status, polls;
};
enum polaris_sdma_engine_result {
    POLARIS_SDMA_ENGINE_OK = 0,
    POLARIS_SDMA_ENGINE_INVALID = -1,
    POLARIS_SDMA_ENGINE_BUSY = -2,
    POLARIS_SDMA_ENGINE_PREREQUISITE = -3,
    POLARIS_SDMA_ENGINE_IO = -4,
    POLARIS_SDMA_ENGINE_TIMEOUT = -5,
    POLARIS_SDMA_ENGINE_QUARANTINED = -6
};
/* SDMA0/VMID0 direct-VRAM setup after successful polaris_smu_load, checked
 * again against its exact fresh SRAM load mask and soft-register pointer.
 * Only an idle engine with no active RLC queues may be taken over. First
 * mutation disables RB/IB, then HALT is read back and idle is bounded before
 * clearing owned ring/fence memory and programming read-back-verified config.
 * MC_FB_LOCATION and CONFIG_MEMSIZE must actually cover both mappings; this
 * initial path does not set up system-memory GART or change display mappings.
 * The final queue_attach checks the resulting hardware state. Actual execution
 * still requires queue copy/fill canaries; success here alone is not speed or
 * acceleration evidence. Engine and queue must be zero-initialized once and
 * share exclusive device ownership with loader/ops, including indirect ports.
 * All descriptors/mapped allocations are disjoint and live across failures.
 * Any uncertain mutation sticks quarantine in both contexts; no reset retry.
 * Callback operations are bounded; startup is bounded by 100 ms/100000 polls.
 */
int polaris_sdma_engine_start(struct polaris_sdma_engine *,
                              const struct polaris_sdma_engine_ops *,
                              const struct polaris_smu_loader *,
                              const struct polaris_sdma_mapping *ring,
                              const struct polaris_sdma_mapping *fence,
                              struct polaris_sdma_queue *);
#endif
