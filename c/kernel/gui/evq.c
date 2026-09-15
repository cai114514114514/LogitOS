#include "evq.h"

static unsigned long long g_queued, g_coalesced, g_dropped, g_evicted_motion;

unsigned long long evq_queued(void)    { return __atomic_load_n(&g_queued, __ATOMIC_RELAXED); }
unsigned long long evq_coalesced(void) { return __atomic_load_n(&g_coalesced, __ATOMIC_RELAXED); }
unsigned long long evq_dropped(void)   { return __atomic_load_n(&g_dropped, __ATOMIC_RELAXED); }
unsigned long long evq_evicted_motion(void) { return __atomic_load_n(&g_evicted_motion, __ATOMIC_RELAXED); }

static void evq_reset_locked(struct evq *q) { q->head = q->tail = 0; }

void evq_reset(struct evq *q)
{
    gui_spin_lock(&((struct evq *)q)->lock);
    evq_reset_locked(q);
    gui_spin_unlock(&((struct evq *)q)->lock);
}

static int evq_empty_locked(const struct evq *q) { return q->head == q->tail; }

int evq_empty(const struct evq *q)
{
    gui_spin_lock(&((struct evq *)q)->lock);
    int result = evq_empty_locked(q);
    gui_spin_unlock(&((struct evq *)q)->lock);
    return result;
}

static int evq_evict_motion_locked(struct evq *q)
{
#ifdef EVQ_NEGCTL_DROP_SEMANTIC
    (void)q;
    return 0;
#else
    int victim = q->head;
    while (victim != q->tail && q->q[victim].type != EV_MOUSE_MOVE)
        victim = (victim + 1) % EVQ_N;
    if (victim == q->tail) return 0;

    /* Full-ring overload only: compact one obsolete absolute motion and keep
     * every remaining key/click/wheel in its original order. Scanning on the
     * ordinary enqueue path would charge every event for a case a healthy app
     * never reaches. */
    for (int src = (victim + 1) % EVQ_N; src != q->tail; src = (src + 1) % EVQ_N) {
        q->q[victim] = q->q[src];
        victim = src;
    }
    q->tail = (q->tail + EVQ_N - 1) % EVQ_N;
    __atomic_add_fetch(&g_evicted_motion, 1, __ATOMIC_RELAXED);
    return 1;
#endif
}

static int evq_push_locked(struct evq *q, const struct logit_event *e)
{
    /* Coalesce motion onto motion. The tail entry is the newest UNREAD event,
     * at tail-1 -- and only when the ring is non-empty, because on an empty ring
     * that slot holds an event the app has already polled and acting on it would
     * resurrect it. Position is absolute, so overwriting is exact: the app sees
     * where the pointer IS, having merely skipped intermediate samples it could
     * not have painted anyway. `mods` comes along because the modifier state
     * that matters for a hover is the current one. */
    if (e->type == EV_MOUSE_MOVE && q->head != q->tail) {
        int last = (q->tail + EVQ_N - 1) % EVQ_N;
        if (q->q[last].type == EV_MOUSE_MOVE) {
            q->q[last].a = e->a;
            q->q[last].b = e->b;
            q->q[last].mods = e->mods;
            __atomic_add_fetch(&g_coalesced, 1, __ATOMIC_RELAXED);
            return EVQ_PUSH_COALESCED;
        }
    }

    int nt = (q->tail + 1) % EVQ_N;
    if (nt == q->head) {
        if (e->type != EV_MOUSE_MOVE && evq_evict_motion_locked(q))
            nt = (q->tail + 1) % EVQ_N;
        else {
            __atomic_add_fetch(&g_dropped, 1, __ATOMIC_RELAXED);
            return EVQ_PUSH_DROPPED;
        }
    }
    q->q[q->tail] = *e;
    q->tail = nt;
    __atomic_add_fetch(&g_queued, 1, __ATOMIC_RELAXED);
    return EVQ_PUSH_APPENDED;
}

int evq_push(struct evq *q, const struct logit_event *e)
{
    gui_spin_lock(&((struct evq *)q)->lock);
    int result = evq_push_locked(q, e);
    gui_spin_unlock(&((struct evq *)q)->lock);
    return result;
}

static int evq_pop_locked(struct evq *q, struct logit_event *out)
{
    if (q->head == q->tail) return 0;
    *out = q->q[q->head];
    q->head = (q->head + 1) % EVQ_N;
    return 1;
}

int evq_pop(struct evq *q, struct logit_event *out)
{
    gui_spin_lock(&((struct evq *)q)->lock);
    int result = evq_pop_locked(q, out);
    gui_spin_unlock(&((struct evq *)q)->lock);
    return result;
}
