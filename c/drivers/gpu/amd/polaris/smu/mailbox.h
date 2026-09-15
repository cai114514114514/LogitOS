/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGITOS_POLARIS_SMU_MAILBOX_H
#define LOGITOS_POLARIS_SMU_MAILBOX_H

#include <stdint.h>

/* Logical protocol slots, deliberately NOT MMIO offsets. No product adapter
 * supplies these callbacks. The earlier "RX path remains read-only" note
 * described this test-only model's first version; production SMU boot now
 * lives in loader.c, with its own register and ownership checks. */
enum polaris_smu_slot {
    POLARIS_SMU_RESPONSE, POLARIS_SMU_ARGUMENT, POLARIS_SMU_MESSAGE
};
struct polaris_smu_io {
    void *opaque;
    int (*read)(void *, enum polaris_smu_slot, uint32_t *);
    int (*write)(void *, enum polaris_smu_slot, uint32_t);
    uint64_t (*now_us)(void *);
};
struct polaris_smu_budget {
    uint64_t timeout_us; /* 1..1,000,000, including preflight and completion. */
    uint32_t max_polls;  /* 1..100,000 across ALL response reads. */
};
enum polaris_smu_result {
    POLARIS_SMU_OK = 0,
    POLARIS_SMU_BAD_ARGUMENT = -1,
    POLARIS_SMU_BUSY = -2,
    POLARIS_SMU_TIMEOUT = -3,
    POLARIS_SMU_IO_ERROR = -4,
    POLARIS_SMU_PRIOR_ERROR = -5,
    POLARIS_SMU_UNSUPPORTED = -6,
    POLARIS_SMU_REJECTED = -7,
    POLARIS_SMU_STALE_RESPONSE = -8,
    POLARIS_SMU_BAD_CLOCK = -9,
    POLARIS_SMU_QUARANTINED = -10
};
struct polaris_smu_observation {
    uint32_t raw_response, polls, writes;
    uint8_t submitted, quarantined;
};

/* Zero-initialize once before publication, never memset/reinitialize a live
 * context. One context must own one physical mailbox across all callers.
 * No reset API exists: after an uncertain mutating transaction, only a future
 * independently verified hardware reset may justify creating a new context. */
struct polaris_smu_mailbox {
    unsigned lock;
    unsigned quarantined;
};

/* Bounded transport of SMU7 Test (0x100), with argument explicitly zeroed.
 * Test is NOT promised to be a harmless ping: upstream uses argument 0x20000
 * during protected SMU startup. This function models the wire transaction;
 * success means only a fresh response of 1, never firmware-loaded/GPU-ready.
 * Callbacks must finish promptly and implement ordered device accesses (a
 * RESPONSE read flushes preceding posted writes); software budgets cannot
 * interrupt a callback which itself blocks. ctx/io/budget/out must be mutually
 * disjoint objects: clearing out must not overwrite callbacks or their budget.
 * BUSY does not touch out or perform any callback. All other results populate
 * out; submitted distinguishes an attempted MESSAGE write from pre-send
 * failure, and quarantined forbids ALL later I/O, even if late ACK arrives. */
int polaris_smu_test(struct polaris_smu_mailbox *,
                     const struct polaris_smu_io *,
                     const struct polaris_smu_budget *,
                     struct polaris_smu_observation *);

#endif
