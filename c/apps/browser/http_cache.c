/* http_cache.c -- implementation. The policy contract and the WHY live in
 * http_cache.h (compiled into browser.elf by browser_rt.c's #include; read
 * that comment first). This half is: an entry table, an HTTP-date parser,
 * Cache-Control parsing, and the lifetime rules.
 *
 * THE CLOCK is WAC_NOW_MS() -- the guest's uptime, the same counter the
 * [wa] stamps use. Every lifetime is RELATIVE (receipt + lifetime), so no
 * wall clock is ever needed and a guest whose RTC is wrong caches exactly as
 * correctly as one whose is right. The one place wall time would matter --
 * comparing Expires directly -- is avoided by taking Expires minus Date,
 * which is what the lifetime is when both are server-sent.
 *
 * -DWACACHE_OFF is the negative control: every public function returns its
 * refusal/-1 and stores nothing, so the same binary shape runs with the
 * cache compiled out to nothing (tests/webaccel.mk builds it and the gate
 * must go RED there -- that build is the proof the gate measures the cache).
 *
 * -DWACACHE_NO_COOKIE_KEY is the SECOND control, added 2026-09-02 alongside
 * the cookie key: the cache stays fully on, but ck_hash() collapses to a
 * constant, so the key degenerates to url-alone -- byte-for-byte the key
 * this file had before douyin's challenge-and-reload loop exposed it as
 * wrong (CLAUDE.md's "the loop, measured on the guest"). With it, a
 * chl-cookie-setting response served to a first request must ALSO be served
 * to the re-navigation that follows it (the bug); without it, the
 * re-navigation's different Cookie header must miss and re-fetch (the fix).
 * See tests/webaccel.mk for where this is driven and watched both ways. */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "http_cache.h"

/* ---- tunables ------------------------------------------------------------ */

/* Entry count and total bytes. 64 entries at a mean 20 KB is well over a page's
 * subresource set; the 8 MiB byte cap is the real bound and is a quarter of
 * the browser arena's COMMIT ceiling (335 MiB) -- but the arena peaks in the
 * single-digit MiB on real pages (2345: 10.8 MiB peak including the DOM and
 * JS), so 8 MiB of cache is sized to coexist with that, not to dwarf it. */
#define WAC_N         64
#define WAC_MAX_BYTES (8 * 1024 * 1024)

/* The heuristic lifetime ceiling, and the default when a response carries no
 * freshness information at all. 300 s: a headerless revisit inside five
 * minutes serves from cache, past it revalidates -- short enough that a page
 * whose operator simply forgot the headers recovers quickly, long enough that
 * the second navigation of a session (the case the gate measures) is covered.
 * The 60 s no-information default is shorter still on the same argument. */
#define WAC_HEUR_MAX_S  300
#define WAC_HEUR_DEF_S  60

/* Set-Cookie clamp: see the header. */
#define WAC_COOKIE_CLAMP_S 60

struct wac_ent {
    char content_type[128], content_disposition[768];
    int    used;
    char   url[768];                 /* BF_URLMAX; duplicated deliberately so
                                      * this TU does not depend on bfetch.h */
    unsigned long long ckh;          /* hash of the request Cookie header that
                                      * fetched this entry -- see ck_hash()
                                      * below and http_cache.h's key paragraph.
                                      * ck_hash("") == ck_hash(NULL) == the
                                      * FNV-1a offset basis (a fixed constant,
                                      * not 0), so every cookieless request
                                      * to a given url hashes identically and
                                      * the key degenerates to url-alone --
                                      * exactly the pre-fix key, which is what
                                      * keeps a cookieless build (weaksym
                                      * stub, LOGIT_HAVE false) behaviourally
                                      * unchanged. */
    unsigned char *body;
    int    len;
    long long stored_ms;             /* receipt, monotonic */
    long long lifetime_ms;           /* 0 = must revalidate every use */
    long long last_used_ms;          /* LRU */
    char   etag[256];                /* validator: strong or weak, opaque here */
    char   lmod[64];                 /* Last-Modified, an IMF-fixdate string */
};

static struct wac_ent wac[WAC_N];
static long long wac_bytes;
static int wac_hits, wac_revalidations;

