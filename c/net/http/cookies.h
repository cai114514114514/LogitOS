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

/* How big a Cookie: value any caller in this tree gives us.
 *
 * IT IS ONE NUMBER BECAUSE IT USED TO BE THREE, and they disagreed by 8x:
 * document.cookie read through 4096, fetch/XHR through 2048, and the
 * navigation plus every subresource through 1024.  Three caps on one jar is
 * three different answers to "what are this user's cookies", and the smallest
 * one was on the path that matters most.  Measured on the tree's own code: a
 * 1100-byte token -- JWT, OIDC id_token, cf_clearance are all this size --
 * was readable from document.cookie while the PAGE LOAD sent no Cookie header
 * at all.  Worse, a realistic bilibili-shaped set totals 1039 bytes, so at
 * 1024 the wire dropped exactly one cookie; RFC 6265 5.4's required order is
 * longest-path-then-earliest-created, so the one dropped was the NEWEST --
 * the anti-bot token a WAF had just set on the reload it forced.
 *
 * 8192 is chosen as what SERVERS accept, not as what we can hold: it is
 * nginx's and Apache's default large-header buffer.  A cap derived from the
 * jar instead (50 cookies x 4 KiB) would be 200 KiB of stack for a case no
 * server would answer. */
#define CK_HEADER_MAX 8192

/* Build the Cookie: request-header VALUE for this request into out.
 * Returns the length written, or one of:
 *
 *   0            no cookie applies -- the user has none for this request
 *   CK_E_NOFIT   cookies DO apply and not one of them fit in outmax
 *   CK_E_ARG     bad input
 *
 * THE FIRST TWO USED TO BE THE SAME VALUE, and nothing could tell them apart:
 * out[0] is 0 either way.  So document.cookie answered "" for a jar holding a
 * cookie whose value was legal (4090 bytes, under this file's own
 * CK_VALUE_MAX) and merely too long for the caller's buffer, while
 * navigator.cookieEnabled -- the property a page consults to disambiguate
 * exactly this -- said cookies work.  Three components, three different
 * stories, and the page cannot tell.
 *
 * PARTIAL truncation still returns the length written, because every caller
 * uses the buffer and a negative would make them send nothing where they can
 * send something.  With one CK_HEADER_MAX it is out of reach for real sites;
 * when it does happen the cookie dropped is the newest, and that is 5.4's
 * ordering, not a choice made here.
 *
 * Updates last-access times, hence the non-const jar. */
enum { CK_E_ARG = -1, CK_E_NOFIT = -2 };
int  cookie_header(struct cookie_jar *j, const struct cookie_ctx *ctx,
                   int64_t now, char *out, int outmax);

/* Same, for a request whose SITE differs from the document's.
 *
 * SameSite is a property of the REQUEST, not of the jar, and it is not in
 * struct cookie_ctx on purpose: two existing callers build that struct field
 * by field, so a new member would be read uninitialized rather than default to
 * the safe value.  A parameter cannot be forgotten.
 *
 * cross_site == CK_REQ_CROSS_SITE suppresses every cookie whose SameSite is
 * Strict, Lax, or absent -- absent is Lax per 6265bis.  Only an explicit
 * SameSite=None (which cookies.c already requires to be Secure) is sent.
 * That is the rule that makes a cross-site fetch unable to ride on the
 * user's session.
 *
 * CK_REQ_CROSS_SITE_NAV is the SAME request seen from a different place: a
 * cross-site TOP-LEVEL NAVIGATION, which is the one thing Lax relaxes for.
 * Strict is still suppressed; Lax and absent are sent.  This value exists
 * because the sentence that used to end the paragraph above -- "a fetch is
 * never a top-level navigation" -- was an argument about the only CALLER at
 * the time, written into a rule that outlived it.  The transport path
 * (browser_rt.c) carries navigations, and it inherited the binary reading:
 * every request it made was declared same-site, so a cross-site subresource
 * rode the session.  A two-state enum whose two states are "the caller I
 * know about" and "everything else" is how that happens.
 *
 * The three are ordered by how much they permit, and the safe end is 0 --
 * NOT because anything switches on the order, but because a caller that
 * passes an uninitialised int gets the RESTRICTIVE answer only if the
 * restrictive value is nonzero.  It is: SAME_SITE is 0 and permits
 * everything, so an uninitialised int is the DANGEROUS value here.  That is
 * exactly why this is a parameter and not a struct field, and why every
 * caller below computes it rather than defaulting it. */
enum { CK_REQ_SAME_SITE = 0, CK_REQ_CROSS_SITE = 1, CK_REQ_CROSS_SITE_NAV = 2 };
int  cookie_header_ex(struct cookie_jar *j, const struct cookie_ctx *ctx,
                      int cross_site, int64_t now, char *out, int outmax);

/* 1 if two hosts belong to the same site -- equal registrable domains, by the
 * same approximate suffix rule as cookie_domain_is_public_suffix.  IP literals
 * are the same site only when identical. */
int  cookie_same_site(const char *host_a, const char *host_b);

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
