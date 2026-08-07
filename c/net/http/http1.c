/* http1.c -- HTTP/1.1 client: request building, and a non-blocking response
 * parser.  See http1.h for why this replaces the kernel's c/net/http/http.c.
 *
 * The whole file is written against one rule: EVERY byte here came off the
 * network and is chosen by someone else.  So there is no "trusted" path, no
 * fixed-size copy without a bound, no allocation whose size the peer picks,
 * and no arithmetic on a length that has not been overflow-checked first.
 * The kernel version was never fuzzed; this one is (tests/unit/http1_fuzz.c).
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "http1.h"

/* rust/src/inflate.rs -- the same safe-Rust inflater the PNG decoder uses.
 * It was already linked into browser.aex; it simply had no HTTP caller
 * because the decode point was in the kernel. */
int inflate_raw(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen);
int zlib_decompress(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen);

/* ---- small char helpers ---------------------------------------------- */

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_ws(int c) { return c == ' ' || c == '\t'; }

/* RFC 9110 tchar. A header name that is not a token is not a header; letting
 * one through is how "Content-Length : 5" style smuggling starts. */
static int is_tchar(int c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c)) return 1;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
    case '+': case '-': case '.': case '^': case '_': case '`': case '|':
    case '~': return 1;
    default: return 0;
    }
}

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) { if (lc((unsigned char)*a) != lc((unsigned char)*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static int ci_eq_n(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (lc((unsigned char)a[i]) != lc((unsigned char)b[i])) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

static char *dupn(const char *s, int n)
{
    char *p = (char *)malloc((size_t)n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, (size_t)n);
    p[n] = 0;
    return p;
}

const char *h1_strerror(int err)
{
    switch (err) {
    case H1_OK:          return "ok";
    case H1_E_SYNTAX:    return "malformed response";
    case H1_E_TOOLARGE:  return "response exceeds limit";
    case H1_E_TRUNC:     return "connection closed mid-message";
    case H1_E_NOMEM:     return "out of memory";
    case H1_E_TRANSPORT: return "transport error";
    case H1_E_ARG:       return "bad argument";
    case H1_E_ENCODING:  return "unsupported or corrupt content-encoding";
    default:             return "unknown error";
    }
}

/* ---- header list ------------------------------------------------------ */

void h1_headers_init(struct h1_headers *h) { h->v = NULL; h->n = 0; h->cap = 0; }

void h1_headers_free(struct h1_headers *h)
{
    if (!h) return;
    for (int i = 0; i < h->n; i++) { free(h->v[i].name); free(h->v[i].value); }
    free(h->v);
    h->v = NULL; h->n = 0; h->cap = 0;
}

/* A value may not contain CR, LF or NUL, and a name must be a token.  This is
 * THE header-injection check: without it a cookie value or a redirect target
 * containing "\r\nX: y" splits one request into two, which is how a browser
 * ends up smuggling requests through a proxy.  It is enforced on the request
 * side (what we send) and on the response side (what we parse), because both
 * directions feed strings back into the other. */
static int hdr_valid(const char *name, int nlen, const char *value, int vlen)
{
    if (!name || nlen <= 0) return 0;
    for (int i = 0; i < nlen; i++) if (!is_tchar((unsigned char)name[i])) return 0;
    if (vlen < 0) vlen = 0;
    for (int i = 0; i < vlen; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c == '\r' || c == '\n' || c == 0) return 0;
    }
    return 1;
}

int h1_headers_add(struct h1_headers *h, const char *name, int nlen,
                   const char *value, int vlen)
{
    if (!h || !name) return H1_E_ARG;
    if (nlen < 0) nlen = (int)strlen(name);
    if (!value) { value = ""; vlen = 0; }
    if (vlen < 0) vlen = (int)strlen(value);
    if (!hdr_valid(name, nlen, value, vlen)) return H1_E_ARG;
    if (h->n >= H1_MAX_HEADERS) return H1_E_TOOLARGE;

    if (h->n == h->cap) {
        int nc = h->cap ? h->cap * 2 : 8;
        if (nc > H1_MAX_HEADERS) nc = H1_MAX_HEADERS;
        struct h1_hdr *nv = (struct h1_hdr *)realloc(h->v, (size_t)nc * sizeof *nv);
        if (!nv) return H1_E_NOMEM;
        h->v = nv; h->cap = nc;
    }
    char *n = dupn(name, nlen);
    char *v = dupn(value, vlen);
    if (!n || !v) { free(n); free(v); return H1_E_NOMEM; }
    h->v[h->n].name = n; h->v[h->n].value = v; h->n++;
    return H1_OK;
}

void h1_headers_remove(struct h1_headers *h, const char *name)
{
    if (!h || !name) return;
    int w = 0;
    for (int i = 0; i < h->n; i++) {
        if (ci_eq(h->v[i].name, name)) { free(h->v[i].name); free(h->v[i].value); continue; }
        h->v[w++] = h->v[i];
    }
    h->n = w;
}

int h1_headers_set(struct h1_headers *h, const char *name, const char *value)
{
    if (!h || !name) return H1_E_ARG;
    h1_headers_remove(h, name);
    return h1_headers_add(h, name, -1, value, -1);
}

const char *h1_headers_get(const struct h1_headers *h, const char *name)
{
    if (!h || !name) return NULL;
    for (int i = 0; i < h->n; i++)
        if (ci_eq(h->v[i].name, name)) return h->v[i].value;
    return NULL;
}

int h1_headers_count(const struct h1_headers *h, const char *name)
{
    int c = 0;
    if (!h || !name) return 0;
    for (int i = 0; i < h->n; i++) if (ci_eq(h->v[i].name, name)) c++;
    return c;
}

const char *h1_headers_nth(const struct h1_headers *h, const char *name, int idx)
{
    int c = 0;
    if (!h || !name || idx < 0) return NULL;
    for (int i = 0; i < h->n; i++)
        if (ci_eq(h->v[i].name, name) && c++ == idx) return h->v[i].value;
    return NULL;
}

/* Append " <text>" to the last header's value (obs-fold continuation). */
static int hdr_fold_append(struct h1_headers *h, const char *text, int len)
{
    if (h->n == 0) return H1_E_SYNTAX;              /* fold before any header */
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\r' || c == '\n' || c == 0) return H1_E_SYNTAX;
    }
    char *old = h->v[h->n - 1].value;
    int ol = (int)strlen(old);
    char *nv = (char *)malloc((size_t)ol + 1 + (size_t)len + 1);
    if (!nv) return H1_E_NOMEM;
    memcpy(nv, old, (size_t)ol);
    nv[ol] = ' ';
    if (len) memcpy(nv + ol + 1, text, (size_t)len);
    nv[ol + 1 + len] = 0;
    free(old);
    h->v[h->n - 1].value = nv;
    return H1_OK;
}

/* ---- request ---------------------------------------------------------- */

static int method_valid(const char *m)
{
    if (!m || !*m) return 0;
    int n = 0;
    for (const char *p = m; *p; p++, n++) if (!is_tchar((unsigned char)*p)) return 0;
    return n < H1_METHOD_MAX;
}

/* A request-target may contain no control character and no space: both would
 * terminate the request line early and let the caller forge a second one. */
static int target_valid(const char *t)
{
    if (!t || !*t) return 0;
    for (const char *p = t; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

int h1_request_init(struct h1_request *q, const char *method, const char *target)
{
    if (!q) return H1_E_ARG;
    memset(q, 0, sizeof *q);
    h1_headers_init(&q->hdr);
    if (!method_valid(method) || !target_valid(target)) return H1_E_ARG;
    /* method_valid bounded the length, so this cannot truncate. */
    memcpy(q->method, method, strlen(method) + 1);
    q->target = dupn(target, (int)strlen(target));
    return q->target ? H1_OK : H1_E_NOMEM;
}

void h1_request_free(struct h1_request *q)
{
    if (!q) return;
    free(q->target); q->target = NULL;
    h1_headers_free(&q->hdr);
}

int h1_request_set_target(struct h1_request *q, const char *target)
{
    if (!q || !target_valid(target)) return H1_E_ARG;
    char *t = dupn(target, (int)strlen(target));
    if (!t) return H1_E_NOMEM;
    free(q->target);
    q->target = t;
    return H1_OK;
}

int h1_request_set_header(struct h1_request *q, const char *name, const char *value)
{
    return q ? h1_headers_set(&q->hdr, name, value) : H1_E_ARG;
}

int h1_request_add_header(struct h1_request *q, const char *name, const char *value)
{
    return q ? h1_headers_add(&q->hdr, name, -1, value, -1) : H1_E_ARG;
}

void h1_request_set_body(struct h1_request *q, const void *body, int len)
{
    if (!q) return;
    if (!body || len <= 0) { q->body = NULL; q->body_len = 0; return; }
    q->body = (const uint8_t *)body;
    q->body_len = len;
}

static void u64_str(uint64_t v, char *out)
{
    char tmp[24]; int n = 0;
    do { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

int h1_request_build(const struct h1_request *q, char **out, int *outlen)
{
    if (!q || !out || !outlen) return H1_E_ARG;
    *out = NULL; *outlen = 0;
    /* Revalidate: a caller may have poked the struct directly, and this is the
     * last point before bytes leave the process. */
    if (!method_valid(q->method) || !target_valid(q->target)) return H1_E_ARG;
    if (q->body_len < 0) return H1_E_ARG;
    for (int i = 0; i < q->hdr.n; i++)
        if (!hdr_valid(q->hdr.v[i].name, (int)strlen(q->hdr.v[i].name),
                       q->hdr.v[i].value, (int)strlen(q->hdr.v[i].value)))
            return H1_E_ARG;

    char clen[24]; clen[0] = 0;
    int need_clen = 0;
    if (q->body_len > 0 && !h1_headers_get(&q->hdr, "content-length") &&
        !h1_headers_get(&q->hdr, "transfer-encoding")) {
        u64_str((uint64_t)q->body_len, clen);
        need_clen = 1;
    }

    size_t n = strlen(q->method) + 1 + strlen(q->target) + sizeof(" HTTP/1.1\r\n");
    for (int i = 0; i < q->hdr.n; i++)
        n += strlen(q->hdr.v[i].name) + 2 + strlen(q->hdr.v[i].value) + 2;
    if (need_clen) n += sizeof("Content-Length: ") + strlen(clen) + 2;
    n += 2 + (size_t)q->body_len + 1;

    char *b = (char *)malloc(n);
    if (!b) return H1_E_NOMEM;
    size_t o = 0;
    #define PUT(s) do { size_t l_ = strlen(s); memcpy(b + o, (s), l_); o += l_; } while (0)
    PUT(q->method); b[o++] = ' '; PUT(q->target); PUT(" HTTP/1.1\r\n");
    for (int i = 0; i < q->hdr.n; i++) {
        PUT(q->hdr.v[i].name); b[o++] = ':'; b[o++] = ' ';
        PUT(q->hdr.v[i].value); b[o++] = '\r'; b[o++] = '\n';
    }
    if (need_clen) { PUT("Content-Length: "); PUT(clen); b[o++] = '\r'; b[o++] = '\n'; }
    b[o++] = '\r'; b[o++] = '\n';
    if (q->body_len > 0) { memcpy(b + o, q->body, (size_t)q->body_len); o += (size_t)q->body_len; }
    #undef PUT
    b[o] = 0;                                    /* convenience for tests/logs */
    *out = b; *outlen = (int)o;
    return H1_OK;
}

/* ---- response: buffers ------------------------------------------------ */

void h1_response_init(struct h1_response *r)
{
    memset(r, 0, sizeof *r);
    h1_headers_init(&r->hdr);
    h1_headers_init(&r->trailer);
    r->clen = -1;
    r->remain = 0;
    r->body_max = H1_BODY_MAX;
    r->keep_alive = 1;
    r->state = H1_ST_STATUS;
}

void h1_response_free(struct h1_response *r)
{
    if (!r) return;
    h1_headers_free(&r->hdr);
    h1_headers_free(&r->trailer);
    free(r->body); r->body = NULL;
    free(r->line); r->line = NULL;
    r->body_len = r->body_cap = r->line_len = r->line_cap = 0;
}

void h1_response_reset(struct h1_response *r)
{
    int bm = r->body_max, head = r->head_request;
    h1_body_sink sk = r->sink; void *sc = r->sink_ctx;
    h1_response_free(r);
    h1_response_init(r);
    r->body_max = bm;
    r->head_request = head;
    r->sink = sk; r->sink_ctx = sc;
}

void h1_response_sink(struct h1_response *r, h1_body_sink cb, void *ctx)
{
    if (!r) return;
    r->sink = cb; r->sink_ctx = ctx;
}

/* "The headers are final" is exactly "the parser has left the header states".
 * H1_ST_STATUS/H1_ST_HEADER mean it is still reading them; a 1xx interim
 * response rewinds to H1_ST_STATUS, so this cannot report an interim's headers
 * as the real ones. */
int h1_response_headers_done(const struct h1_response *r)
{
    if (!r) return 0;
    switch (r->state) {
    case H1_ST_STATUS: case H1_ST_HEADER: case H1_ST_ERROR: return 0;
    default: return r->code != 0;
    }
}

int h1_response_streaming(const struct h1_response *r) { return r && r->streaming; }

void h1_response_limit(struct h1_response *r, int body_max)
{
    if (r && body_max > 0) r->body_max = body_max;
}

void h1_response_head(struct h1_response *r, int is_head) { if (r) r->head_request = is_head ? 1 : 0; }

int h1_response_done(const struct h1_response *r) { return r && r->state == H1_ST_DONE; }

static int fail(struct h1_response *r, int err)
{
    r->state = H1_ST_ERROR;
    r->err = err;
    /* Drop whatever body had accumulated.  Two reasons, and the second is the
     * one that matters.  (1) A failed message has no body a caller may use --
     * rendering the first half of a truncated page, or of one whose framing
     * did not add up, is how a partial response gets mistaken for the real
     * thing.  (2) Determinism: how much of an over-cap body had been copied
     * before the limit tripped depends on how the bytes happened to be split
     * across reads, which would make the parse result depend on TCP segment
     * boundaries.  tests/unit/http1_fuzz.c asserts that it does not. */
    free(r->body);
    r->body = NULL;
    r->body_len = r->body_cap = 0;
    return err;
}

/* Grow the body buffer to hold `add` more bytes.  Note what this does NOT do:
 * it never sizes itself from Content-Length.  A server that claims 4 GiB gets
 * no allocation at all until it actually sends bytes, and stops at body_max. */
static int body_push(struct h1_response *r, const uint8_t *p, int n)
{
    if (n <= 0) return H1_OK;
    r->body_seen += n;
    /* Streaming: the bytes leave immediately and nothing accumulates, so the
     * body cap -- which exists to bound OUR allocation -- does not apply.  An
     * SSE conversation legitimately outlives 8 MiB; what bounds memory on this
     * path is the consumer, and the caller applies backpressure by declining
     * to pump while its queue is full. */
    if (r->streaming) return r->sink(r->sink_ctx, p, n);
    if (n > r->body_max - r->body_len) return H1_E_TOOLARGE;
    if (r->body_len + n + 1 > r->body_cap) {
        int nc = r->body_cap ? r->body_cap : 4096;
        while (nc < r->body_len + n + 1) {
            if (nc > r->body_max) { nc = r->body_max + 1; break; }
            nc *= 2;
        }
        if (nc > r->body_max + 1) nc = r->body_max + 1;
        if (nc < r->body_len + n + 1) return H1_E_TOOLARGE;
        uint8_t *nb = (uint8_t *)realloc(r->body, (size_t)nc);
        if (!nb) return H1_E_NOMEM;
        r->body = nb; r->body_cap = nc;
    }
    memcpy(r->body + r->body_len, p, (size_t)n);
    r->body_len += n;
    r->body[r->body_len] = 0;
    return H1_OK;
}

/* Accumulate one CRLF- (or bare-LF-) terminated line.  Returns bytes consumed
 * (always >= 1 when len > 0, so the caller's loop cannot spin), and sets
 * r->have_line when a complete line is in r->line. */
static int line_feed(struct h1_response *r, const uint8_t *p, int len)
{
    const uint8_t *nl = (const uint8_t *)memchr(p, '\n', (size_t)len);
    int take = nl ? (int)(nl - p) + 1 : len;
    int keep = nl ? take - 1 : take;             /* drop the LF itself */

    if (r->line_len + keep > H1_LINE_MAX) return H1_E_TOOLARGE;
    if (r->line_len + keep + 1 > r->line_cap) {
        int nc = r->line_cap ? r->line_cap : 256;
        while (nc < r->line_len + keep + 1) nc *= 2;
        char *nb = (char *)realloc(r->line, (size_t)nc);
        if (!nb) return H1_E_NOMEM;
        r->line = nb; r->line_cap = nc;
    }
    if (keep) memcpy(r->line + r->line_len, p, (size_t)keep);
    r->line_len += keep;
    r->line[r->line_len] = 0;

    if (nl) {
        if (r->line_len && r->line[r->line_len - 1] == '\r') r->line[--r->line_len] = 0;
        /* Any CR still inside the line was NOT a line terminator: it is an
         * injected control character.  Same for NUL.  Reject the message. */
        for (int i = 0; i < r->line_len; i++)
            if (r->line[i] == '\r' || r->line[i] == 0) return H1_E_SYNTAX;
        r->have_line = 1;
    }
    return take;
}

/* ---- response: line parsers ------------------------------------------ */

static int parse_status_line(struct h1_response *r)
{
    const char *s = r->line;
    int n = r->line_len;
    /* "HTTP/1.x SP ddd [SP reason]" -- nothing looser.  In particular a
     * response that does not start with HTTP/ is not HTTP/0.9, it is a
     * mis-dialled port or a captive portal, and guessing helps nobody. */
    if (n < 12) return H1_E_SYNTAX;
    if (memcmp(s, "HTTP/1.", 7) != 0) return H1_E_SYNTAX;
    if (!is_digit((unsigned char)s[7])) return H1_E_SYNTAX;
    r->minor = s[7] - '0';
    if (s[8] != ' ') return H1_E_SYNTAX;
    if (!is_digit((unsigned char)s[9]) || !is_digit((unsigned char)s[10]) ||
        !is_digit((unsigned char)s[11])) return H1_E_SYNTAX;
    r->code = (s[9] - '0') * 100 + (s[10] - '0') * 10 + (s[11] - '0');
    if (n > 12 && s[12] != ' ') return H1_E_SYNTAX;
    int ro = 0;
    for (int i = 13; i < n && ro < H1_REASON_MAX - 1; i++) r->reason[ro++] = s[i];
    r->reason[ro] = 0;
    return H1_OK;
}

static int parse_header_line(struct h1_response *r, struct h1_headers *into)
{
    const char *s = r->line;
    int n = r->line_len;

    if (n && is_ws((unsigned char)s[0])) {        /* obs-fold continuation */
        int b = 0, e = n;
        while (b < e && is_ws((unsigned char)s[b])) b++;
        while (e > b && is_ws((unsigned char)s[e - 1])) e--;
        return hdr_fold_append(into, s + b, e - b);
    }
    int c = 0;
    while (c < n && s[c] != ':') c++;
    if (c == n) return H1_E_SYNTAX;               /* no colon */
    if (c == 0) return H1_E_SYNTAX;               /* empty name */
    for (int i = 0; i < c; i++)
        if (!is_tchar((unsigned char)s[i])) return H1_E_SYNTAX;   /* also kills "Name :" */
    int vb = c + 1, ve = n;
    while (vb < ve && is_ws((unsigned char)s[vb])) vb++;
    while (ve > vb && is_ws((unsigned char)s[ve - 1])) ve--;
    return h1_headers_add(into, s, c, s + vb, ve - vb);
}

/* Parse an unsigned decimal with hard overflow detection.  Returns 0 and sets
 * *out, or -1.  `len` bytes, no sign accepted: a "-1" Content-Length is not a
 * negative number, it is a malformed message. */
static int parse_u64(const char *s, int len, uint64_t *out)
{
    if (len <= 0) return -1;
    uint64_t v = 0;
    for (int i = 0; i < len; i++) {
        if (!is_digit((unsigned char)s[i])) return -1;
        if (v > (uint64_t)0x1999999999999999ULL) return -1;      /* *10 would overflow */
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

/* Content-Length may appear more than once, and one instance may be a comma
 * list.  RFC 9112 6.3: if the values disagree, the message is malformed --
 * and disagreeing Content-Lengths are the classic request-smuggling primitive,
 * so this is a reject, not a "pick one". */
static int collect_clen(const struct h1_headers *h, int64_t *out, int *present)
{
    *present = 0;
    uint64_t agreed = 0;
    for (int i = 0; i < h->n; i++) {
        if (!ci_eq(h->v[i].name, "content-length")) continue;
        const char *v = h->v[i].value;
        int len = (int)strlen(v), b = 0;
        while (b <= len) {
            int e = b;
            while (e < len && v[e] != ',') e++;
            int tb = b, te = e;
            while (tb < te && is_ws((unsigned char)v[tb])) tb++;
            while (te > tb && is_ws((unsigned char)v[te - 1])) te--;
            uint64_t x;
            if (parse_u64(v + tb, te - tb, &x) != 0) return H1_E_SYNTAX;
            if (*present && x != agreed) return H1_E_SYNTAX;
            agreed = x; *present = 1;
            if (e >= len) break;
            b = e + 1;
        }
    }
    if (*present) {
        if (agreed > (uint64_t)0x7FFFFFFFFFFFFFFFULL) return H1_E_SYNTAX;
        *out = (int64_t)agreed;
    }
    return H1_OK;
}

/* Is the LAST transfer coding "chunked"?  Only the final coding frames the
 * message; "gzip, chunked" is chunked, "chunked, gzip" is not framed by us
 * and must be treated as unframed (close-delimited). */
static int te_final_is_chunked(const struct h1_headers *h, int *present)
{
    const char *last = NULL;
    int lastlen = 0;
    *present = 0;
    for (int i = 0; i < h->n; i++) {
        if (!ci_eq(h->v[i].name, "transfer-encoding")) continue;
        *present = 1;
        const char *v = h->v[i].value;
        int len = (int)strlen(v), b = 0;
        while (b <= len) {
            int e = b;
            while (e < len && v[e] != ',') e++;
            int tb = b, te = e;
            while (tb < te && is_ws((unsigned char)v[tb])) tb++;
            while (te > tb && is_ws((unsigned char)v[te - 1])) te--;
            if (te > tb) { last = v + tb; lastlen = te - tb; }
            if (e >= len) break;
            b = e + 1;
        }
    }
    if (!last || lastlen != 7) return 0;
    return ci_eq_n(last, "chunked", 7);
}

/* Does a comma-separated Connection header list `tok`? */
static int conn_has(const struct h1_headers *h, const char *tok)
{
    int tl = (int)strlen(tok);
    for (int i = 0; i < h->n; i++) {
        if (!ci_eq(h->v[i].name, "connection")) continue;
        const char *v = h->v[i].value;
        int len = (int)strlen(v), b = 0;
        while (b <= len) {
            int e = b;
            while (e < len && v[e] != ',') e++;
            int tb = b, te = e;
            while (tb < te && is_ws((unsigned char)v[tb])) tb++;
            while (te > tb && is_ws((unsigned char)v[te - 1])) te--;
            if (te - tb == tl && ci_eq_n(v + tb, tok, tl)) return 1;
            if (e >= len) break;
            b = e + 1;
        }
    }
    return 0;
}

/* Headers are complete: decide how the body is framed. */
static int begin_body(struct h1_response *r)
{
    int te_present = 0, cl_present = 0;
    int chunked = te_final_is_chunked(&r->hdr, &te_present);
    int64_t cl = -1;
    int rc = collect_clen(&r->hdr, &cl, &cl_present);
    if (rc != H1_OK) return rc;

    r->keep_alive = (r->minor >= 1) ? 1 : conn_has(&r->hdr, "keep-alive");
    if (conn_has(&r->hdr, "close")) r->keep_alive = 0;

    /* 1xx is an interim response: header section only, then the real one
     * follows on the same connection.  100-continue is the common case. */
    if (r->code >= 100 && r->code < 200 && r->code != 101) {
        if (++r->interim > H1_MAX_INTERIM) return H1_E_SYNTAX;
        h1_headers_free(&r->hdr);
        h1_headers_init(&r->hdr);
        r->code = 0; r->reason[0] = 0; r->hdr_bytes = 0;
        r->state = H1_ST_STATUS;
        return H1_OK;
    }

    r->chunked = chunked;
    r->clen = cl_present ? cl : -1;

    /* Incremental delivery, decided here because this is the first moment the
     * answer is knowable and the last moment before a body byte can arrive.
     * A Content-Encoding we have to undo needs the whole body (the inflater is
     * one-shot), so those responses keep buffering. */
    if (r->sink) {
        const char *ce = h1_headers_get(&r->hdr, "content-encoding");
        while (ce && is_ws((unsigned char)*ce)) ce++;
        r->streaming = (!ce || !*ce || ci_eq(ce, "identity"));
    }

    if (r->head_request || r->code == 204 || r->code == 304 || r->code == 101) {
        r->no_body = 1;
        if (r->code == 101) { r->keep_alive = 0; r->must_close = 1; }
        r->state = H1_ST_DONE;
        return H1_OK;
    }

    if (te_present) {
        if (cl_present) {
            /* Both present: TE wins, but the peer disagreed with itself about
             * where the message ends.  RFC 9112 says a user agent MUST close
             * the connection -- reusing it is how a smuggled response gets
             * served to the next request. */
            r->clen = -1;
            r->must_close = 1;
            r->keep_alive = 0;
        }
        if (chunked) { r->state = H1_ST_CHUNK_SIZE; return H1_OK; }
        r->must_close = 1; r->keep_alive = 0;
        r->state = H1_ST_BODY_EOF;
        return H1_OK;
    }
    if (cl_present) {
        if (!r->streaming && cl > (int64_t)r->body_max) return H1_E_TOOLARGE;
        r->remain = cl;
        r->state = cl ? H1_ST_BODY_LEN : H1_ST_DONE;
        return H1_OK;
    }
    /* Neither: the body ends at connection close.  Nothing can be pooled. */
    r->must_close = 1; r->keep_alive = 0;
    r->state = H1_ST_BODY_EOF;
    return H1_OK;
}

/* Chunk size line: 1..16 hex digits, then an optional ";ext" run.  Anything
 * else -- a sign, whitespace, 0x, an empty line -- is malformed.  A 17th digit
 * is an overflow attempt, not a big chunk. */
static int parse_chunk_size(struct h1_response *r)
{
    const char *s = r->line;
    int n = r->line_len, i = 0;
    uint64_t v = 0;
    int digits = 0;
    while (i < n) {
        int c = (unsigned char)s[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (++digits > 16) return H1_E_SYNTAX;
        v = (v << 4) | (uint64_t)d;
        i++;
    }
    if (!digits) return H1_E_SYNTAX;
    if (i < n) {
        /* Only a chunk extension may follow, after optional BWS. */
        while (i < n && is_ws((unsigned char)s[i])) i++;
        if (i < n && s[i] != ';') return H1_E_SYNTAX;
    }
    if (v > (uint64_t)0x7FFFFFFFFFFFFFFFULL) return H1_E_SYNTAX;
    /* The cap bounds what we ALLOCATE. A streamed chunk is never allocated, so
     * it is bounded by the consumer instead -- see body_push. */
    if (!r->streaming && v > (uint64_t)(r->body_max - r->body_len)) return H1_E_TOOLARGE;
    r->remain = (int64_t)v;
    r->state = v ? H1_ST_CHUNK_DATA : H1_ST_TRAILER;
    return H1_OK;
}

static int on_line(struct h1_response *r)
{
    int rc;
    switch (r->state) {
    case H1_ST_STATUS:
        if (r->line_len == 0) {
            /* Leading blank lines before a status line: RFC 9112 lets a
             * recipient skip a bounded number (broken servers emit them). */
            if ((r->hdr_bytes += 2) > 4096) return H1_E_SYNTAX;
            return H1_OK;
        }
        rc = parse_status_line(r);
        if (rc != H1_OK) return rc;
        r->hdr_bytes += r->line_len + 2;
        r->state = H1_ST_HEADER;
        return H1_OK;

    case H1_ST_HEADER:
        if ((r->hdr_bytes += r->line_len + 2) > H1_HDR_MAX) return H1_E_TOOLARGE;
        if (r->line_len == 0) return begin_body(r);
        return parse_header_line(r, &r->hdr);

    case H1_ST_CHUNK_SIZE:
        if ((r->hdr_bytes += r->line_len + 2) > H1_HDR_MAX + H1_LINE_MAX) return H1_E_TOOLARGE;
        return parse_chunk_size(r);

    case H1_ST_CHUNK_CRLF:
        if (r->line_len != 0) return H1_E_SYNTAX;
        r->state = H1_ST_CHUNK_SIZE;
        return H1_OK;

    case H1_ST_TRAILER:
        if (r->line_len == 0) { r->state = H1_ST_DONE; return H1_OK; }
        if (r->trailer.n >= H1_MAX_HEADERS) return H1_E_TOOLARGE;
        return parse_header_line(r, &r->trailer);

    default:
        return H1_E_SYNTAX;
    }
}

int h1_response_feed(struct h1_response *r, const void *data, int len)
{
    if (!r || len < 0 || (!data && len)) return H1_E_ARG;
    if (r->state == H1_ST_ERROR) return r->err;
    const uint8_t *p = (const uint8_t *)data;
    int off = 0;

    while (off < len && r->state != H1_ST_DONE && r->state != H1_ST_ERROR) {
        switch (r->state) {
        case H1_ST_STATUS: case H1_ST_HEADER: case H1_ST_CHUNK_SIZE:
        case H1_ST_CHUNK_CRLF: case H1_ST_TRAILER: {
            int used = line_feed(r, p + off, len - off);
            if (used < 0) return fail(r, used);
            off += used;
            if (!r->have_line) break;
            int rc = on_line(r);
            r->line_len = 0; r->have_line = 0;
            if (r->line) r->line[0] = 0;
            if (rc != H1_OK) return fail(r, rc);
            break;
        }
        case H1_ST_BODY_LEN: case H1_ST_CHUNK_DATA: {
            int avail = len - off;
            int take = (r->remain < (int64_t)avail) ? (int)r->remain : avail;
            int rc = body_push(r, p + off, take);
            if (rc != H1_OK) return fail(r, rc);
            off += take;
            r->remain -= take;
            if (r->remain == 0)
                r->state = (r->state == H1_ST_BODY_LEN) ? H1_ST_DONE : H1_ST_CHUNK_CRLF;
            break;
        }
        case H1_ST_BODY_EOF: {
            int rc = body_push(r, p + off, len - off);
            if (rc != H1_OK) return fail(r, rc);
            off = len;
            break;
        }
        default:
            return fail(r, H1_E_SYNTAX);
        }
    }
    return off;
}

int h1_response_eof(struct h1_response *r)
{
    if (!r) return H1_E_ARG;
    if (r->state == H1_ST_ERROR) return r->err;
    if (r->state == H1_ST_DONE) return H1_OK;
    if (r->state == H1_ST_BODY_EOF) {
        /* This is the legitimate ending for an unframed body. */
        r->state = H1_ST_DONE;
        r->keep_alive = 0;
        return H1_OK;
    }
    /* Everything else -- half a header, a chunk cut in two, a Content-Length
     * body that came up short -- is truncation.  It must be an error and not
     * a short body, or a network-cut page renders as a silently partial one. */
    return fail(r, H1_E_TRUNC);
}

/* ---- content coding ---------------------------------------------------- */

const char *h1_accept_encoding(void) { return "gzip, deflate"; }

/* CRC-32 (RFC 1952), nibble-table: 16 entries instead of 256, since this runs
 * once per compressed response and not per pixel. */
static uint32_t crc32_of(const uint8_t *p, int n)
{
    static const uint32_t t[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu
    };
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        c = (c >> 4) ^ t[c & 0xF];
        c = (c >> 4) ^ t[c & 0xF];
    }
    return c ^ 0xFFFFFFFFu;
}

/* Inflate with a growing output buffer.  inflate_raw cannot distinguish "out
 * of room" from "corrupt stream", so on failure we retry larger, bounded by
 * `cap`.  `hint` is a size the stream claims -- treated as a hint only, never
 * as an allocation the peer gets to pick. */
static int inflate_grow(int zlib_wrapped, const uint8_t *in, int inlen,
                        int hint, int cap, uint8_t **out, int *outlen)
{
    if (inlen <= 0 || cap <= 0) return H1_E_ENCODING;
    int try_cap = hint > 0 ? hint : 0;
    if (try_cap > cap || try_cap <= 0) try_cap = 0;
    if (try_cap == 0) {
        try_cap = inlen < (1 << 28) ? inlen * 4 : cap;
        if (try_cap < 65536) try_cap = 65536;
        if (try_cap > cap) try_cap = cap;
    }
    for (;;) {
        uint8_t *buf = (uint8_t *)malloc((size_t)try_cap + 1);
        if (!buf) return H1_E_NOMEM;
        int got = 0;
        int rc = zlib_wrapped ? zlib_decompress(in, inlen, buf, try_cap, &got)
                              : inflate_raw(in, inlen, buf, try_cap, &got);
        if (rc == 0 && got >= 0 && got <= try_cap) {
            buf[got] = 0;
            *out = buf; *outlen = got;
            return H1_OK;
        }
        free(buf);
        if (try_cap >= cap) return H1_E_ENCODING;
        if (try_cap > cap / 4) try_cap = cap; else try_cap *= 4;
    }
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* RFC 1952: a gzip member is a variable-length header, a raw DEFLATE stream,
 * and an 8-byte CRC32+ISIZE footer.  The header is variable because of the
 * optional FEXTRA/FNAME/FCOMMENT/FHCRC fields -- every one of which is a
 * length or a NUL scan controlled by the sender, so every one is bounded here. */
static int gunzip(const uint8_t *p, int n, int cap, uint8_t **out, int *outlen)
{
    if (n < 18) return H1_E_ENCODING;
    if (p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) return H1_E_ENCODING;
    int flg = p[3];
    if (flg & 0xE0) return H1_E_ENCODING;                    /* reserved bits */
    int off = 10;
    if (flg & 0x04) {                                        /* FEXTRA */
        if (off + 2 > n) return H1_E_ENCODING;
        int xlen = p[off] | (p[off + 1] << 8);
        off += 2;
        if (xlen > n - off) return H1_E_ENCODING;
        off += xlen;
    }
    if (flg & 0x08) {                                        /* FNAME */
        while (off < n && p[off]) off++;
        if (off >= n) return H1_E_ENCODING;
        off++;
    }
    if (flg & 0x10) {                                        /* FCOMMENT */
        while (off < n && p[off]) off++;
        if (off >= n) return H1_E_ENCODING;
        off++;
    }
    if (flg & 0x02) { if (off + 2 > n) return H1_E_ENCODING; off += 2; }   /* FHCRC */
    if (off + 8 > n) return H1_E_ENCODING;

    uint32_t want_crc = le32(p + n - 8);
    uint32_t isize    = le32(p + n - 4);
    int hint = (isize <= (uint32_t)cap) ? (int)isize : 0;

    int rc = inflate_grow(0, p + off, n - off - 8, hint, cap, out, outlen);
    if (rc != H1_OK) return rc;
    /* ISIZE is the length mod 2^32 and CRC32 is over the whole output: both
     * are cheap and both catch a corrupted or truncated member that inflate
     * happened to accept. */
    if ((uint32_t)*outlen != isize || crc32_of(*out, *outlen) != want_crc) {
        free(*out); *out = NULL; *outlen = 0;
        return H1_E_ENCODING;
    }
    return H1_OK;
}

/* Split Content-Encoding into up to 4 codings, left to right. */
static int split_codings(const struct h1_headers *h, char out[4][16])
{
    int n = 0;
    for (int i = 0; i < h->n; i++) {
        if (!ci_eq(h->v[i].name, "content-encoding")) continue;
        const char *v = h->v[i].value;
        int len = (int)strlen(v), b = 0;
        while (b <= len) {
            int e = b;
            while (e < len && v[e] != ',') e++;
            int tb = b, te = e;
            while (tb < te && is_ws((unsigned char)v[tb])) tb++;
            while (te > tb && is_ws((unsigned char)v[te - 1])) te--;
            if (te > tb) {
                if (n >= 4) return -1;
                if (te - tb >= 16) return -1;
                for (int k = 0; k < te - tb; k++) out[n][k] = (char)lc((unsigned char)v[tb + k]);
                out[n][te - tb] = 0;
                n++;
            }
            if (e >= len) break;
            b = e + 1;
        }
    }
    return n;
}

int h1_decode_body(struct h1_response *r)
{
    if (!r) return H1_E_ARG;
    if (r->decoded) return H1_OK;
    char cod[4][16];
    int nc = split_codings(&r->hdr, cod);
    if (nc < 0) return H1_E_ENCODING;
    r->decoded = 1;
    if (nc == 0) return H1_OK;

    /* Codings are listed in the order they were applied, so undo right to left. */
    for (int i = nc - 1; i >= 0; i--) {
        const char *c = cod[i];
        if (!strcmp(c, "identity")) continue;
        if (r->body_len <= 0) return H1_E_ENCODING;
        uint8_t *out = NULL; int olen = 0, rc;
        if (!strcmp(c, "gzip") || !strcmp(c, "x-gzip")) {
            rc = gunzip(r->body, r->body_len, r->body_max, &out, &olen);
        } else if (!strcmp(c, "deflate")) {
            /* Nominally RFC 1950 zlib.  A meaningful minority of servers send
             * a bare RFC 1951 stream under this name, and every real browser
             * falls back, so we do too. */
            rc = inflate_grow(1, r->body, r->body_len, 0, r->body_max, &out, &olen);
            if (rc != H1_OK)
                rc = inflate_grow(0, r->body, r->body_len, 0, r->body_max, &out, &olen);
        } else {
            return H1_E_ENCODING;               /* br, zstd, compress: not ours */
        }
        if (rc != H1_OK) return rc;
        free(r->body);
        r->body = out; r->body_len = olen; r->body_cap = olen + 1;
    }
    h1_headers_remove(&r->hdr, "content-encoding");
    return H1_OK;
}

/* ---- redirects --------------------------------------------------------- */

int h1_is_redirect(int code)
{
    return code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
}

int h1_redirect_method(int code, const char *method, char *out, int outmax, int *drop_body)
{
    if (!method || !out || outmax < H1_METHOD_MAX) return H1_E_ARG;
    int is_head = ci_eq(method, "HEAD");
    int keep = (code == 307 || code == 308);
    if (keep || is_head) {
        size_t l = strlen(method);
        if ((int)l >= outmax) return H1_E_ARG;
        memcpy(out, method, l + 1);
        if (drop_body) *drop_body = 0;
        return H1_OK;
    }
    /* 303 mandates GET; 301/302 after a POST are GET in every deployed browser
     * and servers depend on it.  Anything already GET stays GET. */
    memcpy(out, "GET", 4);
    if (drop_body) *drop_body = 1;
    return H1_OK;
}

/* ---- one exchange ------------------------------------------------------ */

int h1_conn_start(struct h1_conn *c, const struct h1_transport *t, char *req, int reqlen)
{
    if (!c || !t || !t->read || !t->write || !req || reqlen <= 0) return H1_E_ARG;
    memset(c, 0, sizeof *c);
    c->t = *t;
    c->out = req; c->out_len = reqlen; c->out_off = 0;
    h1_response_init(&c->resp);
    c->state = H1_C_SEND;
    return H1_OK;
}

void h1_conn_free(struct h1_conn *c)
{
    if (!c) return;
    free(c->out); c->out = NULL;
    h1_response_free(&c->resp);
}

static int conn_fail(struct h1_conn *c, int err)
{
    c->state = H1_C_ERROR;
    c->err = err;
    return c->state;
}

int h1_conn_pump(struct h1_conn *c)
{
    if (!c) return H1_C_ERROR;
    if (c->state == H1_C_DONE || c->state == H1_C_ERROR) return c->state;

    if (c->state == H1_C_SEND) {
        while (c->out_off < c->out_len) {
            if (c->t.poll && c->t.poll(c->t.ctx, 1) == 0) return c->state;
            int n = c->t.write(c->t.ctx, c->out + c->out_off, c->out_len - c->out_off);
            if (n == H1_AGAIN) return c->state;
            if (n < 0) return conn_fail(c, H1_E_TRANSPORT);
            c->out_off += n;
        }
        c->state = H1_C_RECV;
    }

    /* One read per pump, so a fast server cannot starve the caller's event
     * loop (several requests are meant to make progress in parallel).
     *
     * THE POLL HINT DOES NOT GATE THE READ, and that asymmetry is the whole
     * point of this comment. A readiness hint may be trusted when it says
     * "ready" -- at worst a read comes back H1_AGAIN. It may NEVER be trusted
     * when it says "not ready", because a layered transport holds bytes the
     * hint cannot see. Our own TLS is exactly that: tls_rec_pull() drains the
     * whole TCP receive buffer into the session's record buffer and then hands
     * back ONE record's plaintext, so after the first read the socket reports
     * `tcp_available() == 0` and `tls_pending() == 0` while several complete,
     * still-encrypted records sit in the session. Gating on that hint stalled
     * every https response after its status line until the next wire event --
     * for www.baidu.com, the server's close_notify SIXTY SECONDS later.
     *
     * `read` is the only authority on whether there are bytes. It is a cheap
     * call that returns H1_AGAIN when there are none, so asking it costs one
     * call per idle request per pump and buys a stall that cannot happen. */
    uint8_t buf[4096];
    int n = c->t.read(c->t.ctx, buf, (int)sizeof buf);
    if (n == H1_AGAIN) return c->state;
    if (n == H1_EOF) {
        int rc = h1_response_eof(&c->resp);
        if (rc != H1_OK) return conn_fail(c, rc);
        c->state = H1_C_DONE;
        return c->state;
    }
    if (n < 0) return conn_fail(c, H1_E_TRANSPORT);
    c->rx_bytes += (uint64_t)n;

    int used = h1_response_feed(&c->resp, buf, n);
    if (used < 0) return conn_fail(c, used);
    if (used < n) {
        /* Bytes past the message boundary belong to whatever comes next on a
         * pooled connection.  If more arrived than we can hold, the safe move
         * is to stop reusing the connection rather than silently drop them. */
        int extra = n - used;
        if (extra > (int)sizeof c->spill - c->spill_len) {
            c->resp.keep_alive = 0;
        } else {
            memcpy(c->spill + c->spill_len, buf + used, (size_t)extra);
            c->spill_len += extra;
        }
    }
    if (h1_response_done(&c->resp)) c->state = H1_C_DONE;
    return c->state;
}

uint64_t h1_conn_progress(const struct h1_conn *c)
{
    if (!c) return 0;
    return (uint64_t)c->out_off + c->rx_bytes;
}
