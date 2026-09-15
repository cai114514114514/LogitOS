#ifndef LOGIT_HTTP_CACHE_H
#define LOGIT_HTTP_CACHE_H

/* http_cache -- the browser's cross-navigation HTTP cache (webaccel, 2026-08-30).
 *
 * WHY THIS EXISTS, in the structural fact that motivated it: browser.c's
 * load_once() clears bfetch's per-navigation prefetch cache AND drops every
 * pooled connection on every navigation (browser.c:1884-1898), so a revisit
 * refetched every stylesheet, script and image and redialled every origin.
 * Those two calls are correct for what they own (the prefetch cache is keyed
 * by the OLD document's base; the pool holds the old page's connections), so
 * this cache is a THIRD thing: keyed by absolute post-redirect URL, owned by
 * nobody's navigation, surviving them all. It lives beneath bfetch, in ring 3,
 * consulted before a connection is ever dialled.
 *
 * THIS FILE IS COMPILED BY #INCLUDE FROM browser_rt.c, the way js_wasm.c
 * textually includes c/lib/wasm's three .c files: BROWSER_PIPE in the
 * Makefile is a hand-kept list this wave must not edit, and a new TU nobody
 * lists is a new TU nobody builds. If http_cache.c ever grows a second
 * consumer, the right move is a Makefile line, not a second #include.
 *
 * THE POLICY (RFC 9111, the subset an honest cache can keep here):
 *   - stored: complete 2xx GET responses only. Never a ranged one (a 206's
 *     bytes are a slice; filing them under the plain URL is the corruption
 *     bfetch.h's prefetch cache already guards against), never a partial
 *     body, never a response with `no-store`.
 *   - freshness: `max-age=N` wins; else Expires-minus-Date (both parsed as
 *     IMF-fixdate -- no wall clock exists in this app, and the DIFFERENCE of
 *     two server dates is the lifetime without needing one); else the
 *     documented heuristic (10% of Date-minus-Last-Modified, capped) with a
 *     short default when even that is absent. Everything is capped at
 *     HEUR_MAX because a headerless response is exactly the case where an
 *     operator gave no consent to long storage.
 *   - `no-cache` stores the body with zero lifetime: every use revalidates
 *     (If-None-Match/If-Modified-Since), which is what the header asks for.
 *   - revalidation: a stale entry's validators ride the next GET; a 304
 *     refreshes freshness and serves the stored bytes; a 200 replaces them.
 *   - Vary: `Accept-Encoding` (and empty) is safe to ignore -- every request
 *     this browser sends carries the same Accept-Encoding. `Cookie` is now
 *     safe to ACCEPT too (see the key, next paragraph) rather than refuse:
 *     the request dimension Vary: Cookie asks a cache to key on is exactly
 *     the dimension the key already carries, for every entry, whether the
 *     response declared it or not. Any other Vary still REFUSES the store,
 *     because serving to the wrong variant is worse than not serving.
 *   - THE KEY IS (url, request Cookie header), NOT url ALONE (fixed
 *     2026-09-02; see "Set-Cookie responses" below for the bug this closes).
 *     The cookie half is the exact bytes `webapi_cookie_line()` produced for
 *     the request that is about to go out -- every wacache_* entry point
 *     below takes a `cookie_line` parameter for that reason, and every
 *     caller must pass what it is ACTUALLY SENDING, not a recomputation from
 *     a different moment (one jar, two doors: AGENTS.md section 1). The
 *     cookie line can run to CK_HEADER_MAX (8 KiB, cookies.h) -- too large to
 *     hold per entry at WAC_N=64 without eating the whole byte budget on
 *     keys instead of bodies -- so the ENTRY stores a 64-bit hash of it, not
 *     the bytes. A collision (two different cookie jars hashing equal) is a
 *     false HIT, which is the one outcome this design cannot tolerate being
 *     silent about; see http_cache.c's hash comment for why 64 bits over a
 *     64-entry table makes that probability the kind you'd need a targeted
 *     preimage attack, not bad luck, to hit. A hash MISS (different bytes,
 *     different hash, always true) costs exactly one avoidable fetch, which
 *     is the failure mode this cache already has on every cold entry.
 *   - Set-Cookie responses: THE OLD LINE READ "stored with lifetime clamped
 *     to 60 s: this cache cannot vary on Cookie, and a login banner cached
 *     for an hour past logout is the failure that clamp exists to bound."
 *     That is still true and the clamp is KEPT (defence in depth against a
 *     101-year max-age typo), but it was never sufficient on its own: the
 *     clamp bounds HOW LONG a wrong serve lasts, not WHETHER one happens
 *     inside that window -- and a challenge-and-reload loop (douyin's
 *     ByteDance PoW page) re-navigates well inside 60 s, so the clamp alone
 *     still let the second navigation serve the FIRST navigation's
 *     Set-Cookie response back to a request that (correctly) now carries the
 *     cookie the first response set. Same URL, same cache, different
 *     request -- and the old key could not tell the two apart. What makes a
 *     Set-Cookie response safe to cache now is the key change above: the
 *     challenge page is stored under (url, cookieless-Cookie-header) and the
 *     re-navigation that follows it carries the new cookie, so it keys to a
 *     DIFFERENT entry -- a miss, which is correct, not a bug the clamp has
 *     to paper over. The clamp still exists for the 60-second window where a
 *     cookie WASN'T sent for the fetch that stored the entry (a plain page
 *     that happens to also set a session cookie) but might be relevant to
 *     other same-URL fetches during that window.
 *
 * WHAT IT DELIBERATELY IS NOT: persistent. It is process memory, gone at
 * exit; the storage wave's SYS_FTRUNCATE makes a disk-backed cache possible
 * and that is a later wave's job. Not a shared cache -- each entry is keyed
 * to the exact Cookie header that fetched it, so nothing is served across a
 * cookie boundary by construction (see the key, above), but there is still
 * one process, one jar, one cache: two logged-in users of this browser would
 * not be a scenario this machine has anyway. Redirect HOP TARGETS are never
 * cache-served: the cache is consulted when a request is armed, and a hop
 * re-queues straight to the network (validators cleared, so url-A's ETag can
 * never ask url-B for a 304) -- the hop target's real bytes arrive and are
 * stored under their own URL; serving B from memory mid-chain would save one
 * request and cost a second request-identity rule, and the first measured
 * corpus redirect chains are short enough that it buys nothing. reload()
 * cannot bypass it yet -- browser.c's ctrl+R calls load() with no
 * distinguishing signal; the knob exists (bfetch_set_bypass) and the
 * one-line browser.c diff is in the webaccel report until that file's owner
 * lands it. */

