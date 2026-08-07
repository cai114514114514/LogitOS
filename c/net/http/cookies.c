/* cookies.c -- RFC 6265 cookie jar with the 6265bis security tightenings.
 *
 * Read the domain rules before changing anything here.  A cookie jar is one of
 * the few browser components where a plausible-looking off-by-one is directly
 * exploitable: `cookie_domain_match("evil.com", "example.com")` returning 1
 * hands over a session.  Every rule below is written as the RFC states it and
 * is covered by its near-miss in tests/unit/cookie_test.c, because the easy
 * cases (exact match, obvious mismatch) pass under almost any implementation.
 *
 * THE MISSING PUBLIC SUFFIX LIST.
 * RFC 6265 5.3 step 5 says: reject a Domain= attribute that names a public
 * suffix.  Without that check, evil.co.uk can set `Domain=co.uk` and its
 * cookie is then domain-matched by bbc.co.uk, gov.uk's neighbours, everyone --
 * the supercookie.  Deciding "is this a public suffix" genuinely requires
 * Mozilla's PSL: it is a ~10k-entry data file with no algorithmic shortcut
 * (com.au is a suffix, com.de is not).  We do not ship it.
 *
 * So the conservative rule, chosen because over-rejecting costs a session
 * cookie while over-accepting costs the session itself:
 *
 *   1. A Domain= attribute with no dot at all is rejected.  That alone kills
 *      `Domain=com`, `Domain=uk`, `Domain=localhost`, and every single-label
 *      TLD supercookie.
 *   2. A Domain= attribute naming an entry of a small built-in table of common
 *      two-label public suffixes (co.uk, com.au, ...) is rejected.
 *   3. Everything else with >= 2 labels is accepted if it domain-matches the
 *      request host, as the RFC requires.
 *
 * The residual risk is exactly step 3 for a two-label public suffix outside
 * the table (say `Domain=com.xx` for some ccTLD we did not list).  It is a
 * real gap and it is stated here rather than hidden; closing it means shipping
 * the PSL, which is a data-vendoring decision, not a code one.  Note the
 * common case is unaffected: a cookie with NO Domain attribute is host-only
 * and never widens, and that is the majority of real cookies.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "cookies.h"

#define CK_NAME_MAX    256
#define CK_VALUE_MAX  4096
#define CK_DOMAIN_MAX  256
#define CK_PATH_MAX   1024
/* An expiry far enough out that it is unambiguously "never", used when
 * Max-Age is huge.  2^31 seconds past 2038 is fine on int64. */
#define CK_FAR_FUTURE  ((int64_t)253402300799LL)     /* 9999-12-31T23:59:59Z */

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
    for (int i = 0; i < n; i++) out[i] = (char)lc((unsigned char)host[i]);
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

int cookie_path_match(const char *request_path, const char *cookie_path)
{
    if (!request_path || !cookie_path || !*cookie_path) return 0;
    if (*request_path != '/') return 0;
    int rl = (int)strlen(request_path), cl = (int)strlen(cookie_path);
    if (cl > rl) return 0;
    if (memcmp(request_path, cookie_path, (size_t)cl) != 0) return 0;
    if (cl == rl) return 1;
    if (cookie_path[cl - 1] == '/') return 1;      /* "/a/" prefixes "/a/b" */
    return request_path[cl] == '/';                /* "/a" prefixes "/a/b", not "/ab" */
}

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

/* ---- the mini public-suffix table ------------------------------------- */

/* Deliberately small and boring: the two-label suffixes a real user is most
 * likely to browse.  See the file header for why this is not a PSL and what
 * that costs. */
static const char *const public_suffix2[] = {
    "co.uk", "org.uk", "ac.uk", "gov.uk", "me.uk", "net.uk", "sch.uk",
    "com.au", "net.au", "org.au", "edu.au", "gov.au", "id.au",
    "com.cn", "net.cn", "org.cn", "gov.cn", "edu.cn", "ac.cn",
    "com.br", "com.mx", "com.ar", "com.co", "com.pe",
    "co.jp", "or.jp", "ne.jp", "ac.jp", "go.jp",
    "co.kr", "or.kr", "co.in", "net.in", "org.in", "gov.in", "ac.in",
    "co.za", "org.za", "com.tr", "com.tw", "com.hk", "org.hk",
    "co.nz", "org.nz", "net.nz", "com.sg", "com.my", "com.ph",
    "com.vn", "com.ua", "com.pl", "com.ru", "co.il", "com.eg",
    "com.sa", "com.ng", "com.pk", "com.bd", "co.th", "or.th",
    NULL
};

