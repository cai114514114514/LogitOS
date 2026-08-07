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
void bfetch_reset_stats(void);
/* Drop every pooled connection (navigation, or shutdown). */
void bfetch_close_all(void);

#endif /* LOGIT_BFETCH_H */
