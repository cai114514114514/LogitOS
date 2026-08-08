#ifndef LOGIT_BFETCH_H
#define LOGIT_BFETCH_H

/* bfetch -- the browser's resource fetcher, in ring 3.
 *
 * WHY THIS EXISTS, in one measurement: a single load of
 * en.wikipedia.org/wiki/Operating_system did 16 full TLS handshakes with zero
 * reuse -- eight of them for eight images from the SAME host -- because every
 * sub-resource went through SYS_RES_FETCH, and the kernel client behind it
 * sends `Connection: close` on every request and can only have one request in
 * flight on the whole machine. Each of those handshakes is an RSA or ECDSA
 * chain verification done in software on an emulated CPU, and the window
 * manager could not even poll the network for the duration.
 *
 * This replaces that path with the pieces that landed today and were not yet
 * wired to anything:
 *
 *   - the non-blocking socket ABI (sock_open/poll/send/recv/close): nothing
 *     here waits in the kernel, so N requests genuinely progress together and
 *     the WM keeps running net_poll -- which is what advances them;
 *   - c/net/http/http1.c: a real HTTP/1.1 client whose response parser never
 *     blocks, so "feed it whatever bytes arrived" is the normal case rather
 *     than a special one;
 *   - c/net/http/hpool.c: keep-alive connection pooling keyed by origin, which
 *     is where the handshake count actually goes away.
 *
 * The interface is a small request table. Start as many as you like; they are
 * queued behind the pool's caps rather than refused, and bfetch_pump() moves
 * every one of them forward one step without ever blocking.
 */

enum { BF_PENDING = 0, BF_DONE = 1, BF_FAILED = -1 };

void bfetch_init(void);

/* The document URL every relative reference resolves against. */
void bfetch_set_base(const char *page_url);

/* Resolve `ref` against `base` (or the document base when `base` is NULL) into
 * an absolute URL, with RFC 3986 dot-segment removal applied -- url.c's
 * url_resolve does not do that, and bundlers emit "./chunk.js" constantly. */
int  bfetch_resolve(const char *base, const char *ref, char *out, int max);

/* Queue a GET. Returns a request id, or -1 (bad URL / table full). */
int  bfetch_start(const char *ref);
/* As above, but resolved against `base` instead of the document URL. */
int  bfetch_start_from(const char *base, const char *ref);

int  bfetch_state(int id);                  /* BF_PENDING / BF_DONE / BF_FAILED */
int  bfetch_status(int id);                 /* HTTP status code, or 0 */
/* The body. Owned by bfetch until bfetch_release(); NUL-terminated at [len]. */
const unsigned char *bfetch_body(int id, int *len);
const char *bfetch_url(int id);             /* final URL, after redirects */
const char *bfetch_error(int id);
/* Hand the body to the caller (who then owns it and must free()) and drop the
 * request. Returns the length, or -1. */
int  bfetch_take(int id, unsigned char **out);
void bfetch_release(int id);

/* Advance every in-flight request one step. Returns how many are still
 * pending. Never blocks. */
int  bfetch_pump(void);
/* Pump until `id` settles, or (id < 0) until nothing is pending. `tick` runs
 * once per pass so the caller can keep its window alive; it may be NULL. */
void bfetch_wait(int id, void (*tick)(void));

/* One request, run to completion. Returns 0 and a malloc'd body, or -1. */
int  bfetch_sync(const char *ref, unsigned char **out, int *outlen);

/* ---- prefetch, for callers that fetch one resource at a time ----
 *
 * layout.c's image loop calls res_fetch() per <img>, decodes, and only then
 * asks for the next one. Each decode is seconds on an emulated CPU, and a CDN's
 * keep-alive timeout is often five -- so the pooled connection was reliably
 * dead by the time the next image wanted it, and eight images from one host
 * still cost seven handshakes with a working pool. MEASURED: 8 pool hits, 5 of
 * them on sockets the server had already closed.
 *
 * The fix is not a longer timeout, it is not leaving the connection idle:
 * queue every image first so they transfer together with no decode in between,
 * and let res_fetch() take the bytes out of the cache. */
void bfetch_prefetch(const char *ref);
/* Drive every queued prefetch to completion, then stop. */
void bfetch_prefetch_wait(void);
/* Drop anything still held. Called on navigation. */
void bfetch_cache_clear(void);
/* Put bytes the caller already has into the cache, so the next res_fetch() for
 * that absolute URL costs no connection. Copies. See the comment on the
 * definition: this is what makes bringing a background tab back to the screen a
 * replay rather than a reload. */
int  bfetch_cache_put(const char *abs_url, const unsigned char *data, int len);

/* The tick bfetch_sync passes to bfetch_wait. Set once by the browser so that
 * the paths which cannot pass one themselves -- layout's image fetch, and
 * QuickJS's module loader, both of which are called from inside code that has
 * no idea a network round trip is about to happen -- still keep the window
 * answering its close button. */
void bfetch_set_tick(void (*fn)(void));

/* Observability. A pool that silently never reuses anything looks exactly like
 * one that works, so these are the numbers the whole exercise is about:
 * `dials` is how many TCP/TLS connections were actually opened, `reuses` how
 * many requests rode a connection that already existed. */
void bfetch_stats(int *dials, int *reuses, int *requests);
/* The pool's own counters. `evicted` (ran out of slots) and `closed` (the
 * connection went away) are the two reasons a page dials more than it should,
 * and they are indistinguishable from the request side. */
