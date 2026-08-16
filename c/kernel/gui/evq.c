#include "evq.h"

static unsigned long long g_queued, g_coalesced, g_dropped;

unsigned long long evq_queued(void)    { return g_queued; }
unsigned long long evq_coalesced(void) { return g_coalesced; }
unsigned long long evq_dropped(void)   { return g_dropped; }

void evq_reset(struct evq *q) { q->head = q->tail = 0; }

int evq_empty(const struct evq *q) { return q->head == q->tail; }

void evq_push(struct evq *q, const struct logit_event *e)
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
            g_coalesced++;
            return;
        }
    }

    int nt = (q->tail + 1) % EVQ_N;
    if (nt == q->head) { g_dropped++; return; }   /* full: drop */
    q->q[q->tail] = *e;
    q->tail = nt;
    g_queued++;
}

int evq_pop(struct evq *q, struct logit_event *out)
{
    if (q->head == q->tail) return 0;
    *out = q->q[q->head];
    q->head = (q->head + 1) % EVQ_N;
    return 1;
}
