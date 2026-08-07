/* Host unit test for c/net/http/hpool.c -- the HTTP connection pool.
 *
 * Two things are being proved. The first is that reuse happens at all: with
 * Connection: close on every request (what the kernel client does today) a
 * 40-resource page costs 40 TLS handshakes against a TCP stack that has 8
 * connection slots, so the pool is a feasibility requirement rather than a
 * speed-up. The second, and the one worth testing hard, is that reuse NEVER
 * crosses an origin -- handing example.com's connection to evil.com would send
 * the request, Cookie header and all, to the wrong peer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "hpool.h"

static int fails;
#define OK(cond) do { if (cond) printf("ok   %s\n", #cond); \
                      else { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

/* The pool owns no sockets, so the test's "socket" is a counter. */
static int closed_fds[64], nclosed;
static void rec_close(int fd, void *ctx, void *user)
{
    (void)ctx; (void)user;
    if (nclosed < 64) closed_fds[nclosed++] = fd;
}
static int was_closed(int fd)
{
    for (int i = 0; i < nclosed; i++) if (closed_fds[i] == fd) return 1;
    return 0;
}

static void t_reuse(void)
{
    struct hpool p;
    hpool_init(&p);
    hpool_set_closer(&p, rec_close, NULL);
    nclosed = 0;

    OK(hpool_acquire(&p, "example.com", 443, 1, 1000) == -1);   /* cold pool */
    int s = hpool_admit(&p, "example.com", 443, 1, 7, NULL, 1000);
    OK(s >= 0 && hpool_fd(&p, s) == 7);
    OK(hpool_count(&p) == 1 && hpool_idle_count(&p) == 0);

    /* An in-use connection is not handed out twice. */
    OK(hpool_acquire(&p, "example.com", 443, 1, 1001) == -1);

    hpool_release(&p, s, 1, 1002);
    OK(hpool_idle_count(&p) == 1);

    int s2 = hpool_acquire(&p, "example.com", 443, 1, 1003);
    OK(s2 == s && hpool_fd(&p, s2) == 7);                       /* same connection */
    OK(p.hits == 1 && p.opened == 1);
    OK(!was_closed(7));

    hpool_release(&p, s2, 1, 1004);
    hpool_close_all(&p);
    OK(was_closed(7));
}

static void t_no_cross_origin(void)
{
    struct hpool p;
    hpool_init(&p);
    hpool_set_closer(&p, rec_close, NULL);
    nclosed = 0;

    int s = hpool_admit(&p, "example.com", 443, 1, 11, NULL, 1000);
    hpool_release(&p, s, 1, 1000);

    /* Every axis of the key, separately. A pool that ignores any one of them
     * sends the request to the wrong peer. */
    OK(hpool_acquire(&p, "evil.com", 443, 1, 1001) == -1);          /* other host */
    OK(hpool_acquire(&p, "a.example.com", 443, 1, 1001) == -1);     /* subdomain is not the same origin */
    OK(hpool_acquire(&p, "example.com", 8443, 1, 1001) == -1);      /* other port */
    OK(hpool_acquire(&p, "example.com", 443, 0, 1001) == -1);       /* https conn, http request */
    OK(hpool_acquire(&p, "example.com.evil.com", 443, 1, 1001) == -1);
    OK(hpool_acquire(&p, "xample.com", 443, 1, 1001) == -1);
    /* Host comparison is case-insensitive, which is the one place where being
     * strict would be wrong (DNS names are). */
    OK(hpool_acquire(&p, "EXAMPLE.COM", 443, 1, 1001) == s);
    hpool_release(&p, s, 1, 1002);

    OK(hpool_count_origin(&p, "example.com", 443, 1) == 1);
    OK(hpool_count_origin(&p, "example.com", 443, 0) == 0);
    OK(hpool_count_origin(&p, "example.com", 80, 1) == 0);
    hpool_close_all(&p);
}

