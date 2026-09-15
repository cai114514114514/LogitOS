/* SPDX-License-Identifier: MIT */
#include "polaris_smu_mailbox.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

/* An independent ordered bus transcript, not a mock that executes the same
 * state machine. Literal 0x100/1/0xfe are pinned wire expectations. Every read
 * and write, including a stale clear flush, must match the next event. */
struct event { char kind; enum polaris_smu_slot slot; uint32_t value; };
#define R(v) {'r', POLARIS_SMU_RESPONSE, (v)}
#define A {'w', POLARIS_SMU_ARGUMENT, 0}
#define C {'w', POLARIS_SMU_RESPONSE, 0}
#define M {'w', POLARIS_SMU_MESSAGE, 0x100}
static const struct event happy[] = {R(1), A, C, R(0), M, R(0), R(1)};
struct bus {
    const struct event *events;
    unsigned count, at, mismatches;
    int fail_at;
    uint64_t now, step;
    int backwards;
    struct polaris_smu_mailbox *reenter;
    struct polaris_smu_io *io;
    struct polaris_smu_budget *budget;
    unsigned nested;
};

static int transfer(struct bus *b, char kind, enum polaris_smu_slot slot,
                    uint32_t *value)
{
    unsigned at = b->at++;
    if (at >= b->count || b->events[at].kind != kind ||
        b->events[at].slot != slot ||
        (kind == 'w' && b->events[at].value != *value)) {
        b->mismatches++;
        return -1;
    }
    if ((int)at == b->fail_at) return -1;
    if (kind == 'r') *value = b->events[at].value;
    return 0;
}
static int read_slot(void *p, enum polaris_smu_slot slot, uint32_t *value)
{
    struct bus *b = p;
    if (b->reenter && !b->nested++) {
        struct polaris_smu_observation nested = { .raw_response = 1234 };
        CHECK(polaris_smu_test(b->reenter, b->io, b->budget, &nested) ==
              POLARIS_SMU_BUSY);
        CHECK(nested.raw_response == 1234 && !nested.polls && !nested.writes);
    }
    return transfer(b, 'r', slot, value);
}
static int write_slot(void *p, enum polaris_smu_slot slot, uint32_t value)
{
    return transfer(p, 'w', slot, &value);
}
static uint64_t now_us(void *p)
{
    struct bus *b = p;
    uint64_t now = b->now;
    if (b->backwards) b->now--;
    else b->now += b->step;
    return now;
}
static struct bus transcript(const struct event *events, unsigned n)
{
    return (struct bus){ .events = events, .count = n, .fail_at = -1, .step = 1 };
}
static int run(struct polaris_smu_mailbox *ctx, struct bus *b,
               struct polaris_smu_budget budget,
               struct polaris_smu_observation *out)
{
    struct polaris_smu_io io = {b, read_slot, write_slot, now_us};
    b->io = &io;
    b->budget = &budget;
    return polaris_smu_test(ctx, &io, &budget, out);
}
static const struct polaris_smu_budget normal = {1000, 20};
static void complete(struct bus *b)
{
    CHECK(b->at == b->count && !b->mismatches);
}
static void retry_quarantine(struct polaris_smu_mailbox *ctx, struct bus *b)
{
    struct polaris_smu_observation out;
    unsigned at = b->at;
    uint64_t time = b->now;
    CHECK(run(ctx, b, normal, &out) == POLARIS_SMU_QUARANTINED);
    CHECK(out.quarantined && !out.submitted && !out.polls && !out.writes);
    CHECK(b->at == at && b->now == time);
}

