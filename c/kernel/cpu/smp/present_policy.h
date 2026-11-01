#ifndef LOGIT_SMP_PRESENT_POLICY_H
#define LOGIT_SMP_PRESENT_POLICY_H
#include <stdint.h>

/* Self-disable policy for the IPI-parallel present (smp.c). When other cores
 * do not service vector 241 promptly, every tall present pays the full ack
 * timeout plus a serial copy, which is slower than the single-core path. These
 * constants decide when that is proven to be happening on this machine.
 *
 * Window: 64 presents is about one second at 60 Hz, long enough that one
 * descheduled core cannot trip it, short enough to react within a second.
 * 8 of 64 (12.5 %) is well above the occasional miss a busy core produces.
 * 4 in a row catches a machine where vector 241 is never serviced at all,
 * before the window has even filled. */
#define SMP_PRESENT_WINDOW_SIZE        64
#define SMP_PRESENT_WINDOW_TIMEOUTS    8
#define SMP_PRESENT_CONSECUTIVE_LIMIT  4

struct smp_present_window {
    uint64_t history;      /* bit n set: the present n calls ago timed out */
    uint32_t calls;        /* presents recorded so far, saturating */
    uint32_t consecutive;  /* timeouts in a row ending at the last present */
    int      disabled;     /* a disable verdict was already returned */
};

/* Record one present (timed_out != 0 when the ack wait expired). Returns 1
 * exactly once, on the present that proves the parallel path is harmful, and
 * 0 for every other call. */
int smp_present_policy(struct smp_present_window *window, int timed_out);

#endif
