#ifndef LOGIT_INPUT_QUEUE_H
#define LOGIT_INPUT_QUEUE_H

#include <stdint.h>

/* IRQ -> window-manager input transport.
 *
 * Pointer controllers publish an absolute position plus button levels. Two
 * adjacent reports with unchanged levels and no wheel delta therefore describe
 * one state: only the newest position can affect the next WM pass. Keeping all
 * of them made a 512-slot ring retain old drag positions, drop the release at
 * the end, and let a producer prevent the old "drain until empty" loop from
 * reaching a frame indefinitely.
 *
 * The queue has three deliberately separate properties:
 *   - its lock serialises PS/2 and USB IRQ producers with the WM consumer;
 *   - adjacent pointer states coalesce without crossing a key, wheel, or button
 *     edge, so those order boundaries remain exact;
 *   - inputq_drain() returns at most 16 events. The WM gets a composite and
 *     scheduler opportunity after that bounded amount of input even while a
 *     device continues reporting.
 *
 * A full queue may discard an OLD coalescible pointer state to admit a key,
 * button edge, or wheel event. It never invents an edge and never reorders the
 * remaining events. If all 511 entries are semantic, loss is unavoidable and
 * is counted separately instead of being called a motion drop.
 */

#if __STDC_HOSTED__
struct inputq_lock { unsigned held; };
#define INPUTQ_LOCK_INIT { 0 }
#else
#include "spinlock.h"
struct inputq_lock { spinlock_t raw; };
#define INPUTQ_LOCK_INIT { SPINLOCK_INIT }
#endif

#define INPUTQ_N 512
#define INPUTQ_DRAIN_BUDGET 16

enum inputq_type {
    INPUTQ_POINTER = 0,
    INPUTQ_KEY = 1,
};

struct inputq_event {
    int type;
    int x, y;
    int left, right, middle;
    int wheel;
    int mods;
    unsigned flags;             /* queue-private; consumers ignore this field */
};

struct inputq_stats {
    uint64_t queued;
    uint64_t coalesced;
    uint64_t evicted_motion;
    uint64_t dropped_motion;
    uint64_t dropped_semantic;
    uint64_t batches_with_backlog;
    unsigned high_watermark;
};

struct input_queue {
    struct inputq_lock lock;
    struct inputq_event q[INPUTQ_N];
    unsigned head, tail;
    int last_left, last_right, last_middle;
    int have_pointer;
    struct inputq_stats stats;
};

#define INPUT_QUEUE_INIT { .lock = INPUTQ_LOCK_INIT }

enum inputq_result {
    INPUTQ_DROPPED = 0,
    INPUTQ_QUEUED = 1,
    INPUTQ_COALESCED = 2,
};

void inputq_reset(struct input_queue *q);
int inputq_push_pointer(struct input_queue *q, int x, int y,
                        int left, int right, int middle, int wheel, int mods);
int inputq_push_key(struct input_queue *q, int key, int mods);

/* `out` must hold INPUTQ_DRAIN_BUDGET entries. */
int inputq_drain(struct input_queue *q, struct inputq_event *out);
int inputq_pending(struct input_queue *q);
unsigned inputq_depth(struct input_queue *q);
void inputq_get_stats(struct input_queue *q, struct inputq_stats *out);

#endif /* LOGIT_INPUT_QUEUE_H */