/* Wipe every entry (memory pressure, tests). NOT called on navigation. */
void wacache_reset(void);

/* A FRESH hit: `url` is stored under `cookie_line` and its lifetime has not
 * run out. `cookie_line` is the exact Cookie header this request is about to
 * send (whatever webapi_cookie_line() returned for it -- pass "" or NULL for
 * none, which is what a cookieless build always passes and is what makes the
 * key degenerate to url-alone there, unchanged from before this parameter
 * existed). Returns 0 and a MALLOC'D COPY of the body (caller frees), or -1.
 * Copies on purpose: the entry must survive the caller freeing its copy. */
int  wacache_lookup(const char *url, const char *cookie_line,
                    unsigned char **body, int *len);

/* The validators of a STORED entry (same url+cookie_line key) that is NOT
 * fresh (stale, or stored with `no-cache`): returns 0 and copies the
 * ETag/Last-Modified into the caller's buffers (truncated, never
 * overflowing), so the next GET can be made conditional. No body is copied
 * here -- the body is only needed if a 304 comes back, and copying 400 KiB of
 * script per request to arm a validator would be the cache paying for the
 * network it exists to avoid. */
int  wacache_validators(const char *url, const char *cookie_line,
                        char *etag, int etagcap, char *lmod, int lmodcap);

/* The stored body of the (url, cookie_line) entry, as a fresh malloc'd copy,
 * whatever its freshness. This is the 304 path: the server just vouched for
 * these bytes. -1 if no entry. `cookie_line` must be the SAME bytes passed to
 * the wacache_validators() call that armed this conditional GET, or the 304
 * answers the wrong entry's stale-check with the wrong entry's body. */
int  wacache_body(const char *url, const char *cookie_line,
                  unsigned char **body, int *len);

/* Store (or replace) the entry for `url` keyed with `cookie_line` -- the
 * Cookie header THIS REQUEST SENT, not anything from the response (a
 * response's Set-Cookie is a WRITE to the jar for the *next* request, never
 * the key for THIS one; see http_cache.h's Set-Cookie paragraph). Header
 * values may all be NULL. Returns 0 if stored, -1 if refused (no-store, bad
 * Vary, over cap and nothing evictable, malloc failure) -- a refusal is
 * silent to the page, which simply refetches next time. */
int  wacache_store(const char *url, const char *cookie_line,
                   const unsigned char *body, int len,
                   const char *cache_control, const char *expires,
                   const char *date, const char *last_modified,
                   const char *etag, const char *vary, int had_setcookie);

/* A 304 answered a conditional GET: keep the stored body, recompute its
 * lifetime from the 304's own headers (per RFC 9111 4.3.4 the 304's headers
 * update the stored response). `cookie_line` selects the same entry
 * wacache_body() just read. No-op if nothing is stored. */
void wacache_refresh(const char *url, const char *cookie_line,
                     const char *cache_control, const char *expires,
                     const char *date);

/* Drop EVERY entry stored under `url`, whatever cookie_line keyed it (a 200
 * replaced it, or the caller knows it is poison) -- no caller of this
 * function has a specific request's cookie_line in hand, and "poison the URL
 * for every cookie variant" is the conservative reading of an invalidate with
 * no such context. */
void wacache_invalidate(const char *url);
/* Invalidate all cookie variants of every matching URI. The transport owns
 * URL parsing (this policy module also serves parser-free host tests), so it
 * supplies equivalence here: explicit default ports and host case must not
 * let a successful mutation leave a second spelling of the target fresh. */
void wacache_invalidate_matching(int (*matches)(const char *url, void *ctx), void *ctx);


/* Observability: entries held, bytes held, fresh/stale lookups served. */
void wacache_stats(int *entries, int *bytes, int *hits, int *revalidations);

void wacache_response_set(const char *,const char *,const char *,const char *);
void wacache_response_get(const char *,const char *,char *,int,char *,int);

#endif /* LOGIT_HTTP_CACHE_H */