static void t_per_host_cap(void)
{
    struct hpool p;
    hpool_init(&p);
    hpool_set_closer(&p, rec_close, NULL);
    hpool_config(&p, 10, 3, 10000);
    nclosed = 0;

    int s[5];
    for (int i = 0; i < 3; i++) {
        s[i] = hpool_admit(&p, "example.com", 443, 1, 100 + i, NULL, 1000);
        OK(s[i] >= 0);
    }
    /* The 4th to the same origin is refused: a slow host must not be able to
     * consume every slot the whole page has to share. */
    OK(hpool_may_open(&p, "example.com", 443, 1, 1000) == 0);
    OK(hpool_admit(&p, "example.com", 443, 1, 103, NULL, 1000) == -1);

    /* A different origin still gets one. */
    OK(hpool_may_open(&p, "other.com", 443, 1, 1000) == 1);
    int o = hpool_admit(&p, "other.com", 443, 1, 200, NULL, 1000);
    OK(o >= 0);
    OK(hpool_count(&p) == 4);

    /* Freeing one lets the origin open another. */
    hpool_release(&p, s[0], 0, 1001);
    OK(was_closed(100));
    OK(hpool_may_open(&p, "example.com", 443, 1, 1001) == 1);
    hpool_close_all(&p);
}

static void t_total_cap_and_eviction(void)
{
    struct hpool p;
    hpool_init(&p);
    hpool_set_closer(&p, rec_close, NULL);
    hpool_config(&p, 3, 3, 10000);
    nclosed = 0;

    int a = hpool_admit(&p, "a.test", 80, 0, 1, NULL, 1000);
    int b = hpool_admit(&p, "b.test", 80, 0, 2, NULL, 1000);
    int c = hpool_admit(&p, "c.test", 80, 0, 3, NULL, 1000);
    OK(a >= 0 && b >= 0 && c >= 0 && hpool_count(&p) == 3);

    /* Full and everything busy: a new origin has to wait. Evicting an in-use
     * connection would abort a request in flight. */
    OK(hpool_may_open(&p, "d.test", 80, 0, 1001) == 0);

    /* Idle ones are fair game -- the oldest idle first. */
    hpool_release(&p, b, 1, 1100);       /* idle since 1100 */
    hpool_release(&p, c, 1, 1200);       /* idle since 1200 */
    OK(hpool_may_open(&p, "d.test", 80, 0, 1300) == 1);
    OK(was_closed(2) && !was_closed(3));
    int d = hpool_admit(&p, "d.test", 80, 0, 4, NULL, 1300);
    OK(d >= 0 && hpool_count(&p) == 3);
    OK(p.evicted >= 1);
    hpool_close_all(&p);
}

static void t_idle_expiry(void)
{
    struct hpool p;
    hpool_init(&p);
    hpool_set_closer(&p, rec_close, NULL);
    hpool_config(&p, 6, 4, 5000);
    nclosed = 0;

    int a = hpool_admit(&p, "a.test", 80, 0, 21, NULL, 1000);
    int b = hpool_admit(&p, "b.test", 80, 0, 22, NULL, 1000);
    hpool_release(&p, a, 1, 1000);
    /* b stays in use: expiry must never touch a connection mid-request. */
    OK(hpool_expire(&p, 3000) == 0);
    OK(hpool_expire(&p, 6000) == 1);
    OK(was_closed(21) && !was_closed(22));
    OK(hpool_count(&p) == 1);

    /* A stale idle connection is not handed out: a server that reaped it looks
     * exactly like a live one until the first write fails. */
    int c = hpool_admit(&p, "c.test", 80, 0, 23, NULL, 7000);
    hpool_release(&p, c, 1, 7000);
    OK(hpool_acquire(&p, "c.test", 80, 0, 8000) >= 0);          /* still fresh */
    hpool_release(&p, c, 1, 8000);
    OK(hpool_acquire(&p, "c.test", 80, 0, 20000) == -1);        /* aged out */
    OK(was_closed(23));
    (void)b;
    hpool_close_all(&p);
}

