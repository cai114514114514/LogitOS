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
 * Historical locking: none, deliberately. Every caller (the WM thread draining input, an
 * app's SYS_POLL_EVENT, wm_set_dark) runs holding the BKL, as they did when
 * this was inline in wm.c. The input IRQs do NOT reach here -- they push onto
 * wm.c's raw inq[] and the WM thread does the real work.
 * Correction: a per-ring lock now protects head, tail and motion coalescing.
 * The waiter takes evwq.lock before checking this ring; the producer releases
 * the ring lock before waking evwq, preserving that order without a BKL. */

#include "logit_abi.h"
#include "gui_sync.h"

#define EVQ_N 256        /* deep enough that a burst of keystrokes isn't dropped
                          * while the app repaints; motion is bounded by the
                          * coalescing above, not by this number */

struct evq {
    struct gui_spin lock;
    struct logit_event q[EVQ_N];
    int head, tail;      /* head == tail: empty. One slot is always left free. */
};

enum evq_push_result {
    EVQ_PUSH_DROPPED = 0,
    EVQ_PUSH_APPENDED = 1,     /* one new unread slot was added */
    EVQ_PUSH_COALESCED = 2,
};

/* Append `e`, coalescing a motion sample onto an unread motion sample at the
 * tail. A semantic event arriving at a full ring replaces one old motion
 * sample when possible; a missing release can wedge a drag, while an old
 * absolute pointer position is already obsolete. EVQ_PUSH_APPENDED means one
 * new unread slot exists and must wake one waiter: multiple threads may wait
 * on the same process/window, so an already-readable ring can still have
 * another sleeper for that new event. A coalesced motion adds no slot and needs
 * no additional wake. */
int evq_push(struct evq *q, const struct logit_event *e);

/* -> 1 and fills *out, or 0 when empty. */
int evq_pop(struct evq *q, struct logit_event *out);

/* 1 when there is nothing to pop. Exists for SYS_WAIT_EVENT's sleep
 * predicate, which must be able to ask without consuming. */
int evq_empty(const struct evq *q);

/* Forget everything queued (a window slot being reused). A new queue must be
 * zero-initialized before first use; resetting a live queue never resets its lock. */
void evq_reset(struct evq *q);

/* System-wide counters since boot, reported by SYS_SYSINFO so the coalescing is
 * observable from userland instead of merely asserted in a comment:
 *   queued     events actually placed in a ring
 *   coalesced  motion samples merged into the tail instead of queued
 *   dropped    events lost to a full ring -- the number that must stay 0 */
unsigned long long evq_queued(void);
unsigned long long evq_coalesced(void);
unsigned long long evq_dropped(void);
unsigned long long evq_evicted_motion(void);

#endif /* LOGIT_EVQ_H */
