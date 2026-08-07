/* Host unit test for the per-window event ring (c/kernel/gui/evq.c).
 *
 * The ring is 256 entries and silently drops when full. That was survivable
 * while the only producers were keystrokes and clicks -- human-rate events. It
 * stops being survivable the moment mouse MOTION goes in: one sample per PS/2
 * packet against an app that polls once per painted frame, with no bound on how
 * far those rates diverge. The fix is coalescing at enqueue, and "coalescing
 * works" is exactly the kind of claim that reads as obviously true in a comment
 * and is false in the code (off-by-one on the tail index, coalescing across a
 * consumed entry, coalescing the wrong event type).
 *
 * So this drives the REAL ring -- evq.c is compiled in, not reimplemented -- and
 * asserts the invariant that matters: an unbounded flood of motion cannot evict
 * a queued click, because motion never occupies more than one slot.
 *
 * Build (host, no QEMU):
 *   cc -O2 -Wall -Wextra -o build/evq_test tests/unit/evq_test.c \
 *      c/kernel/gui/evq.c -Ic/kernel/gui -Iinclude/abi && ./build/evq_test
 */

#include <stdio.h>
#include <string.h>
#include "evq.h"

static int failures;

static void check(int cond, const char *what)
{
    if (!cond) { printf("  FAIL %s\n", what); failures++; }
}

static void checki(long long got, long long want, const char *what)
{
    if (got != want) { printf("  FAIL %s: got %lld, want %lld\n", what, got, want); failures++; }
}

static struct logit_event mk(int type, int a, int b)
{
    struct logit_event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.a = a; e.b = b;
    return e;
}

static int depth(const struct evq *q) { return (q->tail - q->head + EVQ_N) % EVQ_N; }

