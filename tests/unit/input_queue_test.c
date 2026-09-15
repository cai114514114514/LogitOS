/* Drives the production IRQ -> WM queue.  Host wall time is deliberately not
 * measured: the useful bounds are event counts and order, which do not depend
 * on host scheduling or timer resolution.
 *
 * Four build-time mutations are exercised by tests/input_delivery.mk:
 *   NO_COALESCE: pointer flood fills the raw ring and loses the latest;
 *   UNBOUNDED_DRAIN: one pass consumes all work before a frame can run;
 *   DROP_CRITICAL restores the old full-ring policy that drops button-up;
 *   ACK_DROPPED_EDGE forgets to retry a button edge the ring could not admit.
 */
#include <stdio.h>
#include <string.h>
#include "input_queue.h"

static int failures;

static void check(int condition, const char *name)
{
    if (!condition) { printf("FAIL: %s\n", name); failures++; }
}

static void flood_coalescing(void)
{
    struct input_queue q = INPUT_QUEUE_INIT;
    struct inputq_event batch[INPUTQ_N];
    inputq_reset(&q);
    for (int i = 0; i < 100000; i++)
        inputq_push_pointer(&q, i, i * 2, 0, 0, 0, 0, i & 3);

    struct inputq_stats s;
    inputq_get_stats(&q, &s);
    int n = inputq_drain(&q, batch);
    check(n == 1 && batch[0].x == 99999 && batch[0].y == 199998,
          "pointer flood retains exactly the newest position");
    check(s.coalesced == 99999 && s.dropped_motion == 0,
          "pointer flood coalesces without raw-ring loss");
    check(s.high_watermark == 1,
          "pointer flood occupies one raw-ring slot");
}

static void drag_edges(void)
{
    struct input_queue q = INPUT_QUEUE_INIT;
    struct inputq_event batch[INPUTQ_N];
    inputq_reset(&q);
    inputq_push_pointer(&q, 10, 10, 1, 0, 0, 0, 0); /* press */
    for (int i = 0; i < 50000; i++)
        inputq_push_pointer(&q, 11 + i, 12 + i, 1, 0, 0, 0, 0);
    inputq_push_pointer(&q, 50011, 50012, 0, 0, 0, 0, 0); /* release */

    int n = inputq_drain(&q, batch);
    check(n == 3, "drag flood keeps press, newest move, and release");
    check(n >= 3 && batch[0].left == 1 && batch[0].x == 10,
          "drag press keeps its original hit-test coordinate");
    check(n >= 3 && batch[1].left == 1 && batch[1].x == 50010,
          "drag coalescing tracks the latest held position");
    check(n >= 3 && batch[2].left == 0 && batch[2].x == 50011,
          "drag release is a distinct ordered edge");
}

static void full_ring_keeps_release(void)
{
    struct input_queue q = INPUT_QUEUE_INIT;
    struct inputq_event batch[INPUTQ_N];
    inputq_reset(&q);

    /* Move head/tail near the physical end first. The subsequent full-ring
     * compaction must preserve order while both live spans wrap index zero. */
    for (int i = 0; i < 200; i++) inputq_push_key(&q, i, 0);
    while (inputq_pending(&q)) inputq_drain(&q, batch);

    inputq_push_pointer(&q, 1, 1, 1, 0, 0, 0, 0);
    for (int i = 0; inputq_depth(&q) < INPUTQ_N - 1; i++) {
        if (i & 1) inputq_push_key(&q, 'a' + i % 26, 0);
        else inputq_push_pointer(&q, 2 + i, 3 + i, 1, 0, 0, 0, 0);
    }
    check(inputq_push_pointer(&q, 900, 901, 0, 0, 0, 0, 0) == INPUTQ_QUEUED,
          "full raw ring admits button release");

    struct inputq_stats s;
    inputq_get_stats(&q, &s);
    check(s.evicted_motion == 1 && s.dropped_semantic == 0,
          "release evicts one stale move instead of a semantic event");

    int found_release = 0;
    while (inputq_pending(&q)) {
        int n = inputq_drain(&q, batch);
        for (int i = 0; i < n; i++)
            if (batch[i].type == INPUTQ_POINTER && !batch[i].left &&
                batch[i].x == 900) found_release++;
    }
    check(found_release == 1,
          "button release remains observable after overload");
}

