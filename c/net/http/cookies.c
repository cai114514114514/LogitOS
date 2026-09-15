/* cookies.c -- RFC 6265 cookie jar with the 6265bis security tightenings.
 *
 * Read the domain rules before changing anything here.  A cookie jar is one of
 * the few browser components where a plausible-looking off-by-one is directly
 * exploitable: `cookie_domain_match("evil.com", "example.com")` returning 1
 * hands over a session.  Every rule below is written as the RFC states it and
 * is covered by its near-miss in tests/unit/cookie_test.c, because the easy
 * cases (exact match, obvious mismatch) pass under almost any implementation.
 *
 * Correction (2026-09-10): the old "conservative" mini-PSL could both
 * over-reject real domains and under-reject unlisted suffixes. A pinned full
 * ICANN + PRIVATE list now supplies both Domain and site classification.
 * SameSite is enforced on creation as well as retrieval, using explicit
 * request context; Secure integrity is checked before replacement/deletion.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "cookies.h"

/* Saturating arithmetic also matters for a corrupted persisted timestamp;
 * no expiry path may wrap from the future into a deletion. */
static int64_t expiry_limit(int64_t now)
{
    return now > INT64_MAX - CK_MAX_AGE_SECONDS ? INT64_MAX : now + CK_MAX_AGE_SECONDS;
}

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_ws(int c) { return c == ' ' || c == '\t'; }

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) { if (lc((unsigned char)*a) != lc((unsigned char)*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

/* Local case-insensitive strncmp: mini-libc has strncasecmp but declaring our
 * own keeps this file free of <strings.h> include-order differences between
 * the host and the freestanding build. */
static int strncmp_ci(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        int x = lc((unsigned char)a[i]), y = lc((unsigned char)b[i]);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}

static char *dupn(const char *s, int n)
{
    char *p = (char *)malloc((size_t)n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, (size_t)n);
    p[n] = 0;
    return p;
}

/* ---- canonical host --------------------------------------------------- */

int cookie_canon_host(const char *host, char *out, int outmax)
{
    if (!host || !out || outmax <= 0) return -1;
    int n = (int)strlen(host);
    if (n && host[n - 1] == '.') n--;             /* one trailing root dot */
    if (n <= 0 || n >= outmax) return -1;
    /* URL canonicalization owns IDNA. Refusing raw Unicode here avoids
     * comparing U-label input against the A-label PSL as if it were unknown. */
    int ip = strchr(host, ':') != NULL;
    for (int i = 0; i < n; i++) {
        int c = lc((unsigned char)host[i]);
        if (ip) {
            if (!is_digit(c) && !(c >= 'a' && c <= 'f') &&
                c != ':' && c != '.' && c != '[' && c != ']') return -1;
        } else {
            if (!is_digit(c) && !(c >= 'a' && c <= 'z') &&
                c != '-' && c != '_' && c != '.') return -1;
            if (c == '.' && (!i || i == n - 1 || host[i - 1] == '.')) return -1;
        }
        out[i] = (char)c;
    }
    out[n] = 0;
    return 0;
}

/* An IP literal never gets suffix treatment: 10.0.0.1 must not domain-match
 * "0.0.1".  IPv6 is caught by the colon (and by brackets from a URL). */
static int is_ip_literal(const char *h)
{
    if (strchr(h, ':')) return 1;
    if (*h == '[') return 1;
    int digits = 0, dots = 0;
    for (const char *p = h; *p; p++) {
        if (is_digit((unsigned char)*p)) { digits++; continue; }
        if (*p == '.') { dots++; continue; }
        return 0;                                  /* a letter: it is a name */
    }
    return digits > 0 && dots == 3;
}

int cookie_domain_match(const char *host, const char *domain)
{
    if (!host || !domain || !*host || !*domain) return 0;
    int hl = (int)strlen(host), dl = (int)strlen(domain);
    if (hl == dl) {
        for (int i = 0; i < hl; i++)
            if (lc((unsigned char)host[i]) != lc((unsigned char)domain[i])) return 0;
        return 1;                                  /* identical strings */
    }
    if (dl > hl) return 0;
    /* RFC 6265 5.1.3: domain is a suffix of host, the character immediately
     * before the suffix in host is '.', and host is not an IP address.  The
     * dot check is what separates "a.example.com" (match) from the attack
     * "notexample.com" (no match) -- drop it and every suffix collision in
     * the DNS becomes a cookie leak. */
    for (int i = 0; i < dl; i++)
        if (lc((unsigned char)host[hl - dl + i]) != lc((unsigned char)domain[i])) return 0;
    if (host[hl - dl - 1] != '.') return 0;
    if (is_ip_literal(host)) return 0;
    return 1;
}

static int path_match_n(const char *request_path, size_t rl, const char *cookie_path)
{
    if (!request_path || !cookie_path || !*cookie_path) return 0;
    if (*request_path != '/') return 0;
    size_t cl = strlen(cookie_path);
    if (cl > rl) return 0;
    if (memcmp(request_path, cookie_path, (size_t)cl) != 0) return 0;
    if (cl == rl) return 1;
    if (cookie_path[cl - 1] == '/') return 1;      /* "/a/" prefixes "/a/b" */
    return request_path[cl] == '/';                /* "/a" prefixes "/a/b", not "/ab" */
}

int cookie_path_match(const char *request_path, const char *cookie_path)
{ return path_match_n(request_path, request_path ? strlen(request_path) : 0, cookie_path); }

int cookie_default_path(const char *request_path, char *out, int outmax)
{
    if (!out || outmax < 2) return -1;
    if (!request_path || request_path[0] != '/') { out[0] = '/'; out[1] = 0; return 0; }
    int n = 0;
    while (request_path[n] && request_path[n] != '?' && request_path[n] != '#') n++;
    int last = -1;
    for (int i = 0; i < n; i++) if (request_path[i] == '/') last = i;
    if (last <= 0) { out[0] = '/'; out[1] = 0; return 0; }
    if (last >= outmax) return -1;
    memcpy(out, request_path, (size_t)last);
    out[last] = 0;
    return 0;
}

/* ---- one PSL authority for Domain and site-for-cookies ----------------- */
#include "cookie_psl.inc"

/* Offset table avoids one relocation/pointer per rule in both kernel and
 * browser images. Binary search includes the rule kind; wildcard and exception
 * rules are independent entries, not suffix guesses based on label spelling. */
static int psl_has(char kind, const char *domain)
{
    int lo = 0, hi = COOKIE_PSL_COUNT;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        const char *rule = cookie_psl_data + cookie_psl_offsets[mid];
        int cmp = (unsigned char)kind - (unsigned char)rule[0];
        if (!cmp) cmp = strcmp(domain, rule + 1);
        if (!cmp) return 1;
        if (cmp < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

/* The default '*' rule covers unknown TLDs too. An exception removes its
 * leftmost label from the prevailing suffix, even when a wildcard matched. */
static const char *public_suffix(const char *host)
{
    const char *label[128];
    int count = 0;
    const char *p = host;
    do {
        if (count == (int)(sizeof label / sizeof label[0])) return host;
        label[count++] = p;
        p = strchr(p, '.');
        if (p) p++;
    } while (p && *p);
    int best = count - 1;
    for (int i = 0; i < count; i++) {
        if (psl_has('!', label[i])) return i + 1 < count ? label[i + 1] : label[i];
        if (psl_has('=', label[i]) && i < best) best = i;
#ifndef COOKIE_PSL_NO_WILDCARD
        if (i > 0 && psl_has('*', label[i]) && i - 1 < best) best = i - 1;
#endif
    }
    return label[best];
}

int cookie_domain_is_public_suffix(const char *domain)
{
    char host[CK_DOMAIN_MAX];
    if (cookie_canon_host(domain, host, sizeof host) != 0) return 1;
    if (is_ip_literal(host)) return 0;
    return public_suffix(host) == host;
}

static int registrable_domain(const char *host, char *out, int outmax)
{
    char h[CK_DOMAIN_MAX];
    if (cookie_canon_host(host, h, sizeof h) != 0) return -1;
    const char *best = h;
    if (!is_ip_literal(h)) {
        const char *suffix = public_suffix(h);
        if (suffix > h) {
            best = suffix - 1;
            while (best > h && best[-1] != '.') best--;
        }
    }
    if ((int)strlen(best) >= outmax) return -1;
    memcpy(out, best, strlen(best) + 1);
    return 0;
}

int cookie_same_site(const char *host_a, const char *host_b)
{
    if (!host_a || !host_b) return 0;
    char a[CK_DOMAIN_MAX], b[CK_DOMAIN_MAX];
    if (registrable_domain(host_a, a, (int)sizeof a) != 0) return 0;
    if (registrable_domain(host_b, b, (int)sizeof b) != 0) return 0;
    return ci_eq(a, b);
}

/* Missing context is opaque, not equivalent to opening a URL from browser UI.
 * Unsafe top-level requests differ on retrieval but retain the top-level
 * creation exception. That fourth state prevents POST from inheriting Lax. */
int cookie_request_kind(const struct cookie_ctx *ctx,
                        const struct cookie_request *request)
{
    if (!ctx || !ctx->host || !request) return CK_REQ_CROSS_SITE;
    if (request->top_level_navigation && request->browser_initiated &&
        !request->site_host) return CK_REQ_SAME_SITE;
#ifndef COOKIE_SCHEMELESS_CONTEXT
    int same_scheme = !!ctx->secure == !!request->site_secure;
#else
    int same_scheme = 1;
#endif
    if (same_scheme && cookie_same_site(ctx->host, request->site_host))
        return CK_REQ_SAME_SITE;
    if (request->top_level_navigation)
        return request->safe_method ? CK_REQ_CROSS_SITE_NAV : CK_REQ_CROSS_SITE_NAV_UNSAFE;
    return CK_REQ_CROSS_SITE;
}

/* ---- RFC 6265 5.1.1 cookie-date --------------------------------------- */

static int is_delim(int c)
{
    return c == 0x09 || (c >= 0x20 && c <= 0x2F) || (c >= 0x3B && c <= 0x40) ||
           (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
}

static const char *const month_names[12] = {
    "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"
};

/* Read 1..2 digits from the front of a token. Returns the count consumed. */
static int lead_digits(const char *s, int len, int maxd, int *val)
{
    int n = 0, v = 0;
    while (n < len && n < maxd && is_digit((unsigned char)s[n])) { v = v * 10 + (s[n] - '0'); n++; }
    *val = v;
    return n;
}

/* hh:mm:ss with 1..2 digits per field, trailing junk ignored (the RFC's
 * "( non-digit *OCTET )" tail). */
static int parse_time_token(const char *s, int len, int *h, int *m, int *sec)
{
    int i = 0, n;
    n = lead_digits(s + i, len - i, 2, h);      if (!n) return 0; i += n;
    if (i >= len || s[i] != ':') return 0; i++;
    n = lead_digits(s + i, len - i, 2, m);      if (!n) return 0; i += n;
    if (i >= len || s[i] != ':') return 0; i++;
    n = lead_digits(s + i, len - i, 2, sec);    if (!n) return 0; i += n;
    if (i < len && is_digit((unsigned char)s[i])) return 0;
    return 1;
}

/* Days since 1970-01-01 for a proleptic-Gregorian y/m/d (Hinnant's algorithm).
 * Exact for every year we can parse, with no leap-year table to get wrong. */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                                   /* 0..399 */
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  /* 0..365 */
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           /* 0..146096 */
    return era * 146097 + doe - 719468;
}

int64_t cookie_parse_date(const char *s)
{
    if (!s) return 0;
    int found_time = 0, found_dom = 0, found_mon = 0, found_year = 0;
    int hh = 0, mm = 0, ss = 0, dom = 0, mon = 0, year = 0;
    int len = (int)strlen(s), i = 0;

    while (i < len) {
        while (i < len && is_delim((unsigned char)s[i])) i++;
        int b = i;
        while (i < len && !is_delim((unsigned char)s[i])) i++;
        int tl = i - b;
        if (tl <= 0) continue;
        const char *t = s + b;

        if (!found_time && parse_time_token(t, tl, &hh, &mm, &ss)) { found_time = 1; continue; }
        if (!found_dom) {
            int v, n = lead_digits(t, tl, 2, &v);
            if (n && (n == tl || !is_digit((unsigned char)t[n]))) { dom = v; found_dom = 1; continue; }
        }
        if (!found_mon && tl >= 3) {
            int hit = -1;
            for (int k = 0; k < 12; k++)
                if (lc((unsigned char)t[0]) == month_names[k][0] &&
                    lc((unsigned char)t[1]) == month_names[k][1] &&
                    lc((unsigned char)t[2]) == month_names[k][2]) { hit = k; break; }
            if (hit >= 0) { mon = hit + 1; found_mon = 1; continue; }
        }
        if (!found_year) {
            int v, n = lead_digits(t, tl, 4, &v);
            if (n >= 2 && (n == tl || !is_digit((unsigned char)t[n]))) { year = v; found_year = 1; continue; }
        }
    }
    if (!found_time || !found_dom || !found_mon || !found_year) return 0;

    if (year >= 70 && year <= 99) year += 1900;
    else if (year <= 69) year += 2000;
    if (dom < 1 || dom > 31) return 0;
    if (year < 1601) return 0;
    if (hh > 23 || mm > 59 || ss > 59) return 0;

    int64_t days = days_from_civil(year, mon, dom);
    int64_t v = days * 86400 + hh * 3600 + mm * 60 + ss;
    /* 0 is our "unparseable" sentinel; the epoch second itself is not a date
     * any Expires header means, so folding it to 1 loses nothing. */
    return v ? v : 1;
}

/* ---- jar --------------------------------------------------------------- */

void cookie_jar_init(struct cookie_jar *j)
{
    if (!j) return;
    j->v = NULL; j->n = 0; j->cap = 0;
    j->max_total = CK_JAR_MAX_TOTAL; j->max_per_domain = CK_JAR_MAX_PER_DOMAIN;
}

static void cookie_wipe(struct cookie *c)
{
    free(c->name); free(c->value); free(c->domain); free(c->path);
    memset(c, 0, sizeof *c);
}

void cookie_jar_free(struct cookie_jar *j)
{
    if (!j) return;
    for (int i = 0; i < j->n; i++) cookie_wipe(&j->v[i]);
    free(j->v);
    j->v = NULL; j->n = 0; j->cap = 0;
}

void cookie_jar_limits(struct cookie_jar *j, int max_total, int max_per_domain)
{
    if (!j) return;
    if (max_total > 0) j->max_total = max_total;
    if (max_per_domain > 0) j->max_per_domain = max_per_domain;
}

int cookie_jar_count(const struct cookie_jar *j) { return j ? j->n : 0; }

static void jar_erase(struct cookie_jar *j, int idx)
{
    cookie_wipe(&j->v[idx]);
    for (int i = idx; i < j->n - 1; i++) j->v[i] = j->v[i + 1];
    j->n--;
}

static int cookie_expired(const struct cookie *c, int64_t now)
{
    return c->persistent && c->expires <= now;
}

int cookie_jar_gc(struct cookie_jar *j, int64_t now)
{
    if (!j) return 0;
    int removed = 0;
    for (int i = j->n - 1; i >= 0; i--)
        if (cookie_expired(&j->v[i], now)) { jar_erase(j, i); removed++; }
    return removed;
}

/* Evict the least-recently-used cookie among those matching `domain`
 * (domain == NULL means the whole jar). Returns 1 if one was dropped.
 *
 * HttpOnly LAST, and this is a fix, not a preference. Least-recently-used
 * alone made the HttpOnly flag select its own bearer as the victim of the
 * very script it exists to hide from, by a chain of three lines that are each
 * correct on their own:
 *
 *   :731  a script read never MATCHES an HttpOnly cookie (that is the flag),
 *   :795  only cookies actually written to the header get `accessed = now`,
 *   here  the victim is the smallest `accessed`.
 *
 * So every `document.cookie` READ renewed the lifetime of exactly the cookies
 * the page can see and of nothing else, and the session cookie sank to the
 * bottom of the LRU by construction. Measured before this change: 49
 * `document.cookie` writes of fresh names evicted the HttpOnly session, and
 * evicted it BEFORE the page's own ordinary cookie -- the protected one died
 * first. max_per_domain is 50, which real sites reach on their own, so this
 * was also reachable without an attacker.
 *
 * The former two-pass comment claimed 6265bis leaves all eviction order to
 * the implementation. Correction (2026-09-10): secure-only protection is an
 * outer priority; HttpOnly remains our tie-breaking preference within each
 * security class. All four passes permit progress when only protected cookies
 * remain. Admission inserts the new cookie before enforcing limits, so a new
 * ordinary cookie can evict itself instead of displacing a protected one. */
static int evict_lru(struct cookie_jar *j, const char *domain)
{
#ifdef COOKIE_NO_EVICT_PREFERENCE             /* negctl: pure LRU, the old rule */
    const int npass = 1;
#else
    const int npass = 4;
#endif
    for (int pass = 0; pass < npass; pass++) {
        int best = -1;
        for (int i = 0; i < j->n; i++) {
            if (domain && !ci_eq(j->v[i].domain, domain)) continue;
            if (npass == 4) {
                if (pass < 2 && j->v[i].secure) continue;
                if ((pass == 0 || pass == 2) && j->v[i].http_only) continue;
            }
            if (best < 0 || j->v[i].accessed < j->v[best].accessed) best = i;
        }
        if (best >= 0) { jar_erase(j, best); return 1; }
    }
    return 0;
}

static int jar_find(const struct cookie_jar *j, const char *name,
                    const char *domain, const char *path, int host_only)
{
    for (int i = 0; i < j->n; i++)
        if (!strcmp(j->v[i].name, name) && ci_eq(j->v[i].domain, domain) &&
            !strcmp(j->v[i].path, path) && j->v[i].host_only == host_only) return i;
    return -1;
}

static int jar_push(struct cookie_jar *j, const struct cookie *src)
{
    if (j->n == j->cap) {
        int nc = j->cap ? j->cap * 2 : 16;
        struct cookie *nv = (struct cookie *)realloc(j->v, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        j->v = nv; j->cap = nc;
    }
    j->v[j->n++] = *src;
    return 0;
}

/* ---- Set-Cookie parsing ------------------------------------------------ */

/* A cookie name or value may not carry a control character or a ';'.  CR and
 * LF in particular: this string is later spliced into a Cookie: request header,
 * and a value containing "\r\nX: y" would split one request into two.  The
 * request builder rejects that too, but a jar that stores it has already lost
 * -- the bad value would just fail every later request instead. */
static int nv_char_ok(int c, int is_name)
{
    if (c <= 0x20 || c == 0x7f) return 0;
    if (c == ';') return 0;
    if (is_name && (c == '=' || c == ',')) return 0;
    return 1;
}

struct attrs {
    int have_expires; int64_t expires;
    int have_maxage;  int64_t maxage;
    const char *domain; int domain_len;
    const char *path;   int path_len;
    int secure, http_only, samesite, have_path, have_domain;
};

static void parse_attributes(const char *s, int len, struct attrs *a)
{
    int i = 0;
    while (i < len) {
        while (i < len && s[i] == ';') i++;
        while (i < len && is_ws((unsigned char)s[i])) i++;
        int b = i;
        while (i < len && s[i] != ';') i++;
        int e = i;
        while (e > b && is_ws((unsigned char)s[e - 1])) e--;
        if (e <= b) continue;

        int eq = b;
        while (eq < e && s[eq] != '=') eq++;
        int nb = b, ne = eq;
        while (ne > nb && is_ws((unsigned char)s[ne - 1])) ne--;
        int vb = (eq < e) ? eq + 1 : e, ve = e;
        while (vb < ve && is_ws((unsigned char)s[vb])) vb++;

        /* 6265bis ignores oversized attribute values instead of parsing a
         * truncated prefix. The last usable Path/SameSite still wins. */
        if (ve - vb > 1024) continue;
        int nl = ne - nb;
        const char *nm = s + nb;
        #define ATTR_IS(lit) (nl == (int)sizeof(lit) - 1 && \
                              !strncmp_ci(nm, lit, nl))
        if (ATTR_IS("expires")) {
            char buf[1025];
            int cl = ve - vb; if (cl >= (int)sizeof buf) continue;
            memcpy(buf, s + vb, (size_t)cl); buf[cl] = 0;
            int64_t t = cookie_parse_date(buf);
            if (t) { a->have_expires = 1; a->expires = t; }
        } else if (ATTR_IS("max-age")) {
            /* RFC 6265 5.2.2: leading '-' allowed, then DIGIT only; anything
             * else means ignore the attribute entirely (do NOT clamp). */
            int p = vb, neg = 0;
            if (p < ve && s[p] == '-') { neg = 1; p++; }
            if (p < ve) {
                int64_t v = 0, ok = 1;
                for (int k = p; k < ve; k++) {
                    if (!is_digit((unsigned char)s[k])) { ok = 0; break; }
                    if (v < CK_MAX_AGE_SECONDS) {
                        v = v * 10 + (s[k] - '0');
                        if (v > CK_MAX_AGE_SECONDS) v = CK_MAX_AGE_SECONDS;
                    }
                }
                if (ok) { a->have_maxage = 1; a->maxage = neg ? -v : v; }
            }
        } else if (ATTR_IS("domain")) {
            a->have_domain = 1;
            int p = vb;
            if (p < ve && s[p] == '.') p++;         /* 5.2.3: strip one leading dot */
            if (p < ve) { a->domain = s + p; a->domain_len = ve - p; }
        } else if (ATTR_IS("path")) {
            a->have_path = 1; a->path = NULL; a->path_len = 0;
            if (ve > vb && s[vb] == '/') { a->path = s + vb; a->path_len = ve - vb; }
        } else if (ATTR_IS("secure")) {
            a->secure = 1;
        } else if (ATTR_IS("httponly")) {
            a->http_only = 1;
        } else if (ATTR_IS("samesite")) {
            a->samesite = CK_SS_UNSET;
            int vl = ve - vb;
            if (vl == 6 && !strncmp_ci(s + vb, "strict", 6)) a->samesite = CK_SS_STRICT;
            else if (vl == 3 && !strncmp_ci(s + vb, "lax", 3)) a->samesite = CK_SS_LAX;
            else if (vl == 4 && !strncmp_ci(s + vb, "none", 4)) a->samesite = CK_SS_NONE;
        }
        #undef ATTR_IS
    }
}

int cookie_set(struct cookie_jar *j, const struct cookie_ctx *ctx,
               const char *set_cookie_value, int64_t now)
{ return cookie_set_ex(j, ctx, CK_REQ_SAME_SITE, set_cookie_value, now); }

int cookie_set_ex(struct cookie_jar *j, const struct cookie_ctx *ctx,
                  int request_kind, const char *set_cookie_value, int64_t now)
{
    if (!j || !ctx || !ctx->host || !ctx->path || !set_cookie_value) return -1;

    char host[CK_DOMAIN_MAX];
    if (cookie_canon_host(ctx->host, host, (int)sizeof host) != 0) return -1;

    const char *s = set_cookie_value;
    int len = (int)strlen(s);
    if (len > CK_PAIR_MAX + 8192) return -1; /* bounded metadata work */
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) return -1;
    }

    /* 5.2.1: split off the name/value pair at the first ';'. */
    int semi = 0;
    while (semi < len && s[semi] != ';') semi++;
    int eq = 0;
    while (eq < semi && s[eq] != '=') eq++;
    if (eq == semi) return -1;                    /* no '=': ignore, per 5.2 */

    int nb = 0, ne = eq;
    while (nb < ne && is_ws((unsigned char)s[nb])) nb++;
    while (ne > nb && is_ws((unsigned char)s[ne - 1])) ne--;
    int vb = eq + 1, ve = semi;
    while (vb < ve && is_ws((unsigned char)s[vb])) vb++;
    while (ve > vb && is_ws((unsigned char)s[ve - 1])) ve--;

    int nlen = ne - nb, vlen = ve - vb;
    if (nlen <= 0 || nlen > CK_NAME_MAX) return -1;
    if (vlen < 0 || vlen > CK_VALUE_MAX || nlen + vlen > CK_PAIR_MAX) return -1;
    for (int i = 0; i < nlen; i++) if (!nv_char_ok((unsigned char)s[nb + i], 1)) return -1;
    for (int i = 0; i < vlen; i++) if (!nv_char_ok((unsigned char)s[vb + i], 0)) return -1;

    struct attrs a;
    memset(&a, 0, sizeof a);
    a.samesite = CK_SS_UNSET;
    if (semi < len) parse_attributes(s + semi, len - semi, &a);

    /* 5.2.2: Max-Age overrides Expires; <= 0 means delete now. */
    int persistent = 0;
    int64_t expires = 0;
    if (a.have_maxage) {
        persistent = 1;
        expires = a.maxage <= 0 ? now
                : (now > INT64_MAX - a.maxage ? INT64_MAX : now + a.maxage);
    } else if (a.have_expires) {
        persistent = 1;
        expires = a.expires > expiry_limit(now) ? expiry_limit(now) : a.expires;
    }

    /* 5.3 steps 5-6: the Domain attribute. */
    char domain[CK_DOMAIN_MAX];
    int host_only;
    if (a.domain && a.domain_len > 0) {
        if (a.domain_len >= (int)sizeof domain) return -1;
        for (int i = 0; i < a.domain_len; i++) domain[i] = (char)lc((unsigned char)a.domain[i]);
        domain[a.domain_len] = 0;
        /* A trailing dot in Domain is not the URL root-dot normalization:
         * accepting it silently widens a malformed attribute to a real host. */
        if (domain[a.domain_len - 1] == '.') return -1;
        char canonical[CK_DOMAIN_MAX];
        if (cookie_canon_host(domain, canonical, sizeof canonical) != 0) return -1;
        if (cookie_domain_is_public_suffix(domain)) {
            /* The RFC allows one exception: domain == host exactly, which is
             * how a site literally at a public suffix (rare) keeps working.
             * It is still downgraded to host-only so it cannot widen. */
            if (!ci_eq(domain, host)) return -1;
            host_only = 1;
            memcpy(domain, host, strlen(host) + 1);
        } else if (!cookie_domain_match(host, domain)) {
            return -1;                            /* not ours to set */
        } else {
            host_only = 0;
        }
    } else {
        host_only = 1;
        memcpy(domain, host, strlen(host) + 1);
    }

    char path[CK_PATH_MAX];
    if (a.path && a.path_len > 0) {
        if (a.path_len >= (int)sizeof path) return -1;
        memcpy(path, a.path, (size_t)a.path_len);
        path[a.path_len] = 0;
    } else if (cookie_default_path(ctx->path, path, (int)sizeof path) != 0) {
        return -1;
    }

    /* The old "all security rules" claim only checked the incoming flag.
     * Secure integrity also protects an EXISTING cookie when the new one has
     * no Secure flag, including expired writes that would otherwise delete it. */

    /* A Secure cookie may only be SET over a secure channel; otherwise an
     * active network attacker on the http origin can overwrite the https
     * session cookie (cookie forcing). */
    if (a.secure && !ctx->secure) return -1;
    /* SameSite=None is only meaningful with Secure. */
    if (a.samesite == CK_SS_NONE && !a.secure) return -1;
    /* Script may neither set nor overwrite an HttpOnly cookie. */
    if (a.http_only && !ctx->http_api) return -1;

#ifndef COOKIE_NO_CREATION_CONTEXT
    if (a.samesite != CK_SS_NONE && request_kind != CK_REQ_SAME_SITE &&
        !(ctx->http_api && (request_kind == CK_REQ_CROSS_SITE_NAV ||
                           request_kind == CK_REQ_CROSS_SITE_NAV_UNSAFE))) return -1;
#endif

    /* Cookie name prefixes -- cheap, and the only integrity guarantee a
     * cookie name can carry. */
    if (nlen >= 9 && !strncmp_ci(s + nb, "__Secure-", 9)) {
        if (!a.secure || !ctx->secure) return -1;
    }
    if (nlen >= 7 && !strncmp_ci(s + nb, "__Host-", 7)) {
        if (!a.secure || !ctx->secure || !host_only || a.have_domain) return -1;
        if (!a.have_path || !a.path || a.path_len != 1 || strcmp(path, "/") != 0) return -1;
    }

    cookie_jar_gc(j, now);
#ifndef COOKIE_NO_SECURE_OVERLAY
    if (!ctx->secure && !a.secure) {
        for (int i = 0; i < j->n; i++) {
            const struct cookie *old = &j->v[i];
            if (!old->secure || (int)strlen(old->name) != nlen ||
                memcmp(old->name, s + nb, (size_t)nlen)) continue;
            if ((cookie_domain_match(domain, old->domain) ||
                 cookie_domain_match(old->domain, domain)) &&
                cookie_path_match(path, old->path)) return -1;
        }
    }
#endif

    char *cname = dupn(s + nb, nlen);
    char *cvalue = dupn(s + vb, vlen);
    if (!cname || !cvalue) { free(cname); free(cvalue); return -1; }

    int idx = jar_find(j, cname, domain, path, host_only);
    if (idx >= 0 && j->v[idx].http_only && !ctx->http_api) {
        free(cname); free(cvalue);
        return -1;                                /* script cannot clobber HttpOnly */
    }

    if (persistent && expires <= now) {           /* an explicit deletion */
        if (idx >= 0) jar_erase(j, idx);
        free(cname); free(cvalue);
        return 0;
    }

    if (idx >= 0) {
        /* 5.3 step 11: keep the original creation time so the Cookie: header
         * ordering does not shuffle when a session cookie is refreshed. */
        free(j->v[idx].value);
        j->v[idx].value = cvalue;
        j->v[idx].expires = expires;
        j->v[idx].persistent = persistent;
        j->v[idx].secure = a.secure;
        j->v[idx].http_only = a.http_only;
        j->v[idx].samesite = a.samesite;
        j->v[idx].host_only = host_only;
        j->v[idx].accessed = now;
        free(cname);
        return 0;
    }

    cookie_jar_gc(j, now);
    struct cookie c;
    memset(&c, 0, sizeof c);
    c.name = cname; c.value = cvalue;
    c.domain = dupn(domain, (int)strlen(domain));
    c.path = dupn(path, (int)strlen(path));
    if (!c.domain || !c.path) { cookie_wipe(&c); return -1; }
    c.expires = expires; c.persistent = persistent;
    c.created = now; c.accessed = now;
    c.host_only = host_only; c.secure = a.secure;
    c.http_only = a.http_only; c.samesite = a.samesite;
    if (jar_push(j, &c) != 0) { cookie_wipe(&c); return -1; }
    int per = 0;
    for (int i = 0; i < j->n; i++) if (ci_eq(j->v[i].domain, domain)) per++;
    while (per > j->max_per_domain && evict_lru(j, domain)) per--;
    while (j->n > j->max_total && evict_lru(j, NULL)) { }
    return 0;
}

/* Local durable data remains untrusted input. In particular, loading a jar
 * written before a PSL update may not restore a now-public Domain cookie.
 * Do not route this through Set-Cookie: that loses original creation ordering
 * and turns serialization details into an accidental network trust context. */
int cookie_restore_entry(struct cookie_jar *j, const struct cookie *entry,
                          int64_t now)
{
    if (!j || !entry || !entry->name || !entry->value || !entry->domain ||
        !entry->path || entry->persistent != 1 || entry->expires <= now ||
        entry->expires > expiry_limit(now) || entry->created < 0 ||
        entry->accessed < entry->created || entry->created > now ||
        entry->accessed > now ||
        (entry->host_only != 0 && entry->host_only != 1) ||
        (entry->secure != 0 && entry->secure != 1) ||
        (entry->http_only != 0 && entry->http_only != 1) ||
        entry->samesite < CK_SS_UNSET || entry->samesite > CK_SS_STRICT)
        return -1;
    size_t nl = strlen(entry->name), vl = strlen(entry->value);
    size_t pl = strlen(entry->path);
    if (!nl || nl > CK_NAME_MAX || vl > CK_VALUE_MAX || nl + vl > CK_PAIR_MAX ||
        !pl || pl >= CK_PATH_MAX || entry->path[0] != '/') return -1;
    for (size_t i = 0; i < nl; i++)
        if (!nv_char_ok((unsigned char)entry->name[i], 1)) return -1;
    for (size_t i = 0; i < vl; i++)
        if (!nv_char_ok((unsigned char)entry->value[i], 0)) return -1;
    for (size_t i = 0; i < pl; i++) {
        unsigned char c = (unsigned char)entry->path[i];
        if (c < 0x20 || c == 0x7f || c == ';') return -1;
    }
    char host[CK_DOMAIN_MAX];
    if (cookie_canon_host(entry->domain, host, sizeof host) != 0 ||
        strcmp(host, entry->domain)) return -1;
    if (!entry->host_only && cookie_domain_is_public_suffix(host)) return -1;
    if (entry->samesite == CK_SS_NONE && !entry->secure) return -1;
    if (nl >= 9 && !strncmp_ci(entry->name, "__Secure-", 9) && !entry->secure) return -1;
    if (nl >= 7 && !strncmp_ci(entry->name, "__Host-", 7) &&
        (!entry->secure || !entry->host_only || strcmp(entry->path, "/"))) return -1;
    if (j->n >= j->max_total || jar_find(j, entry->name, host, entry->path,
                                      entry->host_only) >= 0) return -1;
    int per = 0;
    for (int i = 0; i < j->n; i++) if (!strcmp(j->v[i].domain, host)) per++;
    if (per >= j->max_per_domain) return -1;
    struct cookie copy = *entry;
    copy.name = dupn(entry->name, (int)nl);
    copy.value = dupn(entry->value, (int)vl);
    copy.domain = dupn(host, (int)strlen(host));
    copy.path = dupn(entry->path, (int)pl);
    if (!copy.name || !copy.value || !copy.domain || !copy.path || jar_push(j, &copy)) {
        cookie_wipe(&copy);
        return -1;
    }
    return 0;
}

/* ---- Cookie: header ---------------------------------------------------- */

static int cookie_applies(const struct cookie *c, const char *host,
                          const char *path, size_t path_len, const struct cookie_ctx *ctx,
                          int cross_site, int64_t now)
{
    if (cookie_expired(c, now)) return 0;
    /* SameSite, three-valued -- see the enum in cookies.h for why two was not
     * enough.  An absent attribute IS Lax per 6265bis, so the Lax arm covers
     * CK_SS_UNSET too and only CK_SS_STRICT is left out of a navigation. */
    /* The two negative controls are the two ways to collapse three values back
     * into two, and they are opposites: one over-sends on a navigation, the
     * other under-sends. Each must fail a DIFFERENT assertion of
     * t_samesite_request_kinds -- if either fails both, the table has stopped
     * distinguishing the row it was added for. */
#if defined(COOKIE_NAV_IS_SAME_SITE)          /* negctl: Lax stops meaning Lax */
    if (cross_site == CK_REQ_CROSS_SITE && c->samesite != CK_SS_NONE) return 0;
#elif defined(COOKIE_NAV_IS_CROSS_SITE)       /* negctl: the pre-fix binary read */
    if (cross_site != CK_REQ_SAME_SITE && c->samesite != CK_SS_NONE) return 0;
#else
    if (cross_site != CK_REQ_SAME_SITE && c->samesite != CK_SS_NONE) {
        if (cross_site != CK_REQ_CROSS_SITE_NAV) return 0;
        if (c->samesite == CK_SS_STRICT) return 0;
    }
#endif
    if (c->host_only) { if (!ci_eq(c->domain, host)) return 0; }
    else if (cookie_domain_is_public_suffix(c->domain) ||
             !cookie_domain_match(host, c->domain)) return 0;
    if (!path_match_n(path, path_len, c->path)) return 0;
    /* A Secure cookie over plaintext is exactly the leak the flag exists to
     * prevent, so this is unconditional and not a preference. */
    if (c->secure && !ctx->secure) return 0;
    if (c->http_only && !ctx->http_api) return 0;
    return 1;
}

int cookie_header(struct cookie_jar *j, const struct cookie_ctx *ctx,
                  int64_t now, char *out, int outmax)
{ return cookie_header_ex(j, ctx, CK_REQ_SAME_SITE, now, out, outmax); }

int cookie_header_ex(struct cookie_jar *j, const struct cookie_ctx *ctx,
                     int cross_site, int64_t now, char *out, int outmax)
{ return cookie_header_with_diagnostics(j, ctx, cross_site, now, out, outmax, NULL); }

int cookie_header_with_diagnostics(struct cookie_jar *j, const struct cookie_ctx *ctx,
                     int cross_site, int64_t now, char *out, int outmax,
                     struct cookie_header_diagnostics *diagnostics)
{
    if (out && outmax > 0) out[0] = 0;
    if (diagnostics) memset(diagnostics, 0, sizeof *diagnostics);
    if (!j || !ctx || !ctx->host || !ctx->path || !out || outmax <= 0) return -1;
    char host[CK_DOMAIN_MAX];
    if (cookie_canon_host(ctx->host, host, (int)sizeof host) != 0) return -1;

    /* Do not truncate a long request path at the cookie-attribute cap: a
     * request and a Path attribute are distinct bounded inputs, and slicing
     * the former can manufacture an exact match at byte 1024. */
    const char *path = ctx->path[0] == '/' ? ctx->path : "/";
    size_t path_len = 0;
    while (path[path_len] && path[path_len] != '?' && path[path_len] != '#') path_len++;

    /* Gather indices, then order per RFC 6265 5.4: longer paths first, and
     * among equal path lengths the earlier-created cookie first.  Servers do
     * rely on this (the classic case is a narrow /app cookie shadowing a
     * broad / one), so it is not cosmetic. */
    int *idx = (int *)malloc((size_t)(j->n ? j->n : 1) * sizeof(int));
    if (!idx) return -1;
    int m = 0;
    for (int i = 0; i < j->n; i++)
        if (cookie_applies(&j->v[i], host, path, path_len, ctx, cross_site, now)) idx[m++] = i;

    size_t required = 1;
    for (int i = 0; i < m; i++) {
        const struct cookie *c = &j->v[idx[i]];
        size_t need = strlen(c->name) + strlen(c->value) + 1 + (i ? 2 : 0);
        if (need > SIZE_MAX - required) { free(idx); return CK_E_ARG; }
        required += need;
    }
    if (diagnostics) {
        diagnostics->required_bytes = required;
        diagnostics->eligible_count = m;
    }
#ifndef COOKIE_PARTIAL_HEADER
    if (required > (size_t)outmax) {
        free(idx);
#ifdef COOKIE_NOFIT_IS_EMPTY
        return 0;
#else
        return CK_E_NOFIT;
#endif
    }
#endif

    for (int i = 1; i < m; i++) {                 /* insertion sort: m is tiny */
        int k = idx[i], p = i - 1;
        while (p >= 0) {
            const struct cookie *a = &j->v[idx[p]], *b = &j->v[k];
            int la = (int)strlen(a->path), lb = (int)strlen(b->path);
            int after = (la < lb) || (la == lb && a->created > b->created);
            if (!after) break;
            idx[p + 1] = idx[p]; p--;
        }
        idx[p + 1] = k;
    }

    int o = 0;
    for (int i = 0; i < m; i++) {
        struct cookie *c = &j->v[idx[i]];
        int nl = (int)strlen(c->name), vl = (int)strlen(c->value);
        int need = (o ? 2 : 0) + nl + 1 + vl;
        if (o + need >= outmax) break;            /* whole cookies only */
        if (o) { out[o++] = ';'; out[o++] = ' '; }
        memcpy(out + o, c->name, (size_t)nl); o += nl;
        out[o++] = '=';
        memcpy(out + o, c->value, (size_t)vl); o += vl;
        c->accessed = now;
    }
    out[o] = 0;
    free(idx);
    /* m cookies applied and none fit. That is not the same fact as "the user
     * has no cookies here", and it used to be reported as the same integer --
     * see the return contract in cookies.h. */
#ifndef COOKIE_NOFIT_IS_EMPTY                     /* negctl: fold them back */
    if (o == 0 && m > 0) return CK_E_NOFIT;
#endif
    return o;
}