/* ---- the cookie half of the key ------------------------------------------
 *
 * WHY A HASH AND NOT THE BYTES: cookie_line can run to CK_HEADER_MAX (8 KiB,
 * cookies.h) and WAC_N is 64 -- storing it verbatim per entry would put
 * 512 KiB into KEYS, a sixteenth of WAC_MAX_BYTES, to hold a value only ever
 * used for equality. A hash buys the same equality test (modulo collision)
 * in 8 bytes.
 *
 * FNV-1a 64-bit, the same algorithm this tree already trusts for a
 * non-adversarial equality check (tools/ uses it for content hashes
 * elsewhere) -- not a cryptographic hash, because the threat model does not
 * need one: nobody controls both (a) the bytes of their own Cookie header
 * AND (b) another cache entry's stored URL well enough to engineer a
 * collision that serves THEM someone else's cookie-scoped response, because
 * the cookie line is built by webapi_cookie_line() from the browser's own
 * jar, not from attacker-supplied input reaching this function directly.
 * What has to be argued is accidental collision, not chosen preimage:
 * WAC_N=64 live entries means at most 64 x 63 / 2 = 2016 pairs at risk, and
 * at 2^-64 per pair the union bound is under 2^-53 per boot -- smaller than
 * the odds this machine's own crypto self-test (genroots.py) accepts for a
 * SHA-256 collision, on a table four orders of magnitude larger. A collision
 * would be a false HIT (the one outcome that matters -- see the header); a
 * hash MISS from two different cookie lines is certain by construction and
 * costs exactly one avoidable fetch, the cache's ordinary cold-entry cost. */
#ifdef WACACHE_NO_COOKIE_KEY
/* CONTROL SWITCH (see the file's WACACHE_OFF comment for the sibling
 * control): compiled with this, every cookie line hashes to the same
 * constant, so find_ent's (url, ckh) match degenerates back to url-alone --
 * byte-for-byte the pre-fix key. tests/webaccel.mk's chl- fixture drives both
 * builds and the control is watched RED here: A must be served TWICE. */
static unsigned long long ck_hash(const char *s) { (void)s; return 0; }
#else
static unsigned long long ck_hash(const char *s)
{
    /* FNV-1a 64: offset basis 0xcbf29ce484222325, prime 0x100000001b3. */
    unsigned long long h = 0xcbf29ce484222325ULL;
    if (s) for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= (unsigned long long)*p;
        h *= 0x100000001b3ULL;
    }
    return h;
}
#endif

/* ---- small helpers (this TU has no libc printf dependency on purpose) ---- */

static int ci_eq(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        int x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 1;
}

/* Is `b` (a NUL-terminated token) a case-insensitive PREFIX of `a`? The
 * caller checks the character after the prefix for a boundary -- ci_eq above
 * compares THROUGH the terminator, which is only right for whole-string
 * equality, and confusing the two is how "max-age" stops matching anything
 * (the first draft of this file did exactly that, quietly). */
