#ifndef LOGIT_COOKIES_H
#define LOGIT_COOKIES_H

#include <stdint.h>

/* cookies -- an RFC 6265 (+ 6265bis security rules) cookie jar.
 *
 * `grep -rni cookie c/` over this tree finds exactly one hit today, and it is
 * DHCP's magic cookie.  The browser has never sent one, because the kernel's
 * request builder concatenates string literals and has no header list to put
 * a Cookie into.  So there is no login, no session, no consent state, nothing
 * that needs a second page view.
 *
 * The rule that matters here is the one that is easy to get subtly wrong and
 * catastrophic when it is: WHICH HOST GETS THE COOKIE.  A jar that leaks
 * example.com's session cookie to evil.com is not a missing feature, it is
 * account takeover.  So domain-match and path-match are implemented exactly
 * as RFC 6265 5.1.3/5.1.4 specify (suffix, on a label boundary, never for an
 * IP literal) and tested against their near-misses, not just their easy cases.
 *
 * WHAT WE DO NOT HAVE: a public suffix list.  See cookies.c for the exact
 * consequence and the conservative rule chosen instead -- in short, a Domain=
 * attribute must have at least two labels and must not name a public suffix
 * from a small built-in table, and when in doubt the cookie is rejected
 * (degrading to host-only) rather than accepted.
 */

enum {                          /* SameSite */
    CK_SS_UNSET = 0,            /* absent -> treated as Lax, per 6265bis */
    CK_SS_NONE,
    CK_SS_LAX,
    CK_SS_STRICT
};

struct cookie {
    char   *name;
    char   *value;
    char   *domain;             /* canonical, lowercase, no leading dot */
    char   *path;
    int64_t expires;            /* unix seconds; 0 == session cookie */
    int64_t created;            /* for the Cookie: ordering rule */
    int64_t accessed;           /* for LRU eviction */
    int     persistent;
    int     host_only;
    int     secure;
    int     http_only;
    int     samesite;
};

struct cookie_jar {
    struct cookie *v;
    int n, cap;
    int max_total;              /* default 300 */
    int max_per_domain;         /* default 50 */
};

/* The request this operation belongs to.  `http_api` distinguishes the
 * network stack (may read and write HttpOnly cookies) from script
 * (document.cookie -- must not). */
struct cookie_ctx {
    const char *host;           /* host from the URL; canonicalized internally */
    const char *path;           /* request path; query/fragment are stripped */
    int         secure;         /* 1 for https */
    int         http_api;       /* 1 network stack, 0 script */
};

void cookie_jar_init(struct cookie_jar *j);
void cookie_jar_free(struct cookie_jar *j);
void cookie_jar_limits(struct cookie_jar *j, int max_total, int max_per_domain);
int  cookie_jar_count(const struct cookie_jar *j);
/* Drop expired cookies. Returns how many were removed. */
int  cookie_jar_gc(struct cookie_jar *j, int64_t now);

/* Store one Set-Cookie field value. Returns 0 if stored (or deleted, for an
 * already-expired cookie), -1 if the cookie was rejected. Rejection is the
 * normal outcome for a hostile or malformed header and is not an error. */
int  cookie_set(struct cookie_jar *j, const struct cookie_ctx *ctx,
                const char *set_cookie_value, int64_t now);

/* Build the Cookie: request-header VALUE for this request into out.
 * Returns the length written (0 if no cookie applies), or -1 on bad input.
 * Updates last-access times, hence the non-const jar. */
int  cookie_header(struct cookie_jar *j, const struct cookie_ctx *ctx,
                   int64_t now, char *out, int outmax);

/* ---- the matching rules, exported so they can be tested directly ---- */

/* RFC 6265 5.1.3. Both arguments must already be canonical (lowercase). */
int  cookie_domain_match(const char *host, const char *domain);
/* RFC 6265 5.1.4. */
int  cookie_path_match(const char *request_path, const char *cookie_path);
/* RFC 6265 5.1.4 default-path: "/a/b/c" -> "/a/b", "/a" -> "/", "" -> "/". */
int  cookie_default_path(const char *request_path, char *out, int outmax);
/* Lowercase, strip one trailing dot. Returns 0, or -1 if it does not fit. */
int  cookie_canon_host(const char *host, char *out, int outmax);
/* RFC 6265 5.1.1 cookie-date. Returns unix seconds, or 0 if unparseable. */
int64_t cookie_parse_date(const char *s);
/* 1 if `domain` may not be used as a Domain= attribute value (too few labels,
 * or a known public suffix). See the header comment about the missing PSL. */
int  cookie_domain_is_public_suffix(const char *domain);

#endif /* LOGIT_COOKIES_H */