static void t_release_and_recycle(void)
{
    struct hpool p;
    hpool_init(&p);
    hpool_set_closer(&p, rec_close, NULL);
    nclosed = 0;

    /* reusable == 0 is what a response with Connection: close, or with a
     * TE/CL framing conflict, hands back. It must actually close. */
    int s = hpool_admit(&p, "a.test", 80, 0, 31, NULL, 1000);
    hpool_release(&p, s, 0, 1000);
    OK(was_closed(31) && hpool_count(&p) == 0);

    /* Bounded reuse: a connection is recycled after max_reqs so a long-lived
     * page cannot pin one forever. */
    hpool_init(&p);
    hpool_set_closer(&p, rec_close, NULL);
    p.max_reqs = 3;
    nclosed = 0;
    s = hpool_admit(&p, "a.test", 80, 0, 32, NULL, 1000);        /* reqs = 1 */
    hpool_release(&p, s, 1, 1000);
    s = hpool_acquire(&p, "a.test", 80, 0, 1000);               /* reqs = 2 */
    OK(s >= 0);
    hpool_release(&p, s, 1, 1000);
    s = hpool_acquire(&p, "a.test", 80, 0, 1000);               /* reqs = 3 */
    OK(s >= 0);
    hpool_release(&p, s, 1, 1000);                              /* hits the cap */
    OK(was_closed(32) && hpool_count(&p) == 0);

    /* drop() is the error path: close and forget, whatever the state. */
    hpool_init(&p);
    hpool_set_closer(&p, rec_close, NULL);
    nclosed = 0;
    s = hpool_admit(&p, "a.test", 80, 0, 33, NULL, 1000);
    hpool_drop(&p, s);
    OK(was_closed(33) && hpool_count(&p) == 0);
    /* Out-of-range and double operations are no-ops, not corruption. */
    hpool_drop(&p, s);
    hpool_drop(&p, -1);
    hpool_drop(&p, HP_MAX_CONNS + 5);
    hpool_release(&p, -1, 1, 0);
    hpool_release(&p, HP_MAX_CONNS, 1, 0);
    OK(hpool_fd(&p, -1) == -1 && hpool_fd(&p, HP_MAX_CONNS) == -1 && hpool_fd(&p, s) == -1);
    OK(hpool_ctx(&p, s) == NULL);
    hpool_close_all(&p);
}

static void t_freshest_first(void)
{
    struct hpool p;
    hpool_init(&p);
    hpool_set_closer(&p, rec_close, NULL);
    nclosed = 0;

    int a = hpool_admit(&p, "a.test", 80, 0, 41, NULL, 1000);
    int b = hpool_admit(&p, "a.test", 80, 0, 42, NULL, 1000);
    hpool_release(&p, a, 1, 1000);          /* idle longer */
    hpool_release(&p, b, 1, 2000);          /* most recently idled */
    /* Prefer the freshest: it is the one the far end is least likely to have
     * already reaped, so it is the one least likely to fail on first write. */
    OK(hpool_acquire(&p, "a.test", 80, 0, 2001) == b);
    hpool_close_all(&p);
}

