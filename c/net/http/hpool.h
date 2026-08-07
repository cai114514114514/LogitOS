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

/* ---- protocol ---------------------------------------------------------
 *
 * HTTP/1.1 and HTTP/2 want OPPOSITE pool shapes, and both have to live here.
 *
 *   HTTP/1.1: a connection carries one request at a time, so concurrency means
 *   SEVERAL connections per origin (max_per_host), each handed out to exactly
 *   one caller and returned when its response is complete.
 *
 *   HTTP/2: a connection carries every request at once, so an origin wants
 *   exactly ONE connection and the pool hands the same slot to many callers
 *   simultaneously. Opening a second is not just wasteful, it splits the HPACK
 *   compression context and the flow-control windows across two connections
 *   for no gain.
 *
 * Which one applies is decided by ALPN, i.e. AFTER the socket is dialled and
 * the TLS handshake has finished. That is why HP_PROTO_PENDING exists: between
 * dial and handshake the pool does not yet know, and a caller that dials six
 * connections during that window ends up with six h2 connections to one
 * origin. A PENDING connection therefore blocks further dials to its origin,
 * which costs a little parallelism on the first load and is what every real
 * browser does for the same reason.
 *
 * HP_PROTO_H1 is 0 so that a caller which never mentions protocols -- every
 * existing one -- gets exactly the behaviour it had before. */
#define HP_PROTO_H1       0
#define HP_PROTO_H2       1
#define HP_PROTO_PENDING  2

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
    int     proto;                 /* HP_PROTO_* */
    int     streams;               /* h2: requests in flight right now */
    int     peak_streams;          /* h2: the most ever concurrent here */
};

struct hpool {
    struct hp_conn v[HP_MAX_CONNS];
    int     max_total;             /* default 6: leaves headroom in TCP's 8 */
    int     max_per_host;          /* default 4 */
    int64_t idle_ms;               /* default 10000 */
    int     max_reqs;              /* recycle after N requests; 0 = unlimited.
                                    * h1 only: recycling an h2 connection would
                                    * cancel every stream still on it. */
    int     max_streams;           /* h2 concurrent streams per conn; default 16 */
    void  (*closer)(int fd, void *ctx, void *user);
    void   *user;
    /* observability -- a pool that silently never reuses anything looks
     * exactly like one that works, so the counters are part of the design. */
    int     hits, misses, opened, evicted, closed;
    int     mux_hits;              /* streams served on an already-open h2 conn */
    int     h2_conns;              /* connections that negotiated h2 */
};

void hpool_init(struct hpool *p);
void hpool_config(struct hpool *p, int max_total, int max_per_host, int64_t idle_ms);
void hpool_set_closer(struct hpool *p, void (*fn)(int fd, void *ctx, void *user), void *user);

/* Take an idle HTTP/1.1 connection to this origin, marking it in-use. Returns
 * the slot index, or -1 if there is none (the caller must dial). Never returns
 * a connection to a different host, port, or TLS-ness -- and never returns an
 * h2 connection, because an HTTP/1.1 request written onto one is framed as a
 * DATA frame's worth of garbage. */
int  hpool_acquire(struct hpool *p, const char *host, int port, int tls, int64_t now);

/* Take a STREAM on the origin's existing h2 connection. Unlike hpool_acquire
 * this hands the same slot to many callers at once -- that is the point -- and
 * succeeds while the connection is already busy. Returns the slot, or -1 if
 * the origin has no h2 connection or that connection is at max_streams. */
int  hpool_acquire_mux(struct hpool *p, const char *host, int port, int tls, int64_t now);

/* Register the protocol once ALPN has reported it. Until this is called a
 * connection admitted as HP_PROTO_PENDING blocks further dials to its origin,
 * so forgetting to call it wedges that origin at one connection. */
void hpool_set_proto(struct hpool *p, int slot, int proto);
int  hpool_proto(const struct hpool *p, int slot);
int  hpool_streams(const struct hpool *p, int slot);
/* How many connections this origin has that are h2 or not yet decided. */
int  hpool_count_mux(const struct hpool *p, const char *host, int port, int tls);

/* May the caller open a NEW connection to this origin right now? Expires stale
 * idle connections and, if the total cap is in the way, evicts the oldest idle
 * connection of another origin to make room. 1 yes, 0 no (try again later). */
int  hpool_may_open(struct hpool *p, const char *host, int port, int tls, int64_t now);

/* Register a freshly dialled connection, already in-use. Returns the slot, or
 * -1 if the caps or the table are full (the caller should close its socket). */
int  hpool_admit(struct hpool *p, const char *host, int port, int tls,
                 int fd, void *ctx, int64_t now);
/* Same, naming the protocol. Pass HP_PROTO_PENDING for a TLS connection whose
 * ALPN has not resolved yet, then hpool_set_proto when it has. */
int  hpool_admit_proto(struct hpool *p, const char *host, int port, int tls,
                       int fd, void *ctx, int proto, int64_t now);

/* Give a connection back. `reusable` should be the response's keep_alive:
 * 0 closes it immediately. On an h2 connection this releases ONE stream and
 * the connection stays open and in use while other streams are still running;
 * `reusable` = 0 still tears the whole connection down, which is correct --
 * an h2 connection is only unusable for reasons that affect every stream on
 * it (GOAWAY, a compression error, a dead socket). */
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