int cookie_domain_is_public_suffix(const char *domain)
{
    if (!domain || !*domain) return 1;
    /* Rule 1: no dot -> a bare TLD (or "localhost"). Never a valid Domain=. */
    if (!strchr(domain, '.')) return 1;
    /* Rule 2: the built-in two-label table. */
    for (int i = 0; public_suffix2[i]; i++)
        if (ci_eq(domain, public_suffix2[i])) return 1;
    return 0;
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
    j->max_total = 300; j->max_per_domain = 50;
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
 * (domain == NULL means the whole jar). Returns 1 if one was dropped. */
static int evict_lru(struct cookie_jar *j, const char *domain)
{
    int best = -1;
    for (int i = 0; i < j->n; i++) {
        if (domain && !ci_eq(j->v[i].domain, domain)) continue;
        if (best < 0 || j->v[i].accessed < j->v[best].accessed) best = i;
    }
    if (best < 0) return 0;
    jar_erase(j, best);
    return 1;
}

static int jar_find(const struct cookie_jar *j, const char *name,
                    const char *domain, const char *path)
{
    for (int i = 0; i < j->n; i++)
        if (!strcmp(j->v[i].name, name) && ci_eq(j->v[i].domain, domain) &&
            !strcmp(j->v[i].path, path)) return i;
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
    int secure, http_only, samesite;
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

        int nl = ne - nb;
        const char *nm = s + nb;
        #define ATTR_IS(lit) (nl == (int)sizeof(lit) - 1 && \
                              !strncmp_ci(nm, lit, nl))
        if (ATTR_IS("expires")) {
            char buf[128];
            int cl = ve - vb; if (cl > (int)sizeof buf - 1) cl = (int)sizeof buf - 1;
            memcpy(buf, s + vb, (size_t)cl); buf[cl] = 0;
            int64_t t = cookie_parse_date(buf);
            if (t) { a->have_expires = 1; a->expires = t; }
        } else if (ATTR_IS("max-age")) {
            /* RFC 6265 5.2.2: leading '-' allowed, then DIGIT only; anything
             * else means ignore the attribute entirely (do NOT clamp). */
            int p = vb, neg = 0;
            if (p < ve && (s[p] == '-' || s[p] == '+')) { neg = (s[p] == '-'); p++; }
            if (p < ve) {
                int64_t v = 0, ok = 1;
                for (int k = p; k < ve; k++) {
                    if (!is_digit((unsigned char)s[k])) { ok = 0; break; }
                    if (v > (int64_t)200000000000LL) { v = 200000000000LL; continue; }
                    v = v * 10 + (s[k] - '0');
                }
                if (ok) { a->have_maxage = 1; a->maxage = neg ? -v : v; }
            }
        } else if (ATTR_IS("domain")) {
            int p = vb;
            if (p < ve && s[p] == '.') p++;         /* 5.2.3: strip one leading dot */
            if (p < ve) { a->domain = s + p; a->domain_len = ve - p; }
        } else if (ATTR_IS("path")) {
            if (ve > vb && s[vb] == '/') { a->path = s + vb; a->path_len = ve - vb; }
        } else if (ATTR_IS("secure")) {
            a->secure = 1;
        } else if (ATTR_IS("httponly")) {
            a->http_only = 1;
        } else if (ATTR_IS("samesite")) {
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
{
    if (!j || !ctx || !ctx->host || !ctx->path || !set_cookie_value) return -1;

    char host[CK_DOMAIN_MAX];
    if (cookie_canon_host(ctx->host, host, (int)sizeof host) != 0) return -1;

    const char *s = set_cookie_value;
    int len = (int)strlen(s);
    if (len > CK_VALUE_MAX + CK_NAME_MAX + 2048) return -1;   /* absurd header */

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
    if (vlen < 0 || vlen > CK_VALUE_MAX) return -1;
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
        expires = a.maxage <= 0 ? (now - 1)
                : (a.maxage > CK_FAR_FUTURE - now ? CK_FAR_FUTURE : now + a.maxage);
    } else if (a.have_expires) {
        persistent = 1;
        expires = a.expires;
    }

    /* 5.3 steps 5-6: the Domain attribute. */
    char domain[CK_DOMAIN_MAX];
    int host_only;
    if (a.domain && a.domain_len > 0) {
        if (a.domain_len >= (int)sizeof domain) return -1;
        for (int i = 0; i < a.domain_len; i++) domain[i] = (char)lc((unsigned char)a.domain[i]);
        domain[a.domain_len] = 0;
        int dl = a.domain_len;
        while (dl > 1 && domain[dl - 1] == '.') domain[--dl] = 0;
        if (!dl) return -1;
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

    /* --- 6265bis security rules, all of them "reject" rather than "adjust" --- */

    /* A Secure cookie may only be SET over a secure channel; otherwise an
     * active network attacker on the http origin can overwrite the https
     * session cookie (cookie forcing). */
    if (a.secure && !ctx->secure) return -1;
    /* SameSite=None is only meaningful with Secure. */
    if (a.samesite == CK_SS_NONE && !a.secure) return -1;
    /* Script may neither set nor overwrite an HttpOnly cookie. */
    if (a.http_only && !ctx->http_api) return -1;

    /* Cookie name prefixes -- cheap, and the only integrity guarantee a
     * cookie name can carry. */
    if (nlen > 9 && !strncmp_ci(s + nb, "__Secure-", 9)) {
        if (!a.secure || !ctx->secure) return -1;
    }
    if (nlen > 7 && !strncmp_ci(s + nb, "__Host-", 7)) {
        if (!a.secure || !ctx->secure || !host_only) return -1;
        if (strcmp(path, "/") != 0) return -1;
    }

    char *cname = dupn(s + nb, nlen);
    char *cvalue = dupn(s + vb, vlen);
    if (!cname || !cvalue) { free(cname); free(cvalue); return -1; }

    int idx = jar_find(j, cname, domain, path);
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
    int per = 0;
    for (int i = 0; i < j->n; i++) if (ci_eq(j->v[i].domain, domain)) per++;
    while (per >= j->max_per_domain && evict_lru(j, domain)) per--;
    while (j->n >= j->max_total && evict_lru(j, NULL)) { }
    if (j->n >= j->max_total) { free(cname); free(cvalue); return -1; }

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
    return 0;
}

/* ---- Cookie: header ---------------------------------------------------- */

static int cookie_applies(const struct cookie *c, const char *host,
                          const char *path, const struct cookie_ctx *ctx, int64_t now)
{
    if (cookie_expired(c, now)) return 0;
    if (c->host_only) { if (!ci_eq(c->domain, host)) return 0; }
    else if (!cookie_domain_match(host, c->domain)) return 0;
    if (!cookie_path_match(path, c->path)) return 0;
    /* A Secure cookie over plaintext is exactly the leak the flag exists to
     * prevent, so this is unconditional and not a preference. */
    if (c->secure && !ctx->secure) return 0;
    if (c->http_only && !ctx->http_api) return 0;
    return 1;
}

int cookie_header(struct cookie_jar *j, const struct cookie_ctx *ctx,
                  int64_t now, char *out, int outmax)
{
    if (!j || !ctx || !ctx->host || !ctx->path || !out || outmax <= 0) return -1;
    char host[CK_DOMAIN_MAX];
    if (cookie_canon_host(ctx->host, host, (int)sizeof host) != 0) return -1;

    char path[CK_PATH_MAX];
    {
        int n = 0;
        const char *p = ctx->path;
        if (!p || p[0] != '/') { path[0] = '/'; path[1] = 0; }
        else {
            while (p[n] && p[n] != '?' && p[n] != '#' && n < (int)sizeof path - 1) n++;
            memcpy(path, p, (size_t)n); path[n] = 0;
        }
    }

    /* Gather indices, then order per RFC 6265 5.4: longer paths first, and
     * among equal path lengths the earlier-created cookie first.  Servers do
     * rely on this (the classic case is a narrow /app cookie shadowing a
     * broad / one), so it is not cosmetic. */
    int *idx = (int *)malloc((size_t)(j->n ? j->n : 1) * sizeof(int));
    if (!idx) return -1;
    int m = 0;
    for (int i = 0; i < j->n; i++)
        if (cookie_applies(&j->v[i], host, path, ctx, now)) idx[m++] = i;

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
    return o;
}