void bfetch_pool_stats(int *hits, int *evicted, int *closed);
void bfetch_reset_stats(void);
/* Drop every pooled connection (navigation, or shutdown). */
void bfetch_close_all(void);

/* ===================== bxfer: the protocol choice ========================
 *
 * WHAT THIS IS FOR.  fetch() in js_webapi.c drives an `h1_conn` directly, and
 * HTTP/2 is not a variant of that -- it is a different framing, a different
 * header encoding, and (the point) one connection carrying many requests at
 * once.  Wiring it in by teaching the fetch state machine about two protocols
 * would put the choice in the file that has the least business making it.
 *
 * So the choice happens BENEATH the call site.  bxfer_* are drop-in
 * replacements for h1_conn_start / h1_conn_pump / h1_conn_free plus the
 * socket open/close, and they present the HTTP/2 result THROUGH THE SAME
 * `struct h1_response`: status, header list, body sink, keep_alive.  Every
 * read of `conn.resp` in the fetch state machine -- the CORS checks, the
 * redirect handling, header visibility, the streaming sink -- is unchanged
 * and does not know which protocol answered.
 *
 * HOW THE PROTOCOL IS CHOSEN.  ALPN, during the TLS handshake, which is why
 * bxfer owns the dial: by the time the caller has a socket the decision is
 * already made.  bxfer_open offers "h2,http/1.1"; bxfer_start reads back what
 * the server picked and takes one of two paths.  A server that answers
 * `http/1.1` gets exactly the code that ran before this existed.
 *
 * WHY bxfer OWNS THE SOCKET TOO.  Multiplexing is a property of the
 * CONNECTION, not of a request, so it cannot work if each caller dials its
 * own.  bxfer_open returns the SAME fd for every concurrent request to an
 * origin that is already speaking h2 (or has a handshake in flight and might
 * be), and bxfer_close refcounts it, so N concurrent fetches to one host cost
 * one connection and N streams.  The speculative case has a real fallback: if
 * the handshake comes back `http/1.1` after all, only the first request may
 * keep that socket and the others are re-dialled their own -- which is why
 * bxfer_start takes the fd BY POINTER and may replace it.
 *
 * THE NEGATIVE CONTROL is -DBXFER_H1_ONLY, and it is applied to the
 * IMPLEMENTATION rather than to this header: browser_rt.c then never offers
 * h2 in ALPN and bxfer_start has only its HTTP/1.1 path, so the call site is
 * byte-identical between the two builds and the only difference under test is
 * the protocol. That build must FAIL the multiplexing assertion while every
 * HTTP/1.1 test still passes.
 *
 * -DWEBAPI_HOST is the other case: the host unit tests link js_webapi.c
 * WITHOUT browser_rt.c, so there these collapse to the plain h1_conn calls and
 * the host suite stays bit-identical to today. */

struct h1_conn;
struct h1_transport;

#ifdef WEBAPI_HOST

/* No HTTP/2, and no dependency on browser_rt.c. `host`/`port`/`tls` and the
 * by-pointer fd are simply unused; the socket is the caller's as before. */
#define bxfer_open(h, p, tls)   \
    sock_open((h), (p), (tls) ? SOCK_F_TLS | SOCK_F_ALPN_HTTP11 : 0)
#define bxfer_close(fd)                 sock_close(fd)
#define bxfer_start(c, t, r, l, pfd, h, p, tls)  h1_conn_start((c), (t), (r), (l))
#define bxfer_pump(c)                   h1_conn_pump(c)
#define bxfer_free(c)                   h1_conn_free(c)

#else

/* Dial, or join an existing HTTP/2 connection to the same origin. Returns a
 * socket handle the caller polls exactly as before, or < 0. */
int  bxfer_open(const char *host, int port, int tls);
/* Give a handle back. The socket is closed once the last user has let go. */
void bxfer_close(int fd);

/* Begin one exchange. `req` is a serialized HTTP/1.1 request and ownership
 * passes here either way -- on the h2 path it is re-encoded as HPACK, and the
 * request body is used in place out of that same buffer, so it is held until
 * the stream completes. `*fd` is the handle from bxfer_open and MAY BE
 * REPLACED (see the speculative case above); the caller must keep polling the
 * value it finds there afterwards. H1_OK, or a negative H1_E_*. */
int  bxfer_start(struct h1_conn *c, const struct h1_transport *t,
                 char *req, int reqlen, int *fd,
                 const char *host, int port, int tls);
/* One non-blocking step. Returns H1_C_SEND / H1_C_RECV / H1_C_DONE /
 * H1_C_ERROR, and on the h2 path fills c->resp as the response materialises. */
int  bxfer_pump(struct h1_conn *c);
void bxfer_free(struct h1_conn *c);

#endif

/* Observability, and the reason this line exists as a number rather than a
 * claim. `conns` counts connections bxfer actually dialled, `streams` the
 * requests carried over HTTP/2, and `peak` the most that were ever in flight
 * on ONE connection at the same moment -- a multiplexed connection that
 * silently serialises looks exactly like one that works, and `peak` is what
 * tells them apart. `h2conns` is how many of `conns` negotiated h2. */
void bxfer_stats(int *conns, int *h2conns, int *streams, int *peak);
void bxfer_reset_stats(void);
/* Drop every HTTP/2 connection (navigation, or shutdown). */
void bxfer_close_all(void);

#endif /* LOGIT_BFETCH_H */