static void successes(void)
{
    struct polaris_smu_mailbox ctx = {0};
    struct polaris_smu_observation out;
    struct bus b = transcript(happy, 7);
    b.reenter = &ctx;
    CHECK(run(&ctx, &b, normal, &out) == POLARIS_SMU_OK);
    CHECK(out.raw_response == 1 && out.polls == 4 && out.writes == 3);
    CHECK(out.submitted && !out.quarantined && !ctx.lock);
    complete(&b);
    /* Reuse a completed context, replacing a stale previous success with a
     * verified zero before the next message. Reserved high bits are ignored. */
    const struct event upper[] = {R(0x12340001), A, C, R(0xabcd0000), M,
                                 R(0xbeef0001)};
    b = transcript(upper, 6);
    CHECK(run(&ctx, &b, normal, &out) == POLARIS_SMU_OK);
    CHECK(out.raw_response == 0xbeef0001 && out.submitted && !out.quarantined);
    complete(&b);
}

static void preflight(void)
{
    struct polaris_smu_mailbox ctx = {0};
    struct polaris_smu_observation out;
    const struct event busy[] = {R(0), R(0), R(0)};
    struct bus b = transcript(busy, 3);
    b.step = 0; /* A stalled clock is bounded by total response polls. */
    CHECK(run(&ctx, &b, (struct polaris_smu_budget){1000,3}, &out) ==
          POLARIS_SMU_TIMEOUT);
    CHECK(!out.writes && !out.submitted && !out.quarantined && out.polls == 3);
    complete(&b);
    b = transcript(happy, 7);
    CHECK(run(&ctx, &b, normal, &out) == POLARIS_SMU_OK);
    complete(&b);
    for (unsigned polls = 1; polls < 3; polls++) {
        b = transcript(happy, 1);
        CHECK(run(&ctx, &b, (struct polaris_smu_budget){1000,polls}, &out) ==
              POLARIS_SMU_TIMEOUT);
        CHECK(!out.writes && !out.submitted && !out.quarantined);
        complete(&b);
    }
    for (unsigned i = 0; i < 5; i++) {
        const uint32_t old[] = {0xfe, 0xff, 2, 3, UINT32_MAX};
        const struct event events[] = {R(old[i])};
        b = transcript(events, 1);
        CHECK(run(&ctx, &b, normal, &out) ==
              (i == 4 ? POLARIS_SMU_IO_ERROR : POLARIS_SMU_PRIOR_ERROR));
        CHECK(!out.writes && !out.submitted && !out.quarantined);
        complete(&b);
    }
}

static void fresh_errors(void)
{
    const uint32_t errors[] = {0xfe, 0xff, 0xfd, 2, 3};
    for (unsigned i = 0; i < 5; i++) {
        struct polaris_smu_mailbox ctx = {0};
        struct polaris_smu_observation out;
        const struct event events[] = {R(1), A, C, R(0), M, R(errors[i])};
        struct bus b = transcript(events, 6);
        int rc = run(&ctx, &b, normal, &out);
        CHECK(rc == (i == 0 ? POLARIS_SMU_UNSUPPORTED : POLARIS_SMU_REJECTED));
        CHECK(out.submitted && !out.quarantined && out.raw_response == errors[i]);
        complete(&b);
    }
}

static void uncertain(void)
{
    const struct event stale[] = {R(1), A, C, R(1)};
    struct polaris_smu_mailbox ctx = {0};
    struct polaris_smu_observation out;
    struct bus b = transcript(stale, 4);
    CHECK(run(&ctx, &b, normal, &out) == POLARIS_SMU_STALE_RESPONSE);
    CHECK(!out.submitted && out.quarantined && out.writes == 2);
    complete(&b);
    retry_quarantine(&ctx, &b);

    const struct event timeout[] = {R(1), A, C, R(0), M, R(0), R(0)};
    ctx = (struct polaris_smu_mailbox){0};
    b = transcript(timeout, 7);
    b.step = 0;
    CHECK(run(&ctx, &b, (struct polaris_smu_budget){1000,4}, &out) ==
          POLARIS_SMU_TIMEOUT);
    CHECK(out.submitted && out.quarantined && out.polls == 4);
    complete(&b);
    /* A late successful response is deliberately never read by retry. */
    b = transcript(happy, 7);
    retry_quarantine(&ctx, &b);

    const struct event gone[] = {R(1), A, C, R(0), M, R(UINT32_MAX)};
    ctx = (struct polaris_smu_mailbox){0};
    b = transcript(gone, 6);
    CHECK(run(&ctx, &b, normal, &out) == POLARIS_SMU_IO_ERROR);
    CHECK(out.submitted && out.quarantined && out.raw_response == UINT32_MAX);
    complete(&b);
    retry_quarantine(&ctx, &b);

    for (unsigned i = 0; i < 7; i++) {
        ctx = (struct polaris_smu_mailbox){0};
        b = transcript(happy, i+1);
        b.fail_at = (int)i;
        CHECK(run(&ctx, &b, normal, &out) == POLARIS_SMU_IO_ERROR);
        CHECK(out.quarantined == (i > 0) && out.submitted == (i >= 4));
        complete(&b);
        if (i) retry_quarantine(&ctx, &b);
    }
}

