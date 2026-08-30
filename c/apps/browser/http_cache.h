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
 *   - Vary: only `Accept-Encoding` (and empty) is safe to ignore -- every
 *     request this browser sends carries the same Accept-Encoding. Any other
 *     Vary REFUSES the store, because serving to the wrong variant is worse
 *     than not serving.
 *   - Set-Cookie responses are stored with lifetime clamped to 60 s: this
 *     cache cannot vary on Cookie, and a login banner cached for an hour
 *     past logout is the failure that clamp exists to bound.
 *
 * WHAT IT DELIBERATELY IS NOT: persistent. It is process memory, gone at
 * exit; the storage wave's SYS_FTRUNCATE makes a disk-backed cache possible
 * and that is a later wave's job. Not authenticated-request-aware beyond the
 * Set-Cookie clamp. Not a shared cache. reload() cannot bypass it yet --
 * browser.c's ctrl+R calls load() with no distinguishing signal; the knob
 * exists (bfetch_set_bypass) and the one-line browser.c diff is in the
 * webaccel report until that file's owner lands it. */

/* Wipe every entry (memory pressure, tests). NOT called on navigation. */
void wacache_reset(void);

/* A FRESH hit: `url` is stored and its lifetime has not run out. Returns 0
 * and a MALLOC'D COPY of the body (caller frees), or -1. Copies on purpose:
 * the entry must survive the caller freeing its copy. */
int  wacache_lookup(const char *url, unsigned char **body, int *len);

/* The validators of a STORED entry that is NOT fresh (stale, or stored with
 * `no-cache`): returns 0 and copies the ETag/Last-Modified into the caller's
 * buffers (truncated, never overflowing), so the next GET can be made
 * conditional. No body is copied here -- the body is only needed if a 304
 * comes back, and copying 400 KiB of script per request to arm a validator
 * would be the cache paying for the network it exists to avoid. */
int  wacache_validators(const char *url, char *etag, int etagcap,
                        char *lmod, int lmodcap);

/* The stored body of `url`, as a fresh malloc'd copy, whatever its freshness.
 * This is the 304 path: the server just vouched for these bytes. -1 if no
 * entry. */
int  wacache_body(const char *url, unsigned char **body, int *len);

/* Store (or replace) the entry for `url`. Header values may all be NULL.
 * Returns 0 if stored, -1 if refused (no-store, bad Vary, over cap and
 * nothing evictable, malloc failure) -- a refusal is silent to the page,
 * which simply refetches next time. */
int  wacache_store(const char *url, const unsigned char *body, int len,
                   const char *cache_control, const char *expires,
                   const char *date, const char *last_modified,
                   const char *etag, const char *vary, int had_setcookie);

/* A 304 answered a conditional GET: keep the stored body, recompute its
 * lifetime from the 304's own headers (per RFC 9111 4.3.4 the 304's headers
 * update the stored response). No-op if nothing is stored. */
void wacache_refresh(const char *url, const char *cache_control,
                     const char *expires, const char *date);

/* Drop one entry (a 200 replaced it, or the caller knows it is poison). */
void wacache_invalidate(const char *url);

/* Observability: entries held, bytes held, fresh/stale lookups served. */
void wacache_stats(int *entries, int *bytes, int *hits, int *revalidations);

#endif /* LOGIT_HTTP_CACHE_H */