static int ci_pref(const char *a, const char *b)
{
    for (int i = 0; b[i]; i++) {
        int x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

/* Case-insensitive substring search over a header VALUE (token list). */
static int val_has_token(const char *v, const char *tok)
{
    if (!v || !tok) return 0;
    for (const char *p = v; *p; p++) {
        /* token boundaries: start or comma/space before, comma/space/= after */
        if (p > v) {
            char prev = p[-1];
            if (prev != ',' && prev != ' ' && prev != '\t' && prev != ';') continue;
        }
        if (!ci_pref(p, tok)) continue;
        int tl = 0; while (tok[tl]) tl++;
        char nxt = p[tl];
        if (nxt == 0 || nxt == ',' || nxt == ' ' || nxt == '\t' || nxt == ';' ||
            nxt == '=')
            return 1;
    }
    return 0;
}

/* THE CLOCK comes from the includer. browser_rt.c has monotonic_ms() as a
 * static inline from c/apps/logit.h (defined BEFORE this file is #included,
 * so calling it here sees the right prototype); a host unit test defines
 * WAC_NOW_MS to its own fake before including this file. There is deliberately
 * no prototype for monotonic_ms in THIS file: redeclaring an inline function
 * with a different signature (long long vs unsigned long long) is a compile
 * error the first build of this file found the honest way. */
#ifndef WAC_NOW_MS
#define WAC_NOW_MS() monotonic_ms()
#endif

/* ---- HTTP dates ------------------------------------------------------------
 *
 * IMF-fixdate only: "Sun, 06 Nov 1994 08:49:37 GMT". The two obsolete formats
 * (RFC 850 asctime-ish) are refused by name rather than half-parsed -- a
 * lifetime computed from a mis-parsed date is exactly the plausible-looking
 * wrong number this tree refuses to ship. Returns unix seconds or -1. */
static long long parse_imf_fixdate(const char *s)
{
    static const char *mons[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (!s) return -1;
    /* "Www, " -- any 3-letter weekday, then comma-space. */
    int i = 0;
    while (s[i] && s[i] != ',') i++;
    if (i != 3 || s[4] == 0) return -1;
    s += i + 2;
    /* DD Mon YYYY HH:MM:SS GMT */
    int day = 0, n = 0;
    while (s[n] >= '0' && s[n] <= '9') { day = day * 10 + (s[n] - '0'); n++; }
    if (n != 2 || s[n] != ' ') return -1;
    s += n + 1;
    int mon = -1;
    for (int m = 0; m < 12; m++)
        if (ci_pref(s, mons[m])) { mon = m; break; }
    if (mon < 0 || s[3] != ' ') return -1;
    s += 4;
    long long year = 0; n = 0;
    while (s[n] >= '0' && s[n] <= '9') { year = year * 10 + (s[n] - '0'); n++; }
    if (n != 4 || s[n] != ' ') return -1;
    s += n + 1;
    long long hh = 0, mm = 0, ss = 0;
    n = 0; while (s[n] >= '0' && s[n] <= '9') { hh = hh * 10 + (s[n] - '0'); n++; }
    if (n != 2 || s[n] != ':') return -1; s += n + 1;
    n = 0; while (s[n] >= '0' && s[n] <= '9') { mm = mm * 10 + (s[n] - '0'); n++; }
    if (n != 2 || s[n] != ':') return -1; s += n + 1;
    n = 0; while (s[n] >= '0' && s[n] <= '9') { ss = ss * 10 + (s[n] - '0'); n++; }
    if (n != 2) return -1;
    s += n;
    while (*s == ' ') s++;
    if (!(ci_eq(s, "GMT", 3) || ci_eq(s, "UTC", 3))) return -1;
    if (day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60 || year < 1970)
        return -1;

    /* days-from-civil (Howard Hinnant's algorithm, public domain). NOTE THE
     * MONTH BASE: the original formula takes m ONE-BASED and shifts the year
     * for Jan/Feb; `mon` here is the 0-based table index, so the branch is
     * mon >= 2 (0-based March onward) and the offset is -2, not -3. The
     * first version of this line fed the 0-based index straight into the
     * 1-based formula and every date came out one month (31 days) early --
     * caught by the host policy gate, which is why the gate exists. */
    long long y = year;
    y -= mon <= 1;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (mon + (mon >= 2 ? -2 : 10)) + 2) / 5 + day - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + (long long)doe - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

/* ---- Cache-Control -------------------------------------------------------- */

struct cc_parse {
    int no_store;
    int no_cache;
    long long max_age_ms;            /* -1 = absent */
};

static void parse_cc(const char *v, struct cc_parse *out)
{
    out->no_store = 0; out->no_cache = 0; out->max_age_ms = -1;
    if (!v) return;
    out->no_store  = val_has_token(v, "no-store");
    out->no_cache  = val_has_token(v, "no-cache");
    /* max-age=N: find the token, then "=digits". OWS around the '=' is
     * tolerated (plenty of real servers emit "max-age = 300"), which is also
     * what the other directive parsers in this tree accept. */
    for (const char *p = v; *p; p++) {
        if (p > v) {
            char prev = p[-1];
            if (prev != ',' && prev != ' ' && prev != '\t' && prev != ';') continue;
        }
        if (!ci_pref(p, "max-age")) continue;
        const char *q = p + 7;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        long long n = 0; int any = 0;
        while (*q >= '0' && *q <= '9') { n = n * 10 + (*q - '0'); q++; any = 1;
                                         if (n > 1000000000LL) n = 1000000000LL; }
        if (any) out->max_age_ms = n * 1000;
        break;
    }
}

/* The lifetime a stored response gets, per the header contract in http_cache.h.
 * Every path is clamped to WAC_HEUR_MAX_S: an operator may ASK for a day, and
 * gets it -- up to the ceiling this cache promises a headerless-or-not response
 * will never exceed. (A real browser honours long max-ages; a cache with no
 * eviction-from-disk story and no revalidation budget for a ten-minute-old
 * entry on a machine where a revalidation costs a TLS handshake is trading a
 * promise it cannot keep for a number it cannot defend.) */
static long long compute_lifetime(const char *cache_control, const char *expires,
                                  const char *date, const char *last_modified,
                                  int had_setcookie)
{
    struct cc_parse cc;
    parse_cc(cache_control, &cc);
    if (cc.no_store) return -1;                   /* caller refuses the store */
    if (cc.no_cache) return 0;                    /* store; always revalidate */
    long long cap = WAC_HEUR_MAX_S * 1000LL;

    long long life = -1;
    if (cc.max_age_ms >= 0)
        life = cc.max_age_ms;
    else {
        long long exp = parse_imf_fixdate(expires);
        long long dat = parse_imf_fixdate(date);
        if (exp >= 0 && dat >= 0 && exp >= dat)
            life = (exp - dat) * 1000;
        else {
            long long lm = parse_imf_fixdate(last_modified);
            long long d2 = parse_imf_fixdate(date);
            if (lm >= 0 && d2 >= 0 && d2 > lm) {
                /* RFC 9111 4.2.2's heuristic: 10% of the time since
                 * Last-Modified, capped. */
                life = (d2 - lm) * 100;
            } else {
                life = WAC_HEUR_DEF_S * 1000LL;   /* no freshness info at all */
            }
        }
    }
    if (life > cap) life = cap;
    if (had_setcookie && life > WAC_COOKIE_CLAMP_S * 1000LL)
        life = WAC_COOKIE_CLAMP_S * 1000LL;
    return life;
}

/* ---- the table ------------------------------------------------------------ */

/* THE KEY: (url, cookie hash). Both must match. `ckh` is the caller's
 * ck_hash(cookie_line), computed once by each public entry point below --
 * never recomputed here, so this function has no way to see a cookie_line
 * that disagrees with its own hash. */
static struct wac_ent *find_ent_h(const char *url, unsigned long long ckh)
{
    for (int i = 0; i < WAC_N; i++)
        if (wac[i].used && wac[i].ckh == ckh && strcmp(wac[i].url, url) == 0)
            return &wac[i];
    return 0;
}

/* url-only match, EVERY cookie variant -- wacache_invalidate's contract
 * (see http_cache.h) is "poison this url", not "poison this url for this
 * request", so it has no ckh to filter on and must be able to find (and the
 * caller loop below, drop) more than one entry. */
static struct wac_ent *find_ent_any(const char *url)
{
    for (int i = 0; i < WAC_N; i++)
        if (wac[i].used && strcmp(wac[i].url, url) == 0) return &wac[i];
    return 0;
}

static void drop_ent(struct wac_ent *e)
{
    if (!e->used) return;
    free(e->body);
    wac_bytes -= e->len;
    memset(e, 0, sizeof *e);
}

/* LRU victim among entries, NULL if the table is empty. Never the entry the
 * caller is about to replace (handled by the caller replacing in place). */
static struct wac_ent *lru_victim(void)
{
    struct wac_ent *best = 0;
    for (int i = 0; i < WAC_N; i++) {
        if (!wac[i].used) continue;
        if (!best || wac[i].last_used_ms < best->last_used_ms) best = &wac[i];
    }
    return best;
}

/* LRU pick excluding one entry (the in-flight replacement). */
static struct wac_ent *lru_victim_after(struct wac_ent *skip)
{
    struct wac_ent *best = 0;
    for (int i = 0; i < WAC_N; i++) {
        struct wac_ent *cand = &wac[i];
        if (!cand->used || cand == skip) continue;
        if (!best || cand->last_used_ms < best->last_used_ms) best = cand;
    }
    return best;
}

static struct wac_ent *alloc_ent(void)
{
    for (int i = 0; i < WAC_N; i++)
        if (!wac[i].used) return &wac[i];
    return 0;
}

#ifdef WACACHE_OFF
/* NEGATIVE CONTROL: the cache is compiled to nothing. See the file comment. */
void wacache_reset(void) {}
int  wacache_lookup(const char *u, const char *ckl, unsigned char **b, int *l)
{ (void)u; (void)ckl; (void)b; (void)l; return -1; }
int  wacache_validators(const char *u, const char *ckl, char *e, int ec, char *m, int mc)
{ (void)u; (void)ckl; (void)e; (void)ec; (void)m; (void)mc; return -1; }
int  wacache_body(const char *u, const char *ckl, unsigned char **b, int *l)
{ (void)u; (void)ckl; (void)b; (void)l; return -1; }
int  wacache_store(const char *u, const char *ckl, const unsigned char *b, int l,
                   const char *cc, const char *ex, const char *d, const char *lm,
                   const char *et, const char *v, int ck)
{ (void)u; (void)ckl; (void)b; (void)l; (void)cc; (void)ex; (void)d; (void)lm; (void)et; (void)v; (void)ck; return -1; }
void wacache_refresh(const char *u, const char *ckl, const char *cc, const char *ex, const char *d)
{ (void)u; (void)ckl; (void)cc; (void)ex; (void)d; }
void wacache_invalidate(const char *u) { (void)u; }
void wacache_invalidate_matching(int (*matches)(const char *, void *), void *ctx)
{ (void)matches; (void)ctx; }
void wacache_stats(int *en, int *by, int *h, int *rv)
{ if (en) *en = 0; if (by) *by = 0; if (h) *h = 0; if (rv) *rv = 0; }
#else /* the real cache */

void wacache_reset(void)
{
    for (int i = 0; i < WAC_N; i++) drop_ent(&wac[i]);
    wac_bytes = 0;
}

static struct wac_ent *find_fresh(const char *url, unsigned long long ckh)
{
    struct wac_ent *e = find_ent_h(url, ckh);
    if (!e) return 0;
    long long now = WAC_NOW_MS();
    if (now - e->stored_ms < e->lifetime_ms) return e;
    return 0;
}

int wacache_lookup(const char *url, const char *cookie_line,
                   unsigned char **body, int *len)
{
    struct wac_ent *e = find_fresh(url, ck_hash(cookie_line));
    if (!e) return -1;
    unsigned char *copy = (unsigned char *)malloc((size_t)e->len + 1);
    if (!copy) return -1;
    memcpy(copy, e->body, (size_t)e->len);
    copy[e->len] = 0;
    *body = copy; *len = e->len;
    e->last_used_ms = WAC_NOW_MS();
    wac_hits++;
    return 0;
}

int wacache_validators(const char *url, const char *cookie_line,
                       char *etag, int etagcap, char *lmod, int lmodcap)
{
    struct wac_ent *e = find_ent_h(url, ck_hash(cookie_line));
    if (!e) return -1;
    long long now = WAC_NOW_MS();
    if (now - e->stored_ms < e->lifetime_ms) return -1;   /* fresh: no request at all */
    if (etag && etagcap > 0) {
        int i = 0;
        for (; e->etag[i] && i < etagcap - 1; i++) etag[i] = e->etag[i];
        etag[i] = 0;
    }
    if (lmod && lmodcap > 0) {
        int i = 0;
        for (; e->lmod[i] && i < lmodcap - 1; i++) lmod[i] = e->lmod[i];
        lmod[i] = 0;
    }
    e->last_used_ms = now;
    wac_revalidations++;
    return 0;
}

int wacache_body(const char *url, const char *cookie_line,
                 unsigned char **body, int *len)
{
    struct wac_ent *e = find_ent_h(url, ck_hash(cookie_line));
    if (!e || !e->body) return -1;
    unsigned char *copy = (unsigned char *)malloc((size_t)e->len + 1);
    if (!copy) return -1;
    memcpy(copy, e->body, (size_t)e->len);
    copy[e->len] = 0;
    *body = copy; *len = e->len;
    e->last_used_ms = WAC_NOW_MS();
    return 0;
}

int wacache_store(const char *url, const char *cookie_line,
                  const unsigned char *body, int len,
                  const char *cache_control, const char *expires,
                  const char *date, const char *last_modified,
                  const char *etag, const char *vary, int had_setcookie)
{
    if (!url || !url[0] || !body || len <= 0) return -1;
    if ((int)strlen(url) >= (int)sizeof wac[0].url) return -1;
    /* Vary: Accept-Encoding (or an empty value) is always safe to ignore --
     * see the header. Cookie is now ALSO storable: the key below already
     * carries the exact request Cookie header, which is a strictly finer
     * partition than Vary: Cookie asks for (it also distinguishes requests
     * a server would have called "the same" cookie-wise if it ever varied on
     * a normalised subset -- this cache does not normalise, so it never
     * over-shares, only occasionally under-shares into a cache miss). Any
     * OTHER Vary still refuses the store: it names a request dimension this
     * cache genuinely cannot tell apart (Accept-Language, User-Agent, ...),
     * and serving to the wrong variant is worse than not serving. */
    if (vary && vary[0]) {
        int ok = 0;
        for (const char *p = vary; *p; ) {
            while (*p == ' ' || *p == ',') p++;
            if (!*p) break;
            const char *tok = p;
            while (*p && *p != ',') p++;
            int tl = (int)(p - tok);
            while (tl > 0 && (tok[tl-1] == ' ')) tl--;
            if (tl == 15 && ci_eq(tok, "accept-encoding", 16)) { ok = 1; continue; }
            if (tl == 6 && ci_eq(tok, "cookie", 7)) { ok = 1; continue; }
            if (tl == 1 && tok[0] == '*') return -1;    /* Vary: * -- never */
            return -1;                                  /* any other field */
        }
        if (!ok && vary[0] != 0) {
            /* a value made only of separators: treat as no Vary */
        }
    }
    long long life = compute_lifetime(cache_control, expires, date,
                                      last_modified, had_setcookie);
    if (life < 0) return -1;                       /* no-store */

    /* Size the fit: evict LRU entries (never the one being replaced) until
     * the new body fits under the byte cap. If it cannot fit at all -- a body
     * bigger than the whole cap -- refuse; a caller holding 9 MiB wants the
     * network path, not a cache that thrashes itself for it. */
    if (len > WAC_MAX_BYTES) return -1;
    unsigned long long ckh = ck_hash(cookie_line);
    struct wac_ent *e = find_ent_h(url, ckh);
    if (e) {
        wac_bytes -= e->len;
        free(e->body);
        e->body = 0;
        /* len goes to zero with the body: drop_ent() subtracts e->len, and
         * the OOM path below used to subtract the OLD length a second time,
         * sinking wac_bytes and stalling future eviction (2026-09-16
         * audit). */
        e->len = 0;
        e->content_type[0]=e->content_disposition[0]=0;
    }
    /* The cap holds for the replace path too: in-place growth used to skip
     * eviction entirely, so WAC_N replacements of 8 MiB each could legalise
     * WAC_N times the byte budget. The victim pick excludes e -- the entry
     * this very call is about to refill (e->len is already 0, so evicting it
     * buys nothing and losing it would drop the slot mid-store).
     * (2026-09-16 audit.) */
    while (wac_bytes + len > WAC_MAX_BYTES) {
        struct wac_ent *v = lru_victim();
        if (!v) break;
        if (v == e) {
            /* e is the only candidate left; it carries no bytes. Any other
             * entry is newer than e, so honouring strict LRU here would
             * mean evicting newer-than-e entries: keep evicting those. */
            v = lru_victim_after(e);
            if (!v) break;
        }
        drop_ent(v);
    }
    if (!e) {
        e = alloc_ent();
        if (!e) {
            struct wac_ent *v = lru_victim();
            if (!v) return -1;
            drop_ent(v);
            e = alloc_ent();
            if (!e) return -1;
        }
        memset(e, 0, sizeof *e);
        int i = 0;
        while (url[i]) { e->url[i] = url[i]; i++; }
        e->url[i] = 0;
        e->ckh = ckh;
        e->used = 1;
    }
    unsigned char *copy = (unsigned char *)malloc((size_t)len + 1);
    if (!copy) { drop_ent(e); return -1; }
    memcpy(copy, body, (size_t)len);
    copy[len] = 0;
    e->body = copy; e->len = len;
    wac_bytes += len;
    e->stored_ms = WAC_NOW_MS();
    e->last_used_ms = e->stored_ms;
    e->lifetime_ms = life;
    if (etag) {
        int i = 0;
        for (; etag[i] && i < (int)sizeof e->etag - 1; i++) e->etag[i] = etag[i];
        e->etag[i] = 0;
    } else e->etag[0] = 0;
    if (last_modified) {
        int i = 0;
        for (; last_modified[i] && i < (int)sizeof e->lmod - 1; i++)
            e->lmod[i] = last_modified[i];
        e->lmod[i] = 0;
    } else e->lmod[0] = 0;
    return 0;
}

void wacache_refresh(const char *url, const char *cookie_line,
                     const char *cache_control, const char *expires,
                     const char *date)
{
    struct wac_ent *e = find_ent_h(url, ck_hash(cookie_line));
    if (!e) return;
    long long life = compute_lifetime(cache_control, expires, date, 0, 0);
    if (life < 0) { drop_ent(e); return; }         /* a 304 carrying no-store */
    e->stored_ms = WAC_NOW_MS();
    e->last_used_ms = e->stored_ms;
    e->lifetime_ms = life;
}

void wacache_invalidate_matching(int (*matches)(const char *, void *), void *ctx)
{
    if (!matches) return;
    for (int i = 0; i < WAC_N; i++)
        if (wac[i].used && matches(wac[i].url, ctx)) drop_ent(&wac[i]);
}

void wacache_invalidate(const char *url)
{
    /* ALL cookie variants of `url` -- find_ent_any has no ckh to filter on
     * and there may be more than one live entry sharing this url (a
     * cookieless fetch and a cookie-bearing one can both be stored for the
     * same address), so this drops in a loop rather than once. */
    for (;;) {
        struct wac_ent *e = find_ent_any(url);
        if (!e) break;
        drop_ent(e);
    }
}

void wacache_stats(int *entries, int *bytes, int *hits, int *revalidations)
{
    int n = 0;
    for (int i = 0; i < WAC_N; i++) if (wac[i].used) n++;
    if (entries) *entries = n;
    if (bytes) *bytes = (int)wac_bytes;
    if (hits) *hits = wac_hits;
    if (revalidations) *revalidations = wac_revalidations;
}

#endif /* !WACACHE_OFF */

/* Preserve metadata with the exact cache entry and Cookie key that owns its
 * bytes. Dropping these headers would make a cached attachment render as HTML;
 * bypassing navigation caching instead would regress ordinary page loads. */
static void cache_meta_copy(char *out,int cap,const char *value)
{if(cap<=0)return;int n=0;while(value&&value[n]&&n<cap-1){out[n]=value[n];n++;}out[n]=0;}
void wacache_response_set(const char *url,const char *ck,const char *type,const char *disposition)
{
#ifndef WACACHE_OFF
    struct wac_ent *e=find_ent_h(url,ck_hash(ck));if(!e)return;
    cache_meta_copy(e->content_type,sizeof e->content_type,type);
    cache_meta_copy(e->content_disposition,sizeof e->content_disposition,disposition);
#else
    (void)url;(void)ck;(void)type;(void)disposition;
#endif
}
void wacache_response_get(const char *url,const char *ck,char *type,int tn,char *disposition,int dn)
{
    if(tn>0)type[0]=0;if(dn>0)disposition[0]=0;
#ifndef WACACHE_OFF
    struct wac_ent *e=find_ent_h(url,ck_hash(ck));if(!e)return;
    if(tn>0)cache_meta_copy(type,tn,e->content_type);
    if(dn>0)cache_meta_copy(disposition,dn,e->content_disposition);
#else
    (void)url;(void)ck;
#endif
}
