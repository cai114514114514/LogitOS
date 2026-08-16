#ifndef LOGIT_EVQ_H
#define LOGIT_EVQ_H

/* The per-window event ring behind SYS_POLL_EVENT.
 *
 * It used to be three fields and an eight-line enqueue() inside wm.c. It lives
 * on its own now for one reason: mouse MOTION goes in here. A key press is a
 * human-rate event and 256 slots is a deep buffer for it; the pointer produces
 * a sample per PS/2 packet (up to 200/s per device, and QEMU's `input-send-event`
 * has no rate limit at all), and an app that spends 40 ms painting a frame is not
 * polling while it paints. The old ring answered a full queue by dropping the
 * NEWEST event, which for motion means the app's idea of where the pointer is
 * freezes at wherever it was 256 samples ago and never catches up.
 *
 * So enqueue coalesces: a motion sample landing on top of an unread motion
 * sample overwrites it. That is not a heuristic -- consecutive motion events are
 * genuinely redundant, since the position is absolute and the newest one is the
 * truth. Clicks, keys, wheel notches and closes are each independently
 * meaningful and are never merged. The invariant this buys: an unbounded flood
 * of motion cannot evict a single click, because motion occupies at most one
 * slot at the tail.
 *
 * Split out of wm.c so it can be tested on the host (tests/unit/evq_test.c) --
 * flooding the real ring with 100k samples and asserting zero drops is a
 * different claim from reading the code and believing it.
 *
 * Locking: none, deliberately. Every caller (the WM thread draining input, an
 * app's SYS_POLL_EVENT, wm_set_dark) runs holding the BKL, as they did when
 * this was inline in wm.c. The input IRQs do NOT reach here -- they push onto
 * wm.c's raw inq[] and the WM thread does the real work. */

#include "logit_abi.h"

#define EVQ_N 256        /* deep enough that a burst of keystrokes isn't dropped
                          * while the app repaints; motion is bounded by the
                          * coalescing above, not by this number */

struct evq {
    struct logit_event q[EVQ_N];
    int head, tail;      /* head == tail: empty. One slot is always left free. */
};

/* Append `e`, coalescing a motion sample onto an unread motion sample at the
 * tail. Silently drops when full (the caller has nothing useful to do about a
 * queue an app is not draining). */
void evq_push(struct evq *q, const struct logit_event *e);

/* -> 1 and fills *out, or 0 when empty. */
int evq_pop(struct evq *q, struct logit_event *out);

/* 1 when there is nothing to pop. Exists for SYS_WAIT_EVENT's sleep
 * predicate, which must be able to ask without consuming. */
int evq_empty(const struct evq *q);

/* Forget everything queued (a window slot being reused). */
void evq_reset(struct evq *q);

/* System-wide counters since boot, reported by SYS_SYSINFO so the coalescing is
 * observable from userland instead of merely asserted in a comment:
 *   queued     events actually placed in a ring
 *   coalesced  motion samples merged into the tail instead of queued
 *   dropped    events lost to a full ring -- the number that must stay 0 */
unsigned long long evq_queued(void);
unsigned long long evq_coalesced(void);
unsigned long long evq_dropped(void);

#endif /* LOGIT_EVQ_H */
