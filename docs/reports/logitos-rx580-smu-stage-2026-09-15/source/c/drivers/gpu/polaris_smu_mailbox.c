/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "polaris_smu_mailbox.h"

/* Protocol facts: Linux v6.12 drivers/gpu/drm/amd/pm/powerplay/
 * smumgr/smu7_smumgr.c (send_msg_to_smc), inc/smu7_ppsmc.h (Test/Result),
 * and include/asic_reg/smu/smu_7_1_3_sh_mask.h (16-bit response field).
 * Linux's legacy routine logs errors and returns zero; deliberately do not
 * inherit that return convention, which would turn an unsupported message
 * into permission to continue hardware initialization. */
#define SMU7_TEST 0x100u
#define SMU7_RESPONSE_MASK 0xffffu

struct transaction {
    const struct polaris_smu_io *io;
    const struct polaris_smu_budget *budget;
    struct polaris_smu_observation *out;
    uint64_t start, last;
};

static int time_remaining(struct transaction *t)
{
    uint64_t now = t->io->now_us(t->io->opaque);
    if (now < t->last) return POLARIS_SMU_BAD_CLOCK;
    t->last = now;
    return now - t->start < t->budget->timeout_us ?
           POLARIS_SMU_OK : POLARIS_SMU_TIMEOUT;
}

static int response(struct transaction *t, uint32_t *value)
{
    int rc = time_remaining(t);
    if (rc) return rc;
    if (t->out->polls == t->budget->max_polls) return POLARIS_SMU_TIMEOUT;
    t->out->polls++;
    if (t->io->read(t->io->opaque, POLARIS_SMU_RESPONSE, value))
        return POLARIS_SMU_IO_ERROR;
    t->out->raw_response = *value;
    /* An absent PCI device commonly reads all ones. Masking first would lose
     * that diagnostic and wrongly treat it as an ordinary firmware response. */
    if (*value == UINT32_MAX) return POLARIS_SMU_IO_ERROR;
    *value &= SMU7_RESPONSE_MASK;
    return time_remaining(t);
}

static int write_slot(struct transaction *t, enum polaris_smu_slot slot,
                      uint32_t value)
{
    int rc = time_remaining(t);
    if (rc) return rc;
    t->out->writes++;
    /* A failing callback might already have posted a write, so attempt, not
     * callback success, establishes the uncertain in-flight boundary. */
    if (slot == POLARIS_SMU_MESSAGE) t->out->submitted = 1;
    if (t->io->write(t->io->opaque, slot, value)) return POLARIS_SMU_IO_ERROR;
    return time_remaining(t);
}

int polaris_smu_test(struct polaris_smu_mailbox *ctx,
                     const struct polaris_smu_io *io,
                     const struct polaris_smu_budget *budget,
                     struct polaris_smu_observation *out)
{
    struct transaction t;
    uint32_t value;
    int rc;
    if (!ctx || !out) return POLARIS_SMU_BAD_ARGUMENT;
    if (__atomic_exchange_n(&ctx->lock, 1u, __ATOMIC_ACQUIRE))
        return POLARIS_SMU_BUSY;
    *out = (struct polaris_smu_observation){0};
    if (ctx->quarantined) {
        out->quarantined = 1;
        rc = POLARIS_SMU_QUARANTINED;
        goto done;
    }
    if (!io || !io->read || !io->write || !io->now_us || !budget ||
        !budget->timeout_us || budget->timeout_us > 1000000u ||
        !budget->max_polls || budget->max_polls > 100000u) {
        rc = POLARIS_SMU_BAD_ARGUMENT;
        goto done;
    }
    t = (struct transaction){ .io = io, .budget = budget, .out = out };
    t.start = t.last = io->now_us(io->opaque);
    /* Zero means another transaction may still own the hardware. Waiting for
     * it is bounded and must not write even ARG, which that transaction might
     * still consume. Only a previous successful completion is reclaimed. */
    do {
        rc = response(&t, &value);
        if (rc) goto done;
    } while (!value);
    if (value != 1) {
        rc = POLARIS_SMU_PRIOR_ERROR;
        goto done;
    }
    /* Reserve at least the clear flush and one completion read before any
     * mutation; a caller's already exhausted budget cannot justify a send. */
    if (budget->max_polls - out->polls < 2) {
        rc = POLARIS_SMU_TIMEOUT;
        goto done;
    }

    rc = write_slot(&t, POLARIS_SMU_ARGUMENT, 0);
    if (rc) goto uncertain;
    rc = write_slot(&t, POLARIS_SMU_RESPONSE, 0);
    if (rc) goto uncertain;
    /* Posted-write flush and stale-ACK rejection happen BEFORE MESSAGE. A
     * preserved old 1 must not immediately "complete" a new command. */
    rc = response(&t, &value);
    if (rc) goto uncertain;
    if (value) {
        rc = POLARIS_SMU_STALE_RESPONSE;
        goto uncertain;
    }
    rc = write_slot(&t, POLARIS_SMU_MESSAGE, SMU7_TEST);
    if (rc) goto uncertain;
    do {
        rc = response(&t, &value);
        if (rc) goto uncertain;
    } while (!value);
    if (value == 1) rc = POLARIS_SMU_OK;
#ifdef POLARIS_SMU_NEGCTL_ACCEPT_UNSUPPORTED
    else if (value == 0xfe) rc = POLARIS_SMU_OK;
#endif
    else if (value == 0xfe) rc = POLARIS_SMU_UNSUPPORTED;
    else rc = POLARIS_SMU_REJECTED;
    goto done;

uncertain:
    /* Never restore RESPONSE=1 or automatically resend on timeout: neither
     * cancels a late device operation. Even pre-MESSAGE failures after ARG or
     * response clearing need explicit hardware recovery before reuse. */
    if (out->writes) ctx->quarantined = 1;
    out->quarantined = (uint8_t)ctx->quarantined;
done:
    __atomic_store_n(&ctx->lock, 0u, __ATOMIC_RELEASE);
    return rc;
}