static void clocks(void)
{
    struct polaris_smu_observation out;
    struct polaris_smu_mailbox ctx = {0};
    struct bus b = transcript(happy, 0);
    b.now = 100;
    b.backwards = 1;
    CHECK(run(&ctx, &b, normal, &out) == POLARIS_SMU_BAD_CLOCK);
    CHECK(!out.writes && !out.polls && !out.quarantined);
    complete(&b);
    /* Deadlines cover every phase, not a fresh timeout for each wait. */
    const uint64_t deadlines[] = {3,4,10};
    const unsigned events[] = {1,2,5};
    for (unsigned i = 0; i < 3; i++) {
        ctx = (struct polaris_smu_mailbox){0};
        b = transcript(happy, events[i]);
        CHECK(run(&ctx, &b, (struct polaris_smu_budget){deadlines[i],20}, &out)
              == POLARIS_SMU_TIMEOUT);
        CHECK(out.quarantined == (i > 0) && out.submitted == (i == 2));
        complete(&b);
        if (i) retry_quarantine(&ctx, &b);
    }
    ctx = (struct polaris_smu_mailbox){0};
    b = transcript(happy, 0);
    b.now = UINT64_MAX;
    CHECK(run(&ctx, &b, normal, &out) == POLARIS_SMU_BAD_CLOCK);
    CHECK(!out.writes && !out.polls);
    complete(&b);
}

static void arguments(void)
{
    struct polaris_smu_observation out;
    struct polaris_smu_mailbox ctx = {0};
    struct bus b = transcript(happy, 0);
    struct polaris_smu_io io = {&b, read_slot, write_slot, now_us};
    CHECK(polaris_smu_test(0, &io, &normal, &out) == POLARIS_SMU_BAD_ARGUMENT);
    CHECK(polaris_smu_test(&ctx, &io, &normal, 0) == POLARIS_SMU_BAD_ARGUMENT);
    CHECK(polaris_smu_test(&ctx, 0, &normal, &out) == POLARIS_SMU_BAD_ARGUMENT);
    CHECK(polaris_smu_test(&ctx, &io, 0, &out) == POLARIS_SMU_BAD_ARGUMENT);
    const struct polaris_smu_budget invalid[] = {{0,1},{1000001,1},{1,0},{1,100001}};
    for (unsigned i = 0; i < 4; i++)
        CHECK(polaris_smu_test(&ctx, &io, &invalid[i], &out) ==
              POLARIS_SMU_BAD_ARGUMENT);
    io.read = 0;
    CHECK(polaris_smu_test(&ctx, &io, &normal, &out) == POLARIS_SMU_BAD_ARGUMENT);
    io.read = read_slot; io.write = 0;
    CHECK(polaris_smu_test(&ctx, &io, &normal, &out) == POLARIS_SMU_BAD_ARGUMENT);
    io.write = write_slot; io.now_us = 0;
    CHECK(polaris_smu_test(&ctx, &io, &normal, &out) == POLARIS_SMU_BAD_ARGUMENT);
    complete(&b);
    CHECK(!b.now && !ctx.lock);
}

int main(void)
{
    successes(); preflight(); fresh_errors(); uncertain(); clocks(); arguments();
    printf("POLARIS_SMU_MAILBOX: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
