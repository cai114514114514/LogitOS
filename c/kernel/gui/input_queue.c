#include "input_queue.h"

#if __STDC_HOSTED__
static uint64_t iq_lock(struct input_queue *q)
{
    while (__atomic_exchange_n(&q->lock.held, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&q->lock.held, __ATOMIC_RELAXED)) {}
    return 0;
}
static void iq_unlock(struct input_queue *q, uint64_t flags)
{
    (void)flags;
    __atomic_store_n(&q->lock.held, 0, __ATOMIC_RELEASE);
}
#else
static uint64_t iq_lock(struct input_queue *q)
{
    return spin_lock_irqsave(&q->lock.raw);
}
static void iq_unlock(struct input_queue *q, uint64_t flags)
{
    spin_unlock_irqrestore(&q->lock.raw, flags);
}
#endif

static unsigned iq_next(unsigned i) { return (i + 1U) % INPUTQ_N; }
static unsigned iq_prev(unsigned i) { return (i + INPUTQ_N - 1U) % INPUTQ_N; }
static unsigned iq_depth_locked(const struct input_queue *q)
{
    return (q->tail + INPUTQ_N - q->head) % INPUTQ_N;
}

#define INPUTQ_F_BUTTON_EDGE 1U

static int is_motion(const struct inputq_event *e)
{
    return e->type == INPUTQ_POINTER && e->wheel == 0 &&
           !(e->flags & INPUTQ_F_BUTTON_EDGE);
}

#ifndef INPUTQ_NEGCTL_NO_COALESCE
static int same_buttons(const struct inputq_event *a,
                        const struct inputq_event *b)
{
    return a->left == b->left && a->right == b->right &&
           a->middle == b->middle;
}
#endif

/* Remove one stale pointer state without changing the order of anything else.
 * This path is O(511), but it runs only after the queue is already full. Doing
 * the scan on every IRQ would spend the normal case to optimise an overload;
 * doing it here turns that overload into one discarded absolute position
 * instead of a missing key/button-up/wheel edge. */
static int evict_old_motion_locked(struct input_queue *q)
{
#ifdef INPUTQ_NEGCTL_DROP_CRITICAL
    (void)q;
    return 0;
#else
    unsigned victim = q->head;
    while (victim != q->tail && !is_motion(&q->q[victim]))
        victim = iq_next(victim);
    if (victim == q->tail) return 0;

    for (unsigned src = iq_next(victim); src != q->tail; src = iq_next(src)) {
        q->q[victim] = q->q[src];
        victim = src;
    }
    q->tail = iq_prev(q->tail);
    q->stats.evicted_motion++;
    return 1;
#endif
}

static int push_locked(struct input_queue *q, const struct inputq_event *e)
{
#ifndef INPUTQ_NEGCTL_NO_COALESCE
    if (is_motion(e) && q->head != q->tail) {
        struct inputq_event *last = &q->q[iq_prev(q->tail)];
        if (is_motion(last) && same_buttons(last, e)) {
            *last = *e;
            q->stats.coalesced++;
            return INPUTQ_COALESCED;
        }
    }
#endif

    unsigned next = iq_next(q->tail);
    if (next == q->head) {
        if (!is_motion(e) && evict_old_motion_locked(q))
            next = iq_next(q->tail);
        else {
            if (is_motion(e)) q->stats.dropped_motion++;
            else q->stats.dropped_semantic++;
            return INPUTQ_DROPPED;
        }
    }

    q->q[q->tail] = *e;
    q->tail = next;
    q->stats.queued++;
    unsigned depth = iq_depth_locked(q);
    if (depth > q->stats.high_watermark) q->stats.high_watermark = depth;
    return INPUTQ_QUEUED;
}

void inputq_reset(struct input_queue *q)
{
    uint64_t flags = iq_lock(q);
    q->head = q->tail = 0;
    q->last_left = q->last_right = q->last_middle = 0;
    q->have_pointer = 0;
    q->stats = (struct inputq_stats){0};
    iq_unlock(q, flags);
}

int inputq_push_pointer(struct input_queue *q, int x, int y,
                        int left, int right, int middle, int wheel, int mods)
{
    uint64_t flags = iq_lock(q);
    left = left != 0; right = right != 0; middle = middle != 0;
    unsigned edge = q->have_pointer &&
        (left != q->last_left || right != q->last_right ||
         middle != q->last_middle);
    if (!q->have_pointer && (left || right || middle)) edge = 1;
    struct inputq_event e = {
        INPUTQ_POINTER, x, y, left, right, middle, wheel, mods,
        edge ? INPUTQ_F_BUTTON_EDGE : 0
    };
    int result = push_locked(q, &e);
    /* Compare the next report with the newest state the queue ACCEPTED, not
     * merely the newest state hardware attempted to publish. If a ring made
     * entirely of semantic events has no stale motion to evict, the first
     * button-up is unavoidably dropped. Keeping the accepted level here makes
     * every later all-up report retry that edge until one gets into the queue;
     * recording the dropped level would silently turn those retries into
     * evictable motion and could leave the WM's drag state wedged forever. */
    int accepted = result != INPUTQ_DROPPED;
#ifdef INPUTQ_NEGCTL_ACK_DROPPED_EDGE
    accepted = 1;
#endif
    if (accepted) {
        q->last_left = left;
        q->last_right = right;
        q->last_middle = middle;
        q->have_pointer = 1;
    }
    iq_unlock(q, flags);
    return result;
}

int inputq_push_key(struct input_queue *q, int key, int mods)
{
    struct inputq_event e = { INPUTQ_KEY, key, 0, 0, 0, 0, 0, mods, 0 };
    uint64_t flags = iq_lock(q);
    int result = push_locked(q, &e);
    iq_unlock(q, flags);
    return result;
}

int inputq_drain(struct input_queue *q, struct inputq_event *out)
{
    uint64_t flags = iq_lock(q);
#ifdef INPUTQ_NEGCTL_UNBOUNDED_DRAIN
    const unsigned limit = INPUTQ_N - 1;
#else
    const unsigned limit = INPUTQ_DRAIN_BUDGET;
#endif
    unsigned n = 0;
    while (n < limit && q->head != q->tail) {
        out[n++] = q->q[q->head];
        q->head = iq_next(q->head);
    }
    if (q->head != q->tail) q->stats.batches_with_backlog++;
    iq_unlock(q, flags);
    return (int)n;
}

int inputq_pending(struct input_queue *q)
{
    uint64_t flags = iq_lock(q);
    int result = q->head != q->tail;
    iq_unlock(q, flags);
    return result;
}

unsigned inputq_depth(struct input_queue *q)
{
    uint64_t flags = iq_lock(q);
    unsigned result = iq_depth_locked(q);
    iq_unlock(q, flags);
    return result;
}

void inputq_get_stats(struct input_queue *q, struct inputq_stats *out)
{
    uint64_t flags = iq_lock(q);
    *out = q->stats;
    iq_unlock(q, flags);
}