int main(void)
{
    struct evq q;
    struct logit_event e;

    /* --- basics: FIFO order, empty/one/many --- */
    evq_reset(&q);
    checki(evq_pop(&q, &e), 0, "pop on an empty ring");
    for (int i = 0; i < 5; i++) { struct logit_event k = mk(EV_KEY, 'a' + i, 0); evq_push(&q, &k); }
    for (int i = 0; i < 5; i++) {
        checki(evq_pop(&q, &e), 1, "pop returns an event");
        checki(e.a, 'a' + i, "FIFO order");
    }
    checki(evq_pop(&q, &e), 0, "ring drained");

    /* --- the flood. 100k motion samples into a 256-slot ring with nothing
     *     draining it. Without coalescing this drops ~99.7% of them and, worse,
     *     freezes the app's idea of the pointer at sample 255. --- */
    unsigned long long drops_before = evq_dropped();
    evq_reset(&q);
    for (int i = 0; i < 100000; i++) { struct logit_event m = mk(EV_MOUSE_MOVE, i, 2 * i); evq_push(&q, &m); }
    checki(evq_dropped() - drops_before, 0, "100k motion samples dropped nothing");
    checki(depth(&q), 1, "100k motion samples occupy one slot");
    checki(evq_pop(&q, &e), 1, "the merged sample is there");
    checki(e.type, EV_MOUSE_MOVE, "merged event keeps its type");
    checki(e.a, 99999, "merged event carries the NEWEST position, not the oldest");
    checki(e.b, 2 * 99999, "merged event carries the newest y");

    /* --- the invariant the whole thing exists for: a click queued before a
     *     flood still gets delivered, and in order. --- */
    drops_before = evq_dropped();
    evq_reset(&q);
    { struct logit_event c = mk(EV_MOUSE, 10, 20); c.button = EV_BTN_LEFT; evq_push(&q, &c); }
    for (int i = 0; i < 50000; i++) { struct logit_event m = mk(EV_MOUSE_MOVE, i, i); evq_push(&q, &m); }
    { struct logit_event c = mk(EV_MOUSE_UP, 11, 21); c.button = EV_BTN_LEFT; evq_push(&q, &c); }
    checki(evq_dropped() - drops_before, 0, "a click survives a 50k-sample flood undropped");
    checki(depth(&q), 3, "flood between two clicks is one slot, not 50000");
    checki(evq_pop(&q, &e), 1, "press delivered");   checki(e.type, EV_MOUSE, "press first");
    checki(evq_pop(&q, &e), 1, "motion delivered");  checki(e.type, EV_MOUSE_MOVE, "motion second");
    checki(evq_pop(&q, &e), 1, "release delivered"); checki(e.type, EV_MOUSE_UP, "release third");
    checki(e.button, EV_BTN_LEFT, "release keeps its button id");

    /* --- what must NOT coalesce --- */
    evq_reset(&q);
    { struct logit_event a1 = mk(EV_WHEEL, 0, 0); a1.wheel = 1; evq_push(&q, &a1);
      struct logit_event a2 = mk(EV_WHEEL, 0, 0); a2.wheel = 1; evq_push(&q, &a2);
      struct logit_event a3 = mk(EV_WHEEL, 0, 0); a3.wheel = 1; evq_push(&q, &a3); }
    checki(depth(&q), 3, "three wheel notches stay three events (merging shortens the scroll)");

    evq_reset(&q);
    for (int i = 0; i < 4; i++) { struct logit_event k = mk(EV_KEY, 'x', 0); evq_push(&q, &k); }
    checki(depth(&q), 4, "repeated keys are not merged");

    /* Motion separated by a click must not reach back across it: only the entry
     * AT the tail is a merge candidate. */
    evq_reset(&q);
    { struct logit_event m = mk(EV_MOUSE_MOVE, 1, 1); evq_push(&q, &m);
      struct logit_event c = mk(EV_MOUSE, 2, 2);      evq_push(&q, &c);
      struct logit_event m2 = mk(EV_MOUSE_MOVE, 3, 3); evq_push(&q, &m2); }
    checki(depth(&q), 3, "motion does not merge across an intervening click");

    /* An empty ring must not merge onto the slot the app just consumed --
     * that would resurrect a delivered event. */
    evq_reset(&q);
    { struct logit_event m = mk(EV_MOUSE_MOVE, 7, 7); evq_push(&q, &m); }
    checki(evq_pop(&q, &e), 1, "motion delivered");
    { struct logit_event m = mk(EV_MOUSE_MOVE, 8, 8); evq_push(&q, &m); }
    checki(depth(&q), 1, "motion after a drain queues a NEW event, not a merge");
    checki(evq_pop(&q, &e), 1, "and it is deliverable");
    checki(e.a, 8, "with the new position");

    /* --- the ring can still legitimately fill, and says so --- */
    drops_before = evq_dropped();
    evq_reset(&q);
    for (int i = 0; i < EVQ_N + 100; i++) { struct logit_event k = mk(EV_KEY, 'k', 0); evq_push(&q, &k); }
    checki(depth(&q), EVQ_N - 1, "a full ring keeps one slot free (head==tail means empty)");
    checki(evq_dropped() - drops_before, 101, "overflow is counted, not silent");

    /* --- wraparound: the modulo arithmetic has to survive the tail passing the
     *     end of the buffer, which is where a coalescing off-by-one hides. --- */
    evq_reset(&q);
    for (int cycle = 0; cycle < 10; cycle++) {
        for (int i = 0; i < 200; i++) { struct logit_event k = mk(EV_KEY, i & 0x7F, 0); evq_push(&q, &k); }
        for (int i = 0; i < 200; i++) { checki(evq_pop(&q, &e), 1, "wrap: pop"); checki(e.a, i & 0x7F, "wrap: order"); }
    }
    evq_reset(&q);
    for (int i = 0; i < EVQ_N - 2; i++) { struct logit_event k = mk(EV_KEY, 'p', 0); evq_push(&q, &k); }
    for (int i = 0; i < EVQ_N - 2; i++) evq_pop(&q, &e);          /* head/tail now near the end */
    drops_before = evq_dropped();
    for (int i = 0; i < 10000; i++) { struct logit_event m = mk(EV_MOUSE_MOVE, i, i); evq_push(&q, &m); }
    checki(depth(&q), 1, "coalescing still merges once the ring has wrapped");
    checki(evq_dropped() - drops_before, 0, "and drops nothing there either");

    check(evq_queued() > 0, "the queued counter moved");
    check(evq_coalesced() > 100000, "the coalesced counter moved");

    if (failures) { printf("evq_test: %d FAILURE(S)\n", failures); return 1; }
    printf("evq_test: ok (FIFO, flood coalescing, click survival, no cross-merge, wraparound, overflow accounting)\n");
    return 0;
}