static void t_bounds(void)
{
    struct hpool p;
    hpool_init(&p);
    hpool_set_closer(&p, rec_close, NULL);
    nclosed = 0;

    char toolong[HP_HOST_MAX + 32];
    memset(toolong, 'a', sizeof toolong - 1);
    toolong[sizeof toolong - 1] = 0;
    OK(hpool_admit(&p, toolong, 80, 0, 1, NULL, 1000) == -1);
    OK(hpool_may_open(&p, toolong, 80, 0, 1000) == 0);
    OK(hpool_admit(&p, "", 80, 0, 1, NULL, 1000) == -1);
    OK(hpool_admit(&p, NULL, 80, 0, 1, NULL, 1000) == -1);
    OK(hpool_acquire(&p, NULL, 80, 0, 1000) == -1);
    OK(hpool_count(&p) == 0);

    /* The table ceiling holds even if config asks for more. */
    hpool_config(&p, 1000, 1000, 1000);
    OK(p.max_total == HP_MAX_CONNS);
    int admitted = 0;
    for (int i = 0; i < HP_MAX_CONNS + 8; i++) {
        char h[32];
        sprintf(h, "h%d.test", i);
        if (hpool_admit(&p, h, 80, 0, 500 + i, NULL, 1000) >= 0) admitted++;
    }
    OK(admitted == HP_MAX_CONNS && hpool_count(&p) == HP_MAX_CONNS);
    hpool_close_all(&p);
    OK(hpool_count(&p) == 0);

    /* A pool with no closer configured must not crash on close. */
    struct hpool q;
    hpool_init(&q);
    int s = hpool_admit(&q, "a.test", 80, 0, 1, NULL, 0);
    hpool_release(&q, s, 0, 0);
    hpool_close_all(&q);
    OK(hpool_count(&q) == 0);

    /* NULL pool everywhere. */
    hpool_init(NULL);
    hpool_config(NULL, 1, 1, 1);
    hpool_set_closer(NULL, NULL, NULL);
    OK(hpool_count(NULL) == 0);
    OK(hpool_idle_count(NULL) == 0);
    OK(hpool_count_origin(NULL, "a", 1, 0) == 0);
    OK(hpool_acquire(NULL, "a", 1, 0, 0) == -1);
    OK(hpool_may_open(NULL, "a", 1, 0, 0) == 0);
    OK(hpool_admit(NULL, "a", 1, 0, 1, NULL, 0) == -1);
    OK(hpool_expire(NULL, 0) == 0);
    OK(hpool_fd(NULL, 0) == -1);
    OK(hpool_ctx(NULL, 0) == NULL);
    hpool_release(NULL, 0, 1, 0);
    hpool_drop(NULL, 0);
    hpool_close_all(NULL);
    printf("ok   NULL pool is inert everywhere\n");
}

/* A 40-resource page over the default pool: this is the shape the browser will
 * actually produce, and the assertion is that it never exceeds the caps while
 * still reusing rather than reconnecting. */
static void t_page_shaped_load(void)
{
    struct hpool p;
    hpool_init(&p);
    hpool_set_closer(&p, rec_close, NULL);
    nclosed = 0;

    const char *origins[3] = { "example.com", "cdn.example.com", "fonts.test" };
    int fd_next = 1000;
    int64_t t = 0;
    int dialled = 0, reused = 0, deferred = 0;

    for (int i = 0; i < 40; i++) {
        const char *h = origins[i % 3];
        t += 10;
        int s = hpool_acquire(&p, h, 443, 1, t);
        if (s >= 0) { reused++; }
        else if (hpool_may_open(&p, h, 443, 1, t)) {
            s = hpool_admit(&p, h, 443, 1, fd_next++, NULL, t);
            if (s >= 0) dialled++; else { deferred++; continue; }
        } else { deferred++; continue; }
        if (hpool_count(&p) > p.max_total) { printf("FAIL exceeded total cap\n"); fails++; }
        if (hpool_count_origin(&p, h, 443, 1) > p.max_per_host) {
            printf("FAIL exceeded per-host cap\n"); fails++;
        }
        hpool_release(&p, s, 1, t);      /* keep-alive response */
    }
    OK(dialled == 3);                    /* one connection per origin, then reuse */
    OK(reused == 37);
    OK(deferred == 0);
    OK(hpool_count(&p) == 3);
    printf("ok   40 resources over 3 origins: %d dials, %d reuses\n", dialled, reused);
    hpool_close_all(&p);
}

int main(void)
{
    t_reuse();
    t_no_cross_origin();
    t_per_host_cap();
    t_total_cap_and_eviction();
    t_idle_expiry();
    t_release_and_recycle();
    t_freshest_first();
    t_bounds();
    t_page_shaped_load();

    if (fails) { printf("\n%d FAILURES\n", fails); return 1; }
    printf("\nhpool_test: ALL PASS\n");
    return 0;
}
