/* poll() on the host: the real c/kernel/exec/kpoll.c and the real
 * c/kernel/core/wait.c, driven by a model object, on a modelled scheduler
 * (tests/unit/pollhost/hostsched.c -- read its header for what is real).
 *
 * WHY A MODEL OBJECT RATHER THAN A REAL PIPE. c/kernel/exec/file.c cannot be
 * compiled for the host: kheap, vfs, serial, percpu and the big kernel lock all
 * sit behind it. A poll gate that could only run through file.c would have to
 * boot QEMU, and the one property that matters here -- that an event arriving
 * between the readiness check and the sleep is not lost -- would then be tested
 * by HOPING the race happens on a machine where the whole window is a few
 * hundred instructions. The device half (tests/boot/run-poll-test.sh) checks
 * that the real backends answer correctly; this half checks the thing that is
 * hard, and causes it on purpose.
 *
 * THE LOST WAKEUP IS PROVOKED, NOT WAITED FOR, and check 12 below is the whole
 * reason this file exists. The model's readiness function computes its mask and
 * THEN pushes a byte -- so the event lands at exactly the instruction between
 * "what is your state" and "nobody was ready, park". In a correct build the
 * registration already happened, the push's waitq_wake_all finds it, and
 * poll_core never parks. In -DPOLL_NO_PREREGISTER the registration has not
 * happened yet, the wake reaches nobody, and the call sleeps until its timeout
 * with data sitting in the pipe. Deterministic in both directions: no sleep, no
 * retry, no "usually".
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "kpoll.h"
#include "kernel/core/wait.h"

void hostsched_init(void);

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    if (ok) { g_pass++; }
    else    { g_fail++; printf("  FAIL %s\n", what); }
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ==========================================================================
 * The model object: a pipe with a count, a writer refcount and one wait queue,
 * which is the shape c/kernel/exec/file.c's struct pipe has. Its readiness
 * function obeys the two contracts kpoll.h states -- poll_wait() first, and
 * every state change wakes the queue with wake_ALL.
 * ======================================================================== */
struct mpipe {
    struct waitq wq;
    int count;
    int writers;
    int readers;
    int cap;
    int inject;      /* one-shot: push a byte AFTER the mask is computed */
};

static void mp_init(struct mpipe *p)
{
    waitq_init(&p->wq);
    p->count = 0; p->writers = 1; p->readers = 1; p->cap = 8; p->inject = 0;
}

static void mp_push(struct mpipe *p)
{
    uint64_t f = spin_lock_irqsave(&p->wq.lock);
    if (p->count < p->cap) p->count++;
    spin_unlock_irqrestore(&p->wq.lock, f);
    waitq_wake_all(&p->wq);
}

static void mp_close_writer(struct mpipe *p)
{
    uint64_t f = spin_lock_irqsave(&p->wq.lock);
    p->writers = 0;
    spin_unlock_irqrestore(&p->wq.lock, f);
    waitq_wake_all(&p->wq);
}

static short mp_read_ready(void *obj, struct poll_table *pt)
{
    struct mpipe *p = (struct mpipe *)obj;
    poll_wait(pt, &p->wq);                 /* FIRST -- the contract */

    short m = 0;
    if (p->count > 0)   m |= LPOLLIN;
    if (p->writers == 0) m |= LPOLLHUP;

    /* THE PROBE. Fires once, after the mask above has been decided, which is
     * precisely the window a naive implementation leaves open. */
    if (p->inject) { p->inject = 0; mp_push(p); }
    return m;
}

static short mp_write_ready(void *obj, struct poll_table *pt)
{
    struct mpipe *p = (struct mpipe *)obj;
    poll_wait(pt, &p->wq);
    if (p->readers == 0) return LPOLLERR;
    return p->count < p->cap ? LPOLLOUT : 0;
}

/* A backend that asks for more registrations than the table holds. Nothing in
 * the tree does this -- it is here because POLL_E_NOMEM must be reachable and
 * demonstrated, or it is a branch nobody has ever seen taken. */
static struct waitq g_extra[POLL_MAXWAIT + 4];
static short greedy_ready(void *obj, struct poll_table *pt)
{
    (void)obj;
    for (int i = 0; i < POLL_MAXWAIT + 4; i++) poll_wait(pt, &g_extra[i]);
    return 0;
}

/* --- helper threads ----------------------------------------------------- */

struct delayed { struct mpipe *p; int ms; int close_it; };

