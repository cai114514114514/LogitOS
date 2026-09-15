#ifndef LOGIT_SMP_BOOT_MODEL_H
#define LOGIT_SMP_BOOT_MODEL_H

enum smp_ap_boot_state {
    SMP_AP_EMPTY = 0,
    SMP_AP_CLAIMED,
    SMP_AP_PUBLISHING,
    SMP_AP_ONLINE,
    SMP_AP_REJECTED,
};

/* Publish an AP only from its fixed claimed slot.  This release-publishes all
 * local initialization, but only the BSP may add the slot to the dense online
 * prefix after it acquire-observes ONLINE. */
int smp_ap_publish_online(volatile int *state);

/* Commit one already-published slot to the dense online prefix.  The count
 * must still equal the slot, so a skipped or duplicate admission is rejected. */
int smp_bsp_commit_online(volatile int *state, volatile int *online_count,
                          int slot);

/* Resolve a timeout without racing a late AP: only CLAIMED/PUBLISHING may be
 * changed to REJECTED.  If ONLINE won the race, return ONLINE unchanged. */
enum smp_ap_boot_state smp_bsp_cancel_unpublished(volatile int *state);

/* Once any INIT/SIPI may have left the LAPIC, timeout cannot prove the AP is
 * absent.  Its live stack must remain quarantined for a possible late start. */
int smp_ap_stack_may_free(enum smp_ap_boot_state state, int startup_sent);

#endif
