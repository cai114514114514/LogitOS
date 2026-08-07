#ifndef LOGIT_HPOOL_H
#define LOGIT_HPOOL_H

#include <stdint.h>

/* hpool -- HTTP connection pool, keyed by origin (host, port, tls).
 *
 * This is not a performance nicety, it is a feasibility requirement.  The TCP
 * stack has NCONN = 8 connection slots for the whole machine.  A real page
 * wants 40 sub-resources.  The kernel client sends `Connection: close` on
 * every request, so those 40 resources cost 40 connects and -- over https --
 * 40 full TLS handshakes, each with an RSA or ECDSA chain verification done
 * in software on an emulated CPU.  That is the difference between a page that
 * loads and a page that does not.
 *
 * The pool therefore does three things:
 *   - REUSE an idle connection to the same origin (keep-alive),
 *   - CAP how many connections one origin may hold, so a slow host cannot
 *     starve every other origin out of the 8 slots,
 *   - EVICT idle connections, because a connection the server has silently
 *     dropped looks identical to an idle one until you write to it.
 *
 * The pool owns no sockets.  It stores an opaque `fd` plus a `ctx` pointer and
 * calls a closer callback when it drops one, so it works for plain TCP, TLS,
 * and an in-memory test transport without knowing which is which -- the same
 * reason http1.c takes a transport vtable.
 */

#define HP_HOST_MAX   128
#define HP_MAX_CONNS   16          /* hard ceiling on the table */

struct hp_conn {
    char    host[HP_HOST_MAX];     /* canonical, lowercase */
    int     port;
    int     tls;
    int     fd;                    /* opaque handle owned by the caller */
    void   *ctx;                   /* opaque transport context */
    int     in_use;
    int     reqs;                  /* requests served on this connection */
    int64_t idle_since;            /* ms timestamp, valid when !in_use */
    int     used;                  /* slot occupied */
};

struct hpool {
    struct hp_conn v[HP_MAX_CONNS];
    int     max_total;             /* default 6: leaves headroom in TCP's 8 */
    int     max_per_host;          /* default 4 */
    int64_t idle_ms;               /* default 10000 */
    int     max_reqs;              /* recycle after N requests; 0 = unlimited */
    void  (*closer)(int fd, void *ctx, void *user);
    void   *user;
    /* observability -- a pool that silently never reuses anything looks
     * exactly like one that works, so the counters are part of the design. */
    int     hits, misses, opened, evicted, closed;
};

void hpool_init(struct hpool *p);
void hpool_config(struct hpool *p, int max_total, int max_per_host, int64_t idle_ms);
void hpool_set_closer(struct hpool *p, void (*fn)(int fd, void *ctx, void *user), void *user);

/* Take an idle connection to this origin, marking it in-use. Returns the slot
 * index, or -1 if there is none (the caller must dial). Never returns a
 * connection to a different host, port, or TLS-ness. */
int  hpool_acquire(struct hpool *p, const char *host, int port, int tls, int64_t now);

/* May the caller open a NEW connection to this origin right now? Expires stale
 * idle connections and, if the total cap is in the way, evicts the oldest idle
 * connection of another origin to make room. 1 yes, 0 no (try again later). */
int  hpool_may_open(struct hpool *p, const char *host, int port, int tls, int64_t now);

/* Register a freshly dialled connection, already in-use. Returns the slot, or
 * -1 if the caps or the table are full (the caller should close its socket). */
int  hpool_admit(struct hpool *p, const char *host, int port, int tls,
                 int fd, void *ctx, int64_t now);

/* Give a connection back. `reusable` should be the response's keep_alive:
 * 0 closes it immediately. */
void hpool_release(struct hpool *p, int slot, int reusable, int64_t now);
/* Close and forget a connection (error path). */
void hpool_drop(struct hpool *p, int slot);
/* Close idle connections older than idle_ms. Returns how many. */
int  hpool_expire(struct hpool *p, int64_t now);
void hpool_close_all(struct hpool *p);

int  hpool_count(const struct hpool *p);
int  hpool_count_origin(const struct hpool *p, const char *host, int port, int tls);
int  hpool_idle_count(const struct hpool *p);
int  hpool_fd(const struct hpool *p, int slot);
void *hpool_ctx(const struct hpool *p, int slot);

#endif /* LOGIT_HPOOL_H */