static void *delayed_fn(void *arg)
{
    struct delayed *d = (struct delayed *)arg;
    struct timespec ts = { d->ms / 1000, (long)(d->ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
    if (d->close_it) mp_close_writer(d->p);
    else             mp_push(d->p);
    return NULL;
}

static pthread_t spawn_delayed(struct delayed *d)
{
    pthread_t t;
    pthread_create(&t, NULL, delayed_fn, d);
    return t;
}

/* The watchdog for the lost-wakeup check. It exists so that a build with the
 * bug fails LOUDLY instead of wedging the gate: an infinite poll that never
 * returns is the correct diagnosis and a useless test result. */
static struct mpipe *g_wd_pipe;
static volatile int  g_wd_done, g_wd_rescued;

static void *watchdog_fn(void *arg)
{
    (void)arg;
    struct timespec ts = { 0, 600 * 1000000L };
    nanosleep(&ts, NULL);
    if (!__atomic_load_n(&g_wd_done, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&g_wd_rescued, 1, __ATOMIC_SEQ_CST);
        mp_push(g_wd_pipe);          /* release the wedged poll */
    }
    return NULL;
}

static int q_empty(struct mpipe *p)
{
    uint64_t f = spin_lock_irqsave(&p->wq.lock);
    int e = (p->wq.head == NULL && p->wq.tail == NULL);
    spin_unlock_irqrestore(&p->wq.lock, f);
    return e;
}

int main(void)
{
    hostsched_init();
    for (int i = 0; i < POLL_MAXWAIT + 4; i++) waitq_init(&g_extra[i]);

    struct mpipe a, b, c;
    mp_init(&a); mp_init(&b); mp_init(&c);
    struct pollsrc s[3];
    int rc;
    uint64_t t0, dt;

    printf("poll host gate\n");

    /* 1-3: the pure probe. timeout 0 never sleeps and never registers. */
    s[0].obj = &a; s[0].ready = mp_read_ready; s[0].events = LPOLLIN;
    t0 = now_ms();
    rc = poll_core(s, 1, 0);
    dt = now_ms() - t0;
    check(rc == 0, "probe on an empty pipe returns 0");
    check(dt < 50, "probe does not sleep");
    check(q_empty(&a), "probe left no registration behind");

    /* 4-5: the probe with data. */
    mp_push(&a);
    s[0].events = LPOLLIN;
    rc = poll_core(s, 1, 0);
    check(rc == 1, "probe with data returns 1");
    check(s[0].revents == LPOLLIN, "probe with data reports LPOLLIN");

    /* 6: a mask the caller did not ask for is not reported. `a` has data, and
     * the request is LPOLLOUT only -- which a read end never has. */
    s[0].events = LPOLLOUT;
    rc = poll_core(s, 1, 0);
    check(rc == 0 && s[0].revents == 0, "LPOLLIN is not reported when only LPOLLOUT was asked");

    /* 7-8: LPOLLHUP arrives WITHOUT being requested, which is what makes a
     * poll loop over a dead pipe terminate. Asked for LPOLLOUT; told HUP. */
    mp_init(&b);
    mp_close_writer(&b);
    s[0].obj = &b; s[0].ready = mp_read_ready; s[0].events = LPOLLOUT;
    rc = poll_core(s, 1, 0);
    check(rc == 1, "a hung-up pipe is ready even though only LPOLLOUT was asked");
    check(s[0].revents == LPOLLHUP, "and what it reports is exactly LPOLLHUP");

    /* 9-10: a source with no answering function is LPOLLNVAL, and it RETURNS --
     * with an infinite timeout, which is the case that hangs if this is wrong. */
    s[0].obj = NULL; s[0].ready = NULL; s[0].events = LPOLLIN;
    t0 = now_ms();
    rc = poll_core(s, 1, -1);
    dt = now_ms() - t0;
    check(rc == 1 && s[0].revents == LPOLLNVAL, "a dead source is LPOLLNVAL");
    check(dt < 100, "and an infinite poll over it returns at once instead of hanging");

    /* 11-12: the timeout. An empty pipe, 200 ms, nothing ever arrives. */
    mp_init(&a);
    s[0].obj = &a; s[0].ready = mp_read_ready; s[0].events = LPOLLIN;
    t0 = now_ms();
    rc = poll_core(s, 1, 200);
    dt = now_ms() - t0;
    check(rc == 0, "a poll that times out returns 0");
    check(dt >= 180 && dt < 900, "and it waited about the time it was told");

    /* 13-16: blocked, then woken by data on ONE of three. Only that one is
     * reported ready -- a poll that returned all three would be useless. */
    mp_init(&a); mp_init(&b); mp_init(&c);
    s[0].obj = &a; s[0].ready = mp_read_ready; s[0].events = LPOLLIN;
    s[1].obj = &b; s[1].ready = mp_read_ready; s[1].events = LPOLLIN;
    s[2].obj = &c; s[2].ready = mp_read_ready; s[2].events = LPOLLIN;
    struct delayed d1 = { &b, 120, 0 };
    pthread_t th = spawn_delayed(&d1);
    t0 = now_ms();
    rc = poll_core(s, 3, -1);
    dt = now_ms() - t0;
    pthread_join(th, NULL);
    check(rc == 1, "an infinite poll over three is woken by data on one");
    check(s[1].revents == LPOLLIN, "and the one it names is the one that got the data");
    check(s[0].revents == 0 && s[2].revents == 0, "the other two are not reported ready");
    check(q_empty(&a) && q_empty(&b) && q_empty(&c),
          "all three registrations were taken back off their queues");

    /* 17-18: blocked, then woken by the WRITER CLOSING. */
    mp_init(&a);
    s[0].obj = &a; s[0].ready = mp_read_ready; s[0].events = LPOLLIN;
    struct delayed d2 = { &a, 120, 1 };
    th = spawn_delayed(&d2);
    rc = poll_core(s, 1, -1);
    pthread_join(th, NULL);
    check(rc == 1, "a blocked poll is woken by the last writer closing");
    check(s[0].revents == LPOLLHUP, "and reports LPOLLHUP");

    /* 19: the write end of a pipe whose readers are gone is LPOLLERR, not
     * LPOLLOUT -- reporting writable would send the caller into a write that
     * kills it with SIGPIPE. */
    mp_init(&a);
    a.readers = 0;
    s[0].obj = &a; s[0].ready = mp_write_ready; s[0].events = LPOLLOUT;
    rc = poll_core(s, 1, 0);
    check(rc == 1 && s[0].revents == LPOLLERR, "a write end with no readers is LPOLLERR");

    /* ======================================================================
     * 20-23: THE LOST WAKEUP.
     *
     * `inject` makes mp_read_ready push a byte AFTER it has computed a mask of
     * zero -- the exact instruction between "nothing is ready" and "park". A
     * correct poll_core has already registered on the queue by then, so the
     * push's waitq_wake_all sets the poll table's flag and the park is skipped.
     * -DPOLL_NO_PREREGISTER has not registered yet, so the wake reaches nobody.
     *
     * THE TIMEOUT IS -1, WHICH IS THE HONEST SEVERITY: the bug is not "slow",
     * it is "never". A finite timeout was tried first and made a poor control
     * -- poll_core re-scans at the top of every pass, so after the timeout
     * expired it found the data and returned the RIGHT answer, merely 800 ms
     * late, and only one check of three reddened. That reads as a latency
     * problem. It is a hang.
     *
     * A WATCHDOG makes the hang observable without hanging the gate: it sleeps
     * 600 ms and, if the poll has not returned, pushes a second byte to release
     * it and records that it had to. `rescued` is therefore the check that
     * matters -- a correct build returns in under a millisecond and the
     * watchdog finds nothing to do.
     * ==================================================================== */
    mp_init(&a);
    a.inject = 1;
    s[0].obj = &a; s[0].ready = mp_read_ready; s[0].events = LPOLLIN;
    g_wd_pipe = &a;
    g_wd_done = 0;
    g_wd_rescued = 0;
    pthread_t wd;
    pthread_create(&wd, NULL, watchdog_fn, NULL);
    t0 = now_ms();
    rc = poll_core(s, 1, -1);
    dt = now_ms() - t0;
    __atomic_store_n(&g_wd_done, 1, __ATOMIC_SEQ_CST);
    pthread_join(wd, NULL);
    check(rc == 1, "LOST WAKEUP: data arriving between the check and the sleep is not lost");
    check(s[0].revents == LPOLLIN, "LOST WAKEUP: and the fd is reported readable");
    check(__atomic_load_n(&g_wd_rescued, __ATOMIC_SEQ_CST) == 0,
          "LOST WAKEUP: an INFINITE poll returned on its own -- the watchdog was not needed");
    check(dt < 400, "LOST WAKEUP: it returned promptly rather than hanging");

    /* 23: more registrations than the table holds is refused OUT LOUD. A
     * partial registration followed by a sleep is the bug this whole design
     * exists to prevent, so the branch has to be reachable and seen. */
    s[0].obj = NULL; s[0].ready = greedy_ready; s[0].events = LPOLLIN;
    rc = poll_core(s, 1, -1);
    check(rc == POLL_E_NOMEM, "a registration overflow is POLL_E_NOMEM, not a sleep");

    /* 24-25: a poll of nothing is a sleep, and POSIX says so. */
    t0 = now_ms();
    rc = poll_core(NULL, 0, 150);
    dt = now_ms() - t0;
    check(rc == 0, "poll of zero sources returns 0");
    check(dt >= 130 && dt < 900, "poll of zero sources is a sleep of the requested length");

    printf("poll host gate: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) { printf("POLL-HOST FAIL\n"); return 1; }
    printf("POLL-HOST OK %d checks\n", g_pass);
    return 0;
}
