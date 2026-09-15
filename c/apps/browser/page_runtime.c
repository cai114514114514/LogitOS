/* The current page timer/rAF queue's ownership and scheduling mechanism.
 * Kept independent of QuickJS: tests can exercise cancellation and generations
 * without inventing a second JavaScript runtime. js_page.c includes this file
 * just as layout.c includes layout_flex.c, so existing host source lists cannot
 * silently link the page while omitting its actual queue implementation.
 *
 * The deterministic host gate tests the shipping consumer as well as a two-slot
 * queue: a third enqueue is refused, cancellation frees a slot, and reopening
 * the same owner refuses the previous page's token. Those are ownership/order
 * checks, not a claim about guest frame cadence. Fetch, workers and DOM event
 * queues deliberately retain their existing pumps; this module owns only the
 * timer/rAF callbacks actually migrated by js_page.c. */
#include "page_runtime.h"

static void page_runtime_advance(struct page_runtime *p)
{
    /* Never publish zero as a live epoch. The counter belongs to the owner,
     * not to a JSContext allocation address that malloc can immediately reuse. */
    p->epoch++;
    if (!p->epoch) p->epoch++;
}

int page_runtime_open(struct page_runtime *p, void *runtime, void *context,
                      void *document, size_t capacity)
{
    if (!p || p->state != PAGE_RUNTIME_CLOSED || !context || !capacity) return 0;
    page_runtime_advance(p);
    p->runtime = runtime; p->context = context; p->document = document;
    p->sequence = 0; p->tasks = 0; p->count = 0; p->capacity = capacity;
    p->state = PAGE_RUNTIME_OPEN;
    return 1;
}

struct page_runtime_token page_runtime_token(const struct page_runtime *p)
{
    struct page_runtime_token t = {p, p ? p->epoch : 0}; return t;
}

int page_runtime_accepts(struct page_runtime_token t)
{
    if (!t.owner || t.owner->state != PAGE_RUNTIME_OPEN) return 0;
#ifdef PAGE_RUNTIME_STALE_EPOCH
    return 1; /* negative control: the reused owner address alone is identity */
#else
    return t.epoch != 0 && t.epoch == t.owner->epoch;
#endif
}

void page_runtime_invalidate(struct page_runtime *p)
{
    if (!p || p->state != PAGE_RUNTIME_OPEN) return;
    p->state = PAGE_RUNTIME_CLOSING;
    page_runtime_advance(p);
}

int page_task_detach(struct page_runtime *p, struct page_task *t)
{
    if (!p || !t || !t->queued || t->token.owner != p) return 0;
    for (struct page_task **at = &p->tasks; *at; at = &(*at)->next) {
        if (*at != t) continue;
        *at = t->next; t->next = 0; t->queued = 0; p->count--;
        return 1;
    }
    return 0;
}

int page_task_cancel(struct page_runtime *p, struct page_task *t)
{
    if (!page_task_detach(p, t)) return 0;
    /* Detach first: disposal may inspect/cancel other tasks. The callback may
     * free the enclosing task object, so nothing reads t after this call. */
    if (t->dispose) t->dispose(p->context, t->payload);
    return 1;
}

void page_runtime_close(struct page_runtime *p)
{
    if (!p) return;
    page_runtime_invalidate(p);
    while (p->tasks) page_task_cancel(p, p->tasks);
    p->runtime = p->context = p->document = 0;
    p->state = PAGE_RUNTIME_CLOSED;
}

int page_task_enqueue(struct page_runtime *p, struct page_task *t,
                      struct page_runtime_token token, uint64_t due, unsigned source,
                      void *payload, void (*dispose)(void *, void *))
{
    if (!p || p->state != PAGE_RUNTIME_OPEN) return PAGE_TASK_CLOSED;
    if (token.owner != p || !page_runtime_accepts(token)) return PAGE_TASK_STALE;
    if (!t || t->queued) return PAGE_TASK_QUEUED;
#ifndef PAGE_RUNTIME_UNBOUNDED
    if (p->count >= p->capacity) return PAGE_TASK_FULL;
#endif
    t->token = token; t->due = due; t->seq = ++p->sequence;
    t->source = source; t->payload = payload; t->dispose = dispose;
    t->queued = 1; t->next = p->tasks; p->tasks = t; p->count++;
    return PAGE_TASK_OK;
}

int page_task_rearm(struct page_runtime *p, struct page_task *t, uint64_t due)
{
    if (!p || !t || !t->queued || t->token.owner != p || !page_runtime_accepts(t->token)) return 0;
    t->due = due; t->seq = ++p->sequence;
    return 1;
}

uint64_t page_runtime_turn(const struct page_runtime *p)
{ return p ? p->sequence : 0; }

struct page_task *page_runtime_ready(struct page_runtime *p, uint64_t now, uint64_t limit)
{
    if (!p || p->state != PAGE_RUNTIME_OPEN) return 0;
    struct page_task *best = 0;
    for (struct page_task *t = p->tasks; t; t = t->next) {
        if (!page_runtime_accepts(t->token) || t->due > now || t->seq > limit) continue;
        if (!best || t->due < best->due || (t->due == best->due && t->seq < best->seq)) best = t;
    }
    return best;
}

long long page_runtime_next_due(const struct page_runtime *p)
{
    if (!p || p->state != PAGE_RUNTIME_OPEN) return -1;
    long long best = -1;
    for (const struct page_task *t = p->tasks; t; t = t->next)
        if (page_runtime_accepts(t->token) && (best < 0 || t->due < (uint64_t)best))
            best = t->due > 0x7fffffffffffffffULL ? 0x7fffffffffffffffLL : (long long)t->due;
    return best;
}