static void dropped_release_retries_until_admitted(void)
{
    struct input_queue q = INPUT_QUEUE_INIT;
    struct inputq_event batch[INPUTQ_N];
    inputq_reset(&q);

    /* No motion is available as a pressure valve: this first release really
     * cannot fit. Once the WM consumes one entry, another report with the same
     * all-up levels must still be protected as the unobserved release. */
    inputq_push_pointer(&q, 10, 10, 1, 0, 0, 0, 0);
    for (int i = 0; i < INPUTQ_N - 2; i++) inputq_push_key(&q, i, 0);
    check(inputq_depth(&q) == INPUTQ_N - 1,
          "semantic-only retry fixture fills the raw ring");
    check(inputq_push_pointer(&q, 20, 20, 0, 0, 0, 0, 0) == INPUTQ_DROPPED,
          "semantic-only full ring reports the unavoidable first release loss");

    check(inputq_drain(&q, batch) == INPUTQ_DRAIN_BUDGET,
          "retry fixture creates a bounded amount of room");
    check(inputq_push_pointer(&q, 21, 21, 0, 0, 0, 0, 0) == INPUTQ_QUEUED,
          "unchanged all-up report retries the release after room appears");

    /* Refill to capacity, then add one more key. If the retry was mislabeled
     * as ordinary motion, that key would evict it and appear to succeed. */
    for (int i = 0; i < INPUTQ_DRAIN_BUDGET - 1; i++)
        inputq_push_key(&q, 1000 + i, 0);
    struct inputq_stats before, after;
    inputq_get_stats(&q, &before);
    check(inputq_push_key(&q, 2000, 0) == INPUTQ_DROPPED,
          "admitted release remains semantic under renewed overload");
    inputq_get_stats(&q, &after);
    check(after.evicted_motion == before.evicted_motion,
          "renewed overload cannot evict the retried release");

    int found_release = 0;
    while (inputq_pending(&q)) {
        int n = inputq_drain(&q, batch);
        for (int i = 0; i < n; i++)
            if (batch[i].type == INPUTQ_POINTER && !batch[i].left &&
                batch[i].x == 21) found_release++;
    }
    check(found_release == 1,
          "retried release remains observable after renewed overload");
}

static void bounded_fairness(void)
{
    struct input_queue q = INPUT_QUEUE_INIT;
    struct inputq_event batch[INPUTQ_N];
    inputq_reset(&q);
    for (int i = 0; i < 100; i++) inputq_push_key(&q, i, 0);

    int n = inputq_drain(&q, batch);
    check(n == INPUTQ_DRAIN_BUDGET,
          "one WM pass has a fixed input-work bound");
    check(inputq_pending(&q),
          "backlog yields a frame opportunity instead of draining forever");
    for (int i = 0; i < n; i++)
        check(batch[i].type == INPUTQ_KEY && batch[i].x == i,
              "bounded batch preserves key order");

    struct inputq_stats s;
    inputq_get_stats(&q, &s);
    check(s.batches_with_backlog == 1,
          "bounded-drain fairness is visible in diagnostics");
}

static void wheel_and_key_boundaries(void)
{
    struct input_queue q = INPUT_QUEUE_INIT;
    struct inputq_event batch[INPUTQ_N];
    inputq_reset(&q);
    inputq_push_pointer(&q, 1, 1, 0, 0, 0, 0, 0);
    inputq_push_key(&q, 'x', 0);
    inputq_push_pointer(&q, 2, 2, 0, 0, 0, 0, 0);
    inputq_push_pointer(&q, 3, 3, 0, 0, 0, 2, 0);
    inputq_push_pointer(&q, 4, 4, 0, 0, 0, 3, 0);
    int n = inputq_drain(&q, batch);
    check(n == 5, "keys and wheel notches remain coalescing boundaries");
    check(n >= 5 && batch[1].type == INPUTQ_KEY &&
          batch[3].wheel == 2 && batch[4].wheel == 3,
          "keys and wheel notches retain chronological order");
}

int main(void)
{
    flood_coalescing();
    drag_edges();
    full_ring_keeps_release();
    dropped_release_retries_until_admitted();
    bounded_fairness();
    wheel_and_key_boundaries();
    if (failures) {
        printf("input-queue: FAIL (%d)\n", failures);
        return 1;
    }
    printf("input-queue: PASS (100000 motion flood, release priority, max-batch=%d)\n",
           INPUTQ_DRAIN_BUDGET);
    return 0;
}
