#ifndef LOGIT_COOKIES_H
#define LOGIT_COOKIES_H

#include <stdint.h>
#include <stddef.h>

/* RFC 6265 cookie jar with the current 6265bis storage/retrieval rules.
 * Correction (2026-09-10): the former "missing PSL / conservative table"
 * claim was incomplete: that heuristic both rejected registrable domains and
 * accepted unlisted public suffixes. Domain and SameSite now share the full
 * vendored ICANN + PRIVATE PSL; tools/psl/README.md records its update path.
 * Context belongs to each operation, never to ambient mutable jar state. */
enum { CK_SS_UNSET = 0, CK_SS_NONE, CK_SS_LAX, CK_SS_STRICT };

#define CK_NAME_MAX 4096
#define CK_VALUE_MAX 4096
#define CK_PAIR_MAX 4096                 /* combined name + value, no '=' */
#define CK_DOMAIN_MAX 256                /* storage bytes including NUL */
#define CK_PATH_MAX 1025                 /* 1024 attribute octets + NUL */
#define CK_JAR_MAX_TOTAL 300
#define CK_JAR_MAX_PER_DOMAIN 50
#define CK_MAX_AGE_SECONDS ((int64_t)34560000) /* 400 days; both expiry forms */
#define CK_HEADER_MAX 8192

struct cookie {
    char *name, *value, *domain, *path;
    int64_t expires, created, accessed;
    int persistent, host_only, secure, http_only, samesite;
};
struct cookie_jar {
    struct cookie *v;
    int n, cap, max_total, max_per_domain;
};
struct cookie_ctx {
    const char *host;
    const char *path;
    int secure;                          /* HTTPS; not port-dependent */
    int http_api;                        /* network=1, document.cookie=0 */
};

/* site_host is the already-derived site for cookies (NULL means opaque),
 * which the browser must carry through frames, redirects and async callbacks.
 * Only a top-level navigation started by browser UI may set browser_initiated;
 * a missing/opaque document origin alone never grants same-site access. */
struct cookie_request {
    const char *site_host;
    int site_secure;
    int top_level_navigation;
    int safe_method;
    int browser_initiated;
};
enum {
    CK_REQ_SAME_SITE = 0, CK_REQ_CROSS_SITE = 1,
    CK_REQ_CROSS_SITE_NAV = 2, CK_REQ_CROSS_SITE_NAV_UNSAFE = 3
};
int cookie_request_kind(const struct cookie_ctx *ctx,
                        const struct cookie_request *request);

void cookie_jar_init(struct cookie_jar *j);
void cookie_jar_free(struct cookie_jar *j);
void cookie_jar_limits(struct cookie_jar *j, int max_total, int max_per_domain);
int cookie_jar_count(const struct cookie_jar *j);
int cookie_jar_gc(struct cookie_jar *j, int64_t now);

/* 0 stored/deleted, -1 rejected. Explicit request context is mandatory on
 * browser paths. The older convenience entry point means SAME_SITE only. */
int cookie_set(struct cookie_jar *j, const struct cookie_ctx *ctx,
                const char *value, int64_t now);
int cookie_set_ex(struct cookie_jar *j, const struct cookie_ctx *ctx,
                   int request_kind, const char *value, int64_t now);

/* Restore an owned-store record after strict revalidation against current
 * limits and PSL. Deep copies, restores ordering, rejects malformed/expired
 * or duplicate entries, and never evicts another record. A decoder should
 * restore into a temporary jar so a bad snapshot cannot partially load. */
int cookie_restore_entry(struct cookie_jar *j, const struct cookie *entry,
                          int64_t now);

/* ALL OR NOTHING. The old contract returned a successful partial header;
 * increasing all callers to 8192 did not make that loss of state unreachable.
 * CK_E_NOFIT now empties out for ANY overflow, so transport/cache callers can
 * refuse the request consistently instead of silently dropping newer cookies.
 * required_bytes includes NUL; diagnostics never contain cookie values.
 * Last-access timestamps change only after successful complete serialization. */
enum { CK_E_ARG = -1, CK_E_NOFIT = -2 };
struct cookie_header_diagnostics {
    size_t required_bytes;
    int eligible_count;
};
int cookie_header(struct cookie_jar *j, const struct cookie_ctx *ctx,
                   int64_t now, char *out, int outmax);
int cookie_header_ex(struct cookie_jar *j, const struct cookie_ctx *ctx,
                      int request_kind, int64_t now, char *out, int outmax);
int cookie_header_with_diagnostics(struct cookie_jar *j,
                      const struct cookie_ctx *ctx, int request_kind,
                      int64_t now, char *out, int outmax,
                      struct cookie_header_diagnostics *diagnostics);

/* Host-only comparison for PSL tests. Browser policy MUST use request_kind
 * above, which also compares scheme and handles navigation method/context. */
int cookie_same_site(const char *host_a, const char *host_b);
int cookie_domain_match(const char *host, const char *domain);
int cookie_path_match(const char *request_path, const char *cookie_path);
int cookie_default_path(const char *request_path, char *out, int outmax);
/* Lowercase ASCII host and remove one trailing root dot. URL/IDNA processing
 * precedes this boundary; non-ASCII domains are refused rather than compared
 * using an incomplete IDNA mapping. A-label PSL entries are fully supported. */
int cookie_canon_host(const char *host, char *out, int outmax);
int64_t cookie_parse_date(const char *s);
int cookie_domain_is_public_suffix(const char *domain);

#endif
