/* js_url.c -- the WHATWG URL Standard: the basic URL parser as a real state
 * machine, host parsing (IPv4/IPv6/opaque/domain+punycode), the eleven
 * component getters and setters, application/x-www-form-urlencoded, and the
 * `URL` and `URLSearchParams` globals.
 *
 * Read js_url.h first: it says why this is a second parser rather than a
 * binding over c/net/http/url.c, why it is one TU, and where IDNA stops.
 *
 * THE SHAPE OF THE ALGORITHM IS THE SPEC'S, DELIBERATELY.  The states below
 * are the spec's states with the spec's names (SCHEME_START, NO_SCHEME,
 * SPECIAL_RELATIVE_OR_AUTHORITY, ...), the pointer moves the way the spec's
 * pointer moves (including the decrements), and the buffer is the spec's
 * buffer.  That is not slavishness: the corpus is exhaustive precisely because
 * every browser tried to write this as a splitter first, and the only way to
 * answer "why does this case do that" in a hurry is for the code and the
 * prose to have the same joints.  Where this file departs from the text it
 * says so at the departure.
 *
 * WHAT IS UTF-8 HERE AND WHAT IS CODE POINTS.  The spec iterates code points.
 * Every code point with syntactic meaning in a URL is ASCII, and percent-
 * encoding a code point is defined as percent-encoding the bytes of its UTF-8
 * encoding -- so the state machine iterates BYTES and gets the same answer,
 * with one exception each way: (a) input is sanitised to well-formed UTF-8 on
 * entry, because a lone surrogate from a JS string is not a scalar value and
 * the spec's "UTF-8 encode" would produce U+FFFD; (b) the host path and the
 * searchParams sort decode to code points explicitly, because IDNA and
 * UTF-16 code-unit ordering are genuinely about characters. */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "js_url.h"

/* ===================================================================== *
 *  small allocation + string helpers
 * ===================================================================== */

static void *xalloc(int n)
{
    void *p = malloc((size_t)(n > 0 ? n : 1));
    return p;
}

static char *xstrndup(const char *s, int n)
{
    char *d = (char *)xalloc(n + 1);
    if (!d) return 0;
    if (n > 0) memcpy(d, s, (size_t)n);
    d[n] = 0;
    return d;
}

static char *xstrdup(const char *s) { return xstrndup(s, s ? (int)strlen(s) : 0); }

/* A growable byte buffer. Growth is copy-into-a-new-block rather than realloc:
 * mini-libc has realloc, but this file is also linked into the kernel-adjacent
 * host tests and one allocator primitive is one less thing to be surprised by. */
typedef struct ub { char *p; int n, cap; } ub;

static void ub_init(ub *b) { b->p = 0; b->n = 0; b->cap = 0; }
static void ub_free(ub *b) { free(b->p); b->p = 0; b->n = b->cap = 0; }
static void ub_clear(ub *b) { b->n = 0; if (b->p) b->p[0] = 0; }

static int ub_grow(ub *b, int need)
{
    if (b->n + need + 1 <= b->cap) return 1;
    int nc = b->cap ? b->cap * 2 : 32;
    while (nc < b->n + need + 1) nc *= 2;
    char *np = (char *)xalloc(nc);
    if (!np) return 0;
    if (b->n) memcpy(np, b->p, (size_t)b->n);
    np[b->n] = 0;
    free(b->p);
    b->p = np; b->cap = nc;
    return 1;
}

static void ub_putc(ub *b, int c)
{
    if (!ub_grow(b, 1)) return;
    b->p[b->n++] = (char)c;
    b->p[b->n] = 0;
}

static void ub_put(ub *b, const char *s, int n)
{
    if (n <= 0) return;
    if (!ub_grow(b, n)) return;
    memcpy(b->p + b->n, s, (size_t)n);
    b->n += n;
    b->p[b->n] = 0;
}

static void ub_puts(ub *b, const char *s) { if (s) ub_put(b, s, (int)strlen(s)); }

static void ub_putu(ub *b, unsigned long v)
{
    char t[24]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) ub_putc(b, t[--i]);
}

/* Detach the buffer as a NUL-terminated malloc'd string. Never returns NULL
 * for a buffer that was written to; an untouched buffer yields "". */
static char *ub_take(ub *b)
{
    if (!b->p) return xstrdup("");
    char *r = b->p;
    b->p = 0; b->n = b->cap = 0;
    return r;
}

static int ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_alnum(int c) { return is_alpha(c) || is_digit(c); }
static int is_hex(int c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int streq_ci(const char *a, const char *b)
{
    while (*a && *b) { if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b)) return 0; a++; b++; }
    return *a == *b;
}

/* ===================================================================== *
 *  UTF-8: sanitise on entry, decode where characters actually matter
 * ===================================================================== */

/* Decode one scalar value at s[i]; returns the code point and advances *i.
 * Malformed sequences and surrogate encodings yield U+FFFD over one byte,
 * which is what "UTF-8 decode without BOM" does. */
static unsigned utf8_next(const unsigned char *s, int n, int *i)
{
    int p = *i;
    unsigned c = s[p];
    if (c < 0x80) { *i = p + 1; return c; }
    int need, cp, lo;
    if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; lo = 0x80; }
    else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; lo = 0x800; }
    else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; lo = 0x10000; }
    else { *i = p + 1; return 0xFFFD; }
    if (p + need >= n) { *i = p + 1; return 0xFFFD; }
    for (int k = 1; k <= need; k++) {
        unsigned cc = s[p + k];
        if ((cc & 0xC0) != 0x80) { *i = p + 1; return 0xFFFD; }
        cp = (cp << 6) | (int)(cc & 0x3F);
    }
    if (cp < lo || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { *i = p + 1; return 0xFFFD; }
    *i = p + need + 1;
    return (unsigned)cp;
}

static void ub_putcp(ub *b, unsigned cp)
{
    if (cp < 0x80) ub_putc(b, (int)cp);
    else if (cp < 0x800) { ub_putc(b, (int)(0xC0 | (cp >> 6))); ub_putc(b, (int)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        ub_putc(b, (int)(0xE0 | (cp >> 12)));
        ub_putc(b, (int)(0x80 | ((cp >> 6) & 0x3F)));
        ub_putc(b, (int)(0x80 | (cp & 0x3F)));
    } else {
        ub_putc(b, (int)(0xF0 | (cp >> 18)));
        ub_putc(b, (int)(0x80 | ((cp >> 12) & 0x3F)));
        ub_putc(b, (int)(0x80 | ((cp >> 6) & 0x3F)));
        ub_putc(b, (int)(0x80 | (cp & 0x3F)));
    }
}

/* Re-encode `s` as well-formed UTF-8, replacing anything that is not a scalar
 * value with U+FFFD. Cheap ASCII fast path: most URLs never leave it. */
static char *utf8_sanitize(const char *s, int len, int *outlen)
{
    int ascii = 1;
    for (int i = 0; i < len; i++) if ((unsigned char)s[i] >= 0x80) { ascii = 0; break; }
    if (ascii) { if (outlen) *outlen = len; return xstrndup(s, len); }
    ub b; ub_init(&b);
    const unsigned char *u = (const unsigned char *)s;
    int i = 0;
    while (i < len) ub_putcp(&b, utf8_next(u, len, &i));
    if (outlen) *outlen = b.n;
    return ub_take(&b);
}

/* ===================================================================== *
 *  percent-encoding sets  (URL Standard section 1.3)
 * ===================================================================== */

static int in_set(int set, unsigned char c)
{
    /* C0 control percent-encode set: C0 controls and everything above ~ */
    if (c <= 0x1F || c > 0x7E) return 1;
    if (set == PCT_C0) return 0;

    if (set == PCT_FRAGMENT)
        return c == ' ' || c == '"' || c == '<' || c == '>' || c == '`';

    /* query set, and everything layered on it */
    int q = (c == ' ' || c == '"' || c == '#' || c == '<' || c == '>');
    if (set == PCT_QUERY) return q;
    if (set == PCT_SPECIAL_QUERY) return q || c == '\'';

    /* path = query + ? ` { } ^
     * The '^' is not in the set as most references state it; the corpus is
     * explicit that it must be encoded in a path (urltestdata.json, the two
     * "foo://host/ !\"$%&'()*+,-./:;<=>@[\\]^_`{|}~" cases, for a special and
     * a non-special scheme both), and the corpus is generated from the spec. */
    int path = q || c == '?' || c == '`' || c == '{' || c == '}' || c == '^';
    if (set == PCT_PATH) return path;

    /* userinfo = path + / : ; = @ [ \ ] ^ | */
    int ui = path || c == '/' || c == ':' || c == ';' || c == '=' || c == '@' ||
             c == '[' || c == '\\' || c == ']' || c == '^' || c == '|';
    if (set == PCT_USERINFO) return ui;

    /* component = userinfo + $ % & + , */
    int comp = ui || c == '$' || c == '%' || c == '&' || c == '+' || c == ',';
    if (set == PCT_COMPONENT) return comp;

    /* application/x-www-form-urlencoded = component + ! ' ( ) ~ ; and 0x20
     * is handled by the serializer, not here (it becomes '+'). */
    if (set == PCT_FORM)
        return comp || c == '!' || c == '\'' || c == '(' || c == ')' || c == '~';
    return 0;
}

static void ub_pct(ub *b, unsigned char c)
{
    static const char *H = "0123456789ABCDEF";
    ub_putc(b, '%'); ub_putc(b, H[c >> 4]); ub_putc(b, H[c & 15]);
}

static void ub_put_pct(ub *b, const char *s, int len, int set)
{
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (in_set(set, c)) ub_pct(b, c); else ub_putc(b, (int)c);
    }
}

char *url_percent_encode(const char *s, int len, int set)
{
    if (len < 0) len = s ? (int)strlen(s) : 0;
    ub b; ub_init(&b);
    ub_put_pct(&b, s, len, set);
    return ub_take(&b);
}

char *url_percent_decode(const char *s, int len, int *outlen)
{
    if (len < 0) len = s ? (int)strlen(s) : 0;
    ub b; ub_init(&b);
    for (int i = 0; i < len; i++) {
        if (s[i] == '%' && i + 2 < len && is_hex((unsigned char)s[i + 1]) && is_hex((unsigned char)s[i + 2])) {
            ub_putc(&b, (hexval((unsigned char)s[i + 1]) << 4) | hexval((unsigned char)s[i + 2]));
            i += 2;
        } else ub_putc(&b, (unsigned char)s[i]);
    }
    if (outlen) *outlen = b.n;
    return ub_take(&b);
}

/* ===================================================================== *
 *  special schemes
 * ===================================================================== */

static int default_port(const char *scheme)
{
    if (!strcmp(scheme, "http") || !strcmp(scheme, "ws")) return 80;
    if (!strcmp(scheme, "https") || !strcmp(scheme, "wss")) return 443;
    if (!strcmp(scheme, "ftp")) return 21;
    return -1;
}

static int scheme_is_special(const char *s)
{
    return !strcmp(s, "ftp") || !strcmp(s, "file") || !strcmp(s, "http") ||
           !strcmp(s, "https") || !strcmp(s, "ws") || !strcmp(s, "wss");
}

/* ===================================================================== *
 *  the URL record
 * ===================================================================== */

struct urlrec {
    char  *scheme;      /* never NULL, no trailing ':', lowercase */
    char  *username;    /* never NULL */
    char  *password;    /* never NULL */
    char  *host;        /* NULL = null host. Already SERIALIZED: an IPv6 host
                         * is stored bracketed, an IPv4 host dotted-decimal --
                         * so the getter is a copy and there is one place where
                         * a host's textual form is decided. */
    int    port;        /* -1 = null */
    int    opaque;      /* 1 = the path is a single opaque string */
    char  *opath;       /* opaque path, when opaque */
    char **path;        /* list of segments, when not opaque */
    int    npath, cpath;
    char  *query;       /* NULL = null */
    char  *fragment;    /* NULL = null */
};

static void setstr(char **dst, const char *s)
{
    char *n = s ? xstrdup(s) : 0;
    free(*dst);
    *dst = n;
}

static void setstrn(char **dst, const char *s, int n)
{
    char *v = s ? xstrndup(s, n) : 0;
    free(*dst);
    *dst = v;
}

static void path_clear(urlrec *u)
{
    for (int i = 0; i < u->npath; i++) free(u->path[i]);
    u->npath = 0;
}

static void path_push(urlrec *u, const char *s, int n)
{
    if (u->npath + 1 > u->cpath) {
        int nc = u->cpath ? u->cpath * 2 : 8;
        char **np = (char **)xalloc((int)sizeof(char *) * nc);
        if (!np) return;
        for (int i = 0; i < u->npath; i++) np[i] = u->path[i];
        free(u->path);
        u->path = np; u->cpath = nc;
    }
    u->path[u->npath++] = xstrndup(s ? s : "", n);
}

static urlrec *url_new(void)
{
    urlrec *u = (urlrec *)xalloc((int)sizeof(urlrec));
    if (!u) return 0;
    memset(u, 0, sizeof(*u));
    u->scheme = xstrdup("");
    u->username = xstrdup("");
    u->password = xstrdup("");
    u->port = -1;
    return u;
}

void url_free_w(urlrec *u)
{
    if (!u) return;
    free(u->scheme); free(u->username); free(u->password); free(u->host);
    free(u->opath); free(u->query); free(u->fragment);
    path_clear(u); free(u->path);
    free(u);
}

urlrec *url_clone_w(const urlrec *u)
{
    if (!u) return 0;
    urlrec *n = url_new();
    if (!n) return 0;
    setstr(&n->scheme, u->scheme);
    setstr(&n->username, u->username);
    setstr(&n->password, u->password);
    setstr(&n->host, u->host);
    n->port = u->port;
    n->opaque = u->opaque;
    setstr(&n->opath, u->opath);
    for (int i = 0; i < u->npath; i++) path_push(n, u->path[i], (int)strlen(u->path[i]));
    setstr(&n->query, u->query);
    setstr(&n->fragment, u->fragment);
    return n;
}

static int url_is_special(const urlrec *u) { return scheme_is_special(u->scheme); }
static int url_has_creds(const urlrec *u) { return u->username[0] || u->password[0]; }

/* "cannot have a username/password/port" */
static int url_no_creds_slot(const urlrec *u)
{
    return u->host == 0 || u->host[0] == 0 || !strcmp(u->scheme, "file");
}

/* ===================================================================== *
 *  Windows drive letters and the dot segments
 * ===================================================================== */

static int is_win_drive(const char *s, int n)
{
    return n == 2 && is_alpha((unsigned char)s[0]) && (s[1] == ':' || s[1] == '|');
}
static int is_norm_win_drive(const char *s, int n)
{
    return n == 2 && is_alpha((unsigned char)s[0]) && s[1] == ':';
}
static int starts_win_drive(const char *s, int n)
{
    if (n < 2) return 0;
    if (!is_alpha((unsigned char)s[0])) return 0;
    if (s[1] != ':' && s[1] != '|') return 0;
    return n == 2 || s[2] == '/' || s[2] == '\\' || s[2] == '?' || s[2] == '#';
}

static int eq_ci_n(const char *a, int an, const char *b)
{
    int bn = (int)strlen(b);
    if (an != bn) return 0;
    for (int i = 0; i < an; i++)
        if (ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i])) return 0;
    return 1;
}

static int is_single_dot(const char *s, int n)
{
    return eq_ci_n(s, n, ".") || eq_ci_n(s, n, "%2e");
}
static int is_double_dot(const char *s, int n)
{
    return eq_ci_n(s, n, "..") || eq_ci_n(s, n, ".%2e") ||
           eq_ci_n(s, n, "%2e.") || eq_ci_n(s, n, "%2e%2e");
}

static void shorten_path(urlrec *u)
{
    if (u->npath == 0) return;
    if (!strcmp(u->scheme, "file") && u->npath == 1 &&
        is_norm_win_drive(u->path[0], (int)strlen(u->path[0]))) return;
    free(u->path[--u->npath]);
}

/* ===================================================================== *
 *  IDNA
 * ---------------------------------------------------------------------
 * WHERE THIS STOPS, SAID EXACTLY.  Complete: punycode (RFC 3492) in both
 * directions, so a Unicode label becomes xn-- and an xn-- label is decoded and
 * re-encoded (which is how a mixed-case or non-canonical xn-- label is caught).
 * Complete: the UTS-46 status classes for the ranges that carry syntax --
 * ASCII case folding, the three non-ASCII full stops (U+3002, U+FF0E, U+FF61),
 * the halfwidth/fullwidth ASCII block U+FF01..U+FF5E, the ignorable format
 * characters (soft hyphen, ZWSP/ZWNJ/ZWJ, the bidi controls, the variation
 * selectors, BOM), and the disallowed classes (surrogate/U+FFFD, C0/C1,
 * space, the forbidden domain code points, non-characters).
 *
 * ABSENT, and each of these is a real gap rather than an approximation:
 *   - NFC normalization. A decomposed label is not composed before punycode,
 *     so it encodes to a different xn-- label than a browser would produce.
 *   - The full IdnaMappingTable. Code points outside the ranges above are
 *     treated as VALID rather than looked up, so a mapped or deviation code
 *     point outside those ranges passes through unmapped and a disallowed one
 *     is accepted. Closing this is a generated table from upstream
 *     IdnaMappingTable.txt -- the house pattern (tools/gen_encoding_tables.py)
 *     -- and it is not here.
 *   - CheckBidi and CheckJoiners.
 * The consequence is bounded and is measured, not asserted: url/IdnaTestV2 is
 * reported separately by the host test for exactly this reason.
 * ===================================================================== */

#define PUNY_BASE 36
#define PUNY_TMIN 1
#define PUNY_TMAX 26
#define PUNY_SKEW 38
#define PUNY_DAMP 700
#define PUNY_INIT_BIAS 72
#define PUNY_INIT_N 128

static int puny_adapt(int delta, int numpoints, int firsttime)
{
    delta = firsttime ? delta / PUNY_DAMP : delta / 2;
    delta += delta / numpoints;
    int k = 0;
    while (delta > ((PUNY_BASE - PUNY_TMIN) * PUNY_TMAX) / 2) {
        delta /= PUNY_BASE - PUNY_TMIN;
        k += PUNY_BASE;
    }
    return k + (PUNY_BASE - PUNY_TMIN + 1) * delta / (delta + PUNY_SKEW);
}

/* Encode a code-point array as punycode (without the "xn--" prefix). */
static char *puny_encode(const unsigned *cp, int n)
{
    ub out; ub_init(&out);
    int b = 0;
    for (int i = 0; i < n; i++) if (cp[i] < 0x80) { ub_putc(&out, (int)cp[i]); b++; }
    int h = b;
    if (b > 0) ub_putc(&out, '-');
    unsigned nn = PUNY_INIT_N;
    int delta = 0, bias = PUNY_INIT_BIAS;
    while (h < n) {
        unsigned m = 0x7FFFFFFF;
        for (int i = 0; i < n; i++) if (cp[i] >= nn && cp[i] < m) m = cp[i];
        if ((long)delta + (long)(m - nn) * (long)(h + 1) > 0x7FFFFFFF) { ub_free(&out); return 0; }
        delta += (int)((m - nn) * (unsigned)(h + 1));
        nn = m;
        for (int i = 0; i < n; i++) {
            if (cp[i] < nn) delta++;
            else if (cp[i] == nn) {
                int q = delta;
                for (int k = PUNY_BASE;; k += PUNY_BASE) {
                    int t = k <= bias ? PUNY_TMIN : (k >= bias + PUNY_TMAX ? PUNY_TMAX : k - bias);
                    if (q < t) break;
                    int d = t + (q - t) % (PUNY_BASE - t);
                    ub_putc(&out, d < 26 ? 'a' + d : '0' + d - 26);
                    q = (q - t) / (PUNY_BASE - t);
                }
                ub_putc(&out, q < 26 ? 'a' + q : '0' + q - 26);
                bias = puny_adapt(delta, h + 1, h == b);
                delta = 0;
                h++;
            }
        }
        delta++; nn++;
    }
    return ub_take(&out);
}

/* Decode punycode (input WITHOUT the "xn--" prefix) into a code-point array.
 * Returns the count, or -1 on failure. */
static int puny_decode(const char *s, int n, unsigned *out, int outmax)
{
    int len = 0, b = 0;
    for (int i = n - 1; i >= 0; i--) if (s[i] == '-') { b = i; break; }
    for (int i = 0; i < b; i++) {
        if ((unsigned char)s[i] >= 0x80) return -1;
        if (len >= outmax) return -1;
        out[len++] = (unsigned char)s[i];
    }
    int in = b > 0 ? b + 1 : 0;
    unsigned nn = PUNY_INIT_N;
    int i2 = 0, bias = PUNY_INIT_BIAS;
    while (in < n) {
        int oldi = i2, w = 1;
        for (int k = PUNY_BASE;; k += PUNY_BASE) {
            if (in >= n) return -1;
            int c = (unsigned char)s[in++], d;
            if (c >= 'a' && c <= 'z') d = c - 'a';
            else if (c >= 'A' && c <= 'Z') d = c - 'A';
            else if (c >= '0' && c <= '9') d = c - '0' + 26;
            else return -1;
            if (d > (0x7FFFFFFF - i2) / w) return -1;
            i2 += d * w;
            int t = k <= bias ? PUNY_TMIN : (k >= bias + PUNY_TMAX ? PUNY_TMAX : k - bias);
            if (d < t) break;
            if (w > 0x7FFFFFFF / (PUNY_BASE - t)) return -1;
            w *= PUNY_BASE - t;
        }
        bias = puny_adapt(i2 - oldi, len + 1, oldi == 0);
        if (i2 / (len + 1) > (int)(0x7FFFFFFF - nn)) return -1;
        nn += (unsigned)(i2 / (len + 1));
        i2 %= (len + 1);
        if (nn > 0x10FFFF || (nn >= 0xD800 && nn <= 0xDFFF)) return -1;
        if (len >= outmax) return -1;
        for (int k = len; k > i2; k--) out[k] = out[k - 1];
        out[i2++] = nn;
        len++;
    }
    return len;
}

/* forbidden host / forbidden domain code points */
static int forbidden_host_cp(unsigned c)
{
    return c == 0x00 || c == 0x09 || c == 0x0A || c == 0x0D || c == 0x20 ||
           c == '#' || c == '/' || c == ':' || c == '<' || c == '>' ||
           c == '?' || c == '@' || c == '[' || c == '\\' || c == ']' ||
           c == '^' || c == '|';
}
static int forbidden_domain_cp(unsigned c)
{
    return forbidden_host_cp(c) || c <= 0x1F || c == '%' || c == 0x7F;
}

/* UTS-46 status for the classes this file knows. Returns:
 *   0 valid (keep as-is)         1 ignored (drop)
 *   2 mapped (to *m)             3 disallowed */
static int uts46_status(unsigned c, unsigned *m)
{
    if (c >= 'A' && c <= 'Z') { *m = c + 32; return 2; }
    if (c < 0x80) {
        /* ASCII: everything else is valid here; the forbidden-domain check
         * downstream is what rejects the syntactic ones. */
        return 0;
    }
    /* the three non-ASCII full stops -- label separators after mapping */
    if (c == 0x3002 || c == 0xFF0E || c == 0xFF61) { *m = '.'; return 2; }
    /* halfwidth and fullwidth forms of ASCII. UTS-46 maps these through
     * NFKC_Casefold, so FULLWIDTH LATIN CAPITAL LETTER G lands on 'g' and not
     * on 'G' -- "http://Ｇｏ.com" is "http://go.com/". */
    if (c >= 0xFF01 && c <= 0xFF5E) {
        unsigned a = c - 0xFEE0;
        *m = (a >= 'A' && a <= 'Z') ? a + 32 : a;
        return 2;
    }
    /* Mathematical Alphanumeric Symbols. NFKC_Casefold folds every style in
     * U+1D400..U+1D6A3 onto a lowercase ASCII letter, in runs of 52 (26 caps
     * then 26 lowercase), and U+1D7CE..U+1D7FF onto a digit. This is a REAL
     * range of the mapping table done by arithmetic rather than by lookup, and
     * the arithmetic is exact for the assigned code points; the handful of
     * reserved holes in the block (where a Letterlike Symbol stands in) get a
     * letter here where a full table would call them disallowed. It is in
     * because a corpus case turns on it: "file://loC𝐀𝐋𝐇𝐨𝐬𝐭/usr/bin" is
     * "file:///usr/bin", localhost having been spelled in bold. */
    if (c >= 0x1D400 && c <= 0x1D6A3) { unsigned k = (c - 0x1D400) % 52; *m = 'a' + (k < 26 ? k : k - 26); return 2; }
    if (c >= 0x1D7CE && c <= 0x1D7FF) { *m = '0' + (c - 0x1D7CE) % 10; return 2; }
    /* ignorable format characters */
    if (c == 0x00AD || c == 0x200B || c == 0x200C || c == 0x200D ||
        c == 0x2060 || c == 0xFEFF || c == 0x180E ||
        (c >= 0x200E && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) ||
        (c >= 0x2066 && c <= 0x2069) || (c >= 0xFE00 && c <= 0xFE0F))
        return 1;
    /* disallowed: C1 controls, the space separators, U+FFFD, non-characters,
     * the private-use-adjacent specials block, surrogates (unreachable -- the
     * decoder already turned those into U+FFFD). */
    if (c >= 0x80 && c <= 0x9F) return 3;
    if (c == 0x00A0 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
        c == 0x202F || c == 0x205F || c == 0x3000 || c == 0x1680) return 3;
    if (c == 0xFFFD || (c >= 0xFFF9 && c <= 0xFFFC)) return 3;
    if ((c & 0xFFFE) == 0xFFFE) return 3;
    if (c >= 0xFDD0 && c <= 0xFDEF) return 3;
    if (c >= 0xD800 && c <= 0xDFFF) return 3;
    return 0;
}

/* domain to ASCII. Returns a malloc'd lowercase ASCII domain, or NULL.
 * *outlen carries the LENGTH, because the result can contain a U+0000 -- a
 * percent-decoded "%00" reaches here and the forbidden-domain check is what
 * must reject it. Reading the result with strlen instead silently truncates
 * and turns "http://hello%00" from a failure into "http://hello/". */
static char *domain_to_ascii(const char *s, int len, int *outlen)
{
    /* 1. map */
    unsigned *cps = (unsigned *)xalloc((int)sizeof(unsigned) * (len + 1));
    if (!cps) return 0;
    int ncp = 0;
    {
        const unsigned char *u = (const unsigned char *)s;
        int i = 0;
        while (i < len) {
            unsigned c = utf8_next(u, len, &i), m = 0;
            int st = uts46_status(c, &m);
            if (st == 3) { free(cps); return 0; }
            if (st == 1) continue;
            cps[ncp++] = (st == 2) ? m : c;
        }
    }

    /* 2. label-by-label punycode */
    ub out; ub_init(&out);
    int start = 0;
    for (int i = 0; i <= ncp; i++) {
        if (i != ncp && cps[i] != '.') continue;
        int n = i - start;
        int nonascii = 0;
        for (int k = 0; k < n; k++) if (cps[start + k] >= 0x80) nonascii = 1;
        if (nonascii) {
            char *enc = puny_encode(cps + start, n);
            if (!enc) { free(cps); ub_free(&out); return 0; }
            ub_puts(&out, "xn--");
            ub_puts(&out, enc);
            free(enc);
        } else {
            /* An xn-- label must decode, and must decode to something that
             * re-encodes to itself -- that is the check that rejects
             * "xn--a", "xn--0", and a label whose punycode is non-canonical.
             * Without it every malformed xn-- label would be accepted
             * verbatim, which is one of the ways a "close enough" parser
             * looks right and is not. */
            if (n > 4 && cps[start] == 'x' && cps[start + 1] == 'n' &&
                cps[start + 2] == '-' && cps[start + 3] == '-') {
                char tmp[256];
                if (n - 4 >= (int)sizeof(tmp)) { free(cps); ub_free(&out); return 0; }
                for (int k = 0; k < n - 4; k++) tmp[k] = (char)cps[start + 4 + k];
                unsigned dec[256];
                int dn = puny_decode(tmp, n - 4, dec, 256);
                if (dn <= 0) { free(cps); ub_free(&out); return 0; }
                for (int k = 0; k < dn; k++) {
                    unsigned m = 0;
                    if (uts46_status(dec[k], &m) == 3) { free(cps); ub_free(&out); return 0; }
                }
                char *re = puny_encode(dec, dn);
                if (!re) { free(cps); ub_free(&out); return 0; }
                ub_puts(&out, "xn--");
                ub_puts(&out, re);
                free(re);
            } else {
                for (int k = 0; k < n; k++) ub_putc(&out, (int)cps[start + k]);
            }
        }
        if (i != ncp) ub_putc(&out, '.');
        start = i + 1;
    }
    free(cps);
    if (out.n == 0) { ub_free(&out); return 0; }   /* empty result is failure */
    if (outlen) *outlen = out.n;
    return ub_take(&out);
}

/* ===================================================================== *
 *  host parsing
 * ===================================================================== */

/* IPv4 number parser. Returns 0 on success and writes *out; -1 on failure.
 * Overflow saturates to a value the range checks below will reject rather
 * than wrapping -- "http://10000000000000000000/" must fail, not alias. */
static int ipv4_number(const char *s, int n, uint64_t *out)
{
    int R = 10;
    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { R = 16; s += 2; n -= 2; }
    else if (n >= 2 && s[0] == '0') { R = 8; s += 1; n -= 1; }
    if (n == 0) { *out = 0; return 0; }
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        int d = hexval((unsigned char)s[i]);
        if (d < 0 || d >= R) return -1;
        if (v > 0xFFFFFFFFFFULL) v = 0xFFFFFFFFFFULL;    /* saturate; still fails below */
        v = v * (uint64_t)R + (uint64_t)d;
    }
    *out = v;
    return 0;
}

static int ends_in_number(const char *s, int n)
{
    /* split on '.'; drop one trailing empty part */
    int last = n;
    if (n > 0 && s[n - 1] == '.') {
        /* a single "." is parts ["", ""] -> after removing the last, [""] */
        n--; last = n;
    }
    int start = 0;
    for (int i = n - 1; i >= 0; i--) if (s[i] == '.') { start = i + 1; break; }
    int ln = last - start;
    if (ln <= 0) return 0;
    int alldig = 1;
    for (int i = 0; i < ln; i++) if (!is_digit((unsigned char)s[start + i])) { alldig = 0; break; }
    if (alldig) return 1;
    uint64_t v;
    return ipv4_number(s + start, ln, &v) == 0;
}

/* IPv4 parse -> serialized dotted-decimal, or NULL. */
static char *ipv4_parse(const char *s, int n)
{
    const char *parts[8]; int plen[8], np = 0;
    int start = 0;
    for (int i = 0; i <= n; i++) {
        if (i == n || s[i] == '.') {
            if (np >= 8) return 0;
            parts[np] = s + start; plen[np] = i - start; np++;
            start = i + 1;
        }
    }
    if (np > 1 && plen[np - 1] == 0) np--;         /* trailing "." */
    if (np > 4 || np == 0) return 0;
    uint64_t num[4];
    for (int i = 0; i < np; i++) {
        if (plen[i] == 0) return 0;
        if (ipv4_number(parts[i], plen[i], &num[i]) != 0) return 0;
    }
    for (int i = 0; i < np - 1; i++) if (num[i] > 255) return 0;
    uint64_t limit = 1;
    for (int i = 0; i < 5 - np; i++) limit *= 256;
    if (num[np - 1] >= limit) return 0;
    uint64_t v = num[np - 1];
    for (int i = 0; i < np - 1; i++) {
        uint64_t mul = 1;
        for (int k = 0; k < 3 - i; k++) mul *= 256;
        v += num[i] * mul;
    }
    ub b; ub_init(&b);
    for (int i = 3; i >= 0; i--) {
        ub_putu(&b, (unsigned long)((v >> (i * 8)) & 0xFF));
        if (i) ub_putc(&b, '.');
    }
    return ub_take(&b);
}

/* IPv6 parse -> "[serialized]", or NULL. Input excludes the brackets. */
static char *ipv6_parse(const char *s, int n)
{
    unsigned addr[8];
    for (int i = 0; i < 8; i++) addr[i] = 0;
    int piece = 0, compress = -1, p = 0;

#define C6 (p < n ? (unsigned char)s[p] : -1)
    if (C6 == ':') {
        if (!(p + 1 < n && s[p + 1] == ':')) return 0;
        p += 2; piece++; compress = piece;
    }
    while (C6 != -1) {
        if (piece == 8) return 0;
        if (C6 == ':') {
            if (compress >= 0) return 0;
            p++; piece++; compress = piece;
            continue;
        }
        unsigned value = 0; int length = 0;
        while (length < 4 && is_hex(C6)) { value = value * 16 + (unsigned)hexval(C6); p++; length++; }
        if (C6 == '.') {
            if (length == 0) return 0;
            p -= length;
            if (piece > 6) return 0;
            int seen = 0;
            while (C6 != -1) {
                int ipv4piece = -1;
                if (seen > 0) {
                    if (C6 == '.' && seen < 4) p++;
                    else return 0;
                }
                if (!is_digit(C6)) return 0;
                while (is_digit(C6)) {
                    int num = C6 - '0';
                    if (ipv4piece < 0) ipv4piece = num;
                    else if (ipv4piece == 0) return 0;
                    else ipv4piece = ipv4piece * 10 + num;
                    if (ipv4piece > 255) return 0;
                    p++;
                }
                addr[piece] = addr[piece] * 256 + (unsigned)ipv4piece;
                seen++;
                if (seen == 2 || seen == 4) piece++;
            }
            if (seen != 4) return 0;
            break;
        } else if (C6 == ':') {
            p++;
            if (C6 == -1) return 0;
        } else if (C6 != -1) return 0;
        addr[piece++] = value;
    }
#undef C6
    if (compress >= 0) {
        int swaps = piece - compress;
        piece = 7;
        while (piece != 0 && swaps > 0) {
            unsigned t = addr[piece];
            addr[piece] = addr[compress + swaps - 1];
            addr[compress + swaps - 1] = t;
            piece--; swaps--;
        }
    } else if (piece != 8) return 0;

    /* serialize: compress the FIRST longest run of more than one zero */
    int best = -1, bestlen = 1, run = 0, runstart = 0;
    for (int i = 0; i < 8; i++) {
        if (addr[i] == 0) {
            if (run == 0) runstart = i;
            run++;
            if (run > bestlen) { bestlen = run; best = runstart; }
        } else run = 0;
    }
    ub b; ub_init(&b);
    ub_putc(&b, '[');
    int ignore0 = 0;
    for (int i = 0; i < 8; i++) {
        if (ignore0 && addr[i] == 0) continue;
        else if (ignore0) ignore0 = 0;
        if (best == i) { ub_puts(&b, i == 0 ? "::" : ":"); ignore0 = 1; continue; }
        static const char *H = "0123456789abcdef";
        unsigned v = addr[i];
        char t[4]; int k = 0;
        if (!v) t[k++] = '0';
        while (v) { t[k++] = H[v & 15]; v >>= 4; }
        while (k) ub_putc(&b, t[--k]);
        if (i != 7) ub_putc(&b, ':');
    }
    ub_putc(&b, ']');
    return ub_take(&b);
}

char *url_host_parse(const char *s, int len, int opaque)
{
    if (len < 0) len = s ? (int)strlen(s) : 0;
    if (len > 0 && s[0] == '[') {
        if (s[len - 1] != ']') return 0;
        return ipv6_parse(s + 1, len - 2);
    }
    if (opaque) {
        const unsigned char *u = (const unsigned char *)s;
        int i = 0;
        while (i < len) {
            unsigned c = utf8_next(u, len, &i);
            if (forbidden_host_cp(c)) return 0;
            if (c == '%' ) {
                /* a '%' not followed by two hex digits is only a validation
                 * error here, not a failure -- opaque hosts keep it */
            }
        }
        ub b; ub_init(&b);
        ub_put_pct(&b, s, len, PCT_C0);
        return ub_take(&b);
    }
    if (len == 0) return 0;
    int dl = 0;
    char *dec = url_percent_decode(s, len, &dl);
    if (!dec) return 0;
    int sl = 0;
    char *san = utf8_sanitize(dec, dl, &sl);
    free(dec);
    if (!san) return 0;
    int an = 0;
    char *ascii = domain_to_ascii(san, sl, &an);
    free(san);
    if (!ascii) return 0;
    for (int i = 0; i < an; i++)
        if (forbidden_domain_cp((unsigned char)ascii[i])) { free(ascii); return 0; }
    if (ends_in_number(ascii, an)) {
        char *v4 = ipv4_parse(ascii, an);
        free(ascii);
        return v4;                     /* NULL propagates as failure */
    }
    return ascii;
}

/* ===================================================================== *
 *  the basic URL parser
 * ===================================================================== */

enum {
    ST_NONE = 0,
    ST_SCHEME_START, ST_SCHEME, ST_NO_SCHEME,
    ST_SPECIAL_RELATIVE_OR_AUTHORITY, ST_PATH_OR_AUTHORITY,
    ST_RELATIVE, ST_RELATIVE_SLASH,
    ST_SPECIAL_AUTHORITY_SLASHES, ST_SPECIAL_AUTHORITY_IGNORE_SLASHES,
    ST_AUTHORITY, ST_HOST, ST_HOSTNAME, ST_PORT,
    ST_FILE, ST_FILE_SLASH, ST_FILE_HOST,
    ST_PATH_START, ST_PATH, ST_OPAQUE_PATH, ST_QUERY, ST_FRAGMENT
};

static void copy_authority_from(urlrec *u, const urlrec *base)
{
    setstr(&u->username, base->username);
    setstr(&u->password, base->password);
    setstr(&u->host, base->host);
    u->port = base->port;
}

static void copy_path_from(urlrec *u, const urlrec *base)
{
    u->opaque = base->opaque;
    setstr(&u->opath, base->opath);
    path_clear(u);
    for (int i = 0; i < base->npath; i++)
        path_push(u, base->path[i], (int)strlen(base->path[i]));
}

/* "potentially strip trailing spaces from an opaque path" */
static void strip_opaque_trailing_spaces(urlrec *u)
{
    if (!u->opaque || u->fragment || u->query || !u->opath) return;
    int n = (int)strlen(u->opath);
    while (n > 0 && u->opath[n - 1] == ' ') n--;
    u->opath[n] = 0;
}

#ifndef URL_NAIVE_SPLIT

static int basic_parse(const char *s, int n, const urlrec *base,
                       urlrec *u, int state_override)
{
    int state = state_override ? state_override : ST_SCHEME_START;
    ub buf; ub_init(&buf);
    int at_sign_seen = 0, inside_brackets = 0, password_token_seen = 0;
    int p = 0;
    int rc = 0;

#define FAIL()   do { rc = -1; goto done; } while (0)
#define RET()    do { rc = 0;  goto done; } while (0)
#define SPECIAL  url_is_special(u)
#define REM_IS(str) (n - p - 1 >= (int)sizeof(str) - 1 && \
                     memcmp(s + p + 1, (str), sizeof(str) - 1) == 0)

    for (;;) {
        int c = (p >= 0 && p < n) ? (unsigned char)s[p] : -1;

        switch (state) {

        case ST_SCHEME_START:
            if (c >= 0 && is_alpha(c)) { ub_putc(&buf, ascii_lower(c)); state = ST_SCHEME; }
            else if (!state_override) { state = ST_NO_SCHEME; p--; }
            else FAIL();
            break;

        case ST_SCHEME:
            if (c >= 0 && (is_alnum(c) || c == '+' || c == '-' || c == '.'))
                ub_putc(&buf, ascii_lower(c));
            else if (c == ':') {
                if (state_override) {
                    int old_special = SPECIAL;
                    int new_special = scheme_is_special(buf.p ? buf.p : "");
                    if (old_special != new_special) RET();
                    if ((url_has_creds(u) || u->port >= 0) && !strcmp(buf.p ? buf.p : "", "file")) RET();
                    if (!strcmp(u->scheme, "file") && u->host && u->host[0] == 0) RET();
                }
                setstr(&u->scheme, buf.p ? buf.p : "");
                if (state_override) {
                    if (u->port >= 0 && u->port == default_port(u->scheme)) u->port = -1;
                    RET();
                }
                ub_clear(&buf);
                if (!strcmp(u->scheme, "file")) {
                    state = ST_FILE;
                } else if (SPECIAL && base && !strcmp(base->scheme, u->scheme)) {
                    state = ST_SPECIAL_RELATIVE_OR_AUTHORITY;
                } else if (SPECIAL) {
                    state = ST_SPECIAL_AUTHORITY_SLASHES;
                } else if (REM_IS("/")) {
                    state = ST_PATH_OR_AUTHORITY; p++;
                } else {
                    u->opaque = 1;
                    setstr(&u->opath, "");
                    state = ST_OPAQUE_PATH;
                }
            } else if (!state_override) {
                ub_clear(&buf);
                state = ST_NO_SCHEME;
                p = -1;
            } else FAIL();
            break;

        case ST_NO_SCHEME:
            if (!base || (base->opaque && c != '#')) FAIL();
            else if (base->opaque && c == '#') {
                setstr(&u->scheme, base->scheme);
                u->opaque = 1;
                setstr(&u->opath, base->opath);
                setstr(&u->query, base->query);
                setstr(&u->fragment, "");
                state = ST_FRAGMENT;
            } else if (strcmp(base->scheme, "file")) { state = ST_RELATIVE; p--; }
            else { state = ST_FILE; p--; }
            break;

        case ST_SPECIAL_RELATIVE_OR_AUTHORITY:
            if (c == '/' && REM_IS("/")) { state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES; p++; }
            else { state = ST_RELATIVE; p--; }
            break;

        case ST_PATH_OR_AUTHORITY:
            if (c == '/') state = ST_AUTHORITY;
            else { state = ST_PATH; p--; }
            break;

        case ST_RELATIVE:
            setstr(&u->scheme, base->scheme);
            if (c == '/' || (SPECIAL && c == '\\')) state = ST_RELATIVE_SLASH;
            else {
                copy_authority_from(u, base);
                copy_path_from(u, base);
                setstr(&u->query, base->query);
                if (c == '?') { setstr(&u->query, ""); state = ST_QUERY; }
                else if (c == '#') { setstr(&u->fragment, ""); state = ST_FRAGMENT; }
                else if (c != -1) {
                    setstr(&u->query, 0);
                    shorten_path(u);
                    state = ST_PATH; p--;
                }
            }
            break;

        case ST_RELATIVE_SLASH:
            if (SPECIAL && (c == '/' || c == '\\')) state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
            else if (c == '/') state = ST_AUTHORITY;
            else { copy_authority_from(u, base); state = ST_PATH; p--; }
            break;

        case ST_SPECIAL_AUTHORITY_SLASHES:
            if (c == '/' && REM_IS("/")) { state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES; p++; }
            else { state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES; p--; }
            break;

        case ST_SPECIAL_AUTHORITY_IGNORE_SLASHES:
            if (c != '/' && c != '\\') { state = ST_AUTHORITY; p--; }
            break;

        case ST_AUTHORITY:
            if (c == '@') {
                if (at_sign_seen) {
                    /* prepend "%40" to the buffer */
                    ub t; ub_init(&t);
                    ub_puts(&t, "%40");
                    ub_put(&t, buf.p ? buf.p : "", buf.n);
                    ub_free(&buf);
                    buf = t;
                }
                at_sign_seen = 1;
                {
                    ub un; ub_init(&un);
                    ub pw; ub_init(&pw);
                    ub_puts(&un, u->username);
                    ub_puts(&pw, u->password);
                    for (int i = 0; i < buf.n; i++) {
                        unsigned char ch = (unsigned char)buf.p[i];
                        if (ch == ':' && !password_token_seen) { password_token_seen = 1; continue; }
                        ub *dst = password_token_seen ? &pw : &un;
                        if (in_set(PCT_USERINFO, ch)) ub_pct(dst, ch); else ub_putc(dst, ch);
                    }
                    free(u->username); u->username = ub_take(&un);
                    free(u->password); u->password = ub_take(&pw);
                }
                ub_clear(&buf);
            } else if (c == -1 || c == '/' || c == '?' || c == '#' || (SPECIAL && c == '\\')) {
                if (at_sign_seen && buf.n == 0) FAIL();
                p -= buf.n + 1;
                ub_clear(&buf);
                state = ST_HOST;
            } else ub_putc(&buf, c);
            break;

        case ST_HOST:
        case ST_HOSTNAME:
            if (state_override && !strcmp(u->scheme, "file")) { p--; state = ST_FILE_HOST; }
            else if (c == ':' && !inside_brackets) {
                if (buf.n == 0) FAIL();
                if (state_override == ST_HOSTNAME) RET();
                {
                    char *h = url_host_parse(buf.p ? buf.p : "", buf.n, !SPECIAL);
                    if (!h) FAIL();
                    free(u->host); u->host = h;
                }
                ub_clear(&buf);
                state = ST_PORT;
            } else if (c == -1 || c == '/' || c == '?' || c == '#' || (SPECIAL && c == '\\')) {
                p--;
                if (SPECIAL && buf.n == 0) FAIL();
                if (state_override && buf.n == 0 && (url_has_creds(u) || u->port >= 0)) RET();
                {
                    char *h = url_host_parse(buf.p ? buf.p : "", buf.n, !SPECIAL);
                    if (!h) FAIL();
                    free(u->host); u->host = h;
                }
                ub_clear(&buf);
                state = ST_PATH_START;
                if (state_override) RET();
            } else {
                if (c == '[') inside_brackets = 1;
                if (c == ']') inside_brackets = 0;
                ub_putc(&buf, c);
            }
            break;

        case ST_PORT:
            if (c >= 0 && is_digit(c)) ub_putc(&buf, c);
            else if (c == -1 || c == '/' || c == '?' || c == '#' || (SPECIAL && c == '\\') || state_override) {
                if (buf.n > 0) {
                    long v = 0;
                    for (int i = 0; i < buf.n; i++) {
                        v = v * 10 + (buf.p[i] - '0');
                        if (v > 65535) FAIL();
                    }
                    u->port = (v == default_port(u->scheme)) ? -1 : (int)v;
                    ub_clear(&buf);
                }
                if (state_override) RET();
                state = ST_PATH_START;
                p--;
            } else FAIL();
            break;

        case ST_FILE:
            setstr(&u->scheme, "file");
            setstr(&u->host, "");
            if (c == '/' || c == '\\') state = ST_FILE_SLASH;
            else if (base && !strcmp(base->scheme, "file")) {
                setstr(&u->host, base->host);
                copy_path_from(u, base);
                setstr(&u->query, base->query);
                if (c == '?') { setstr(&u->query, ""); state = ST_QUERY; }
                else if (c == '#') { setstr(&u->fragment, ""); state = ST_FRAGMENT; }
                else if (c != -1) {
                    setstr(&u->query, 0);
                    if (!starts_win_drive(s + p, n - p)) shorten_path(u);
                    else path_clear(u);
                    state = ST_PATH; p--;
                }
            } else { state = ST_PATH; p--; }
            break;

        case ST_FILE_SLASH:
            if (c == '/' || c == '\\') state = ST_FILE_HOST;
            else {
                if (base && !strcmp(base->scheme, "file")) {
                    setstr(&u->host, base->host);
                    if (!starts_win_drive(s + (p < 0 ? 0 : p), n - (p < 0 ? 0 : p)) &&
                        base->npath > 0 &&
                        is_norm_win_drive(base->path[0], (int)strlen(base->path[0])))
                        path_push(u, base->path[0], (int)strlen(base->path[0]));
                }
                state = ST_PATH; p--;
            }
            break;

        case ST_FILE_HOST:
            if (c == -1 || c == '/' || c == '\\' || c == '?' || c == '#') {
                p--;
                if (!state_override && is_win_drive(buf.p ? buf.p : "", buf.n)) {
                    /* NOT a host: "file:c:/" is a path. The buffer is
                     * deliberately NOT cleared -- the path state re-reads it. */
                    state = ST_PATH;
                } else if (buf.n == 0) {
                    setstr(&u->host, "");
                    if (state_override) RET();
                    state = ST_PATH_START;
                } else {
                    char *h = url_host_parse(buf.p ? buf.p : "", buf.n, !SPECIAL);
                    if (!h) FAIL();
                    if (!strcmp(h, "localhost")) { free(h); h = xstrdup(""); }
                    free(u->host); u->host = h;
                    if (state_override) RET();
                    ub_clear(&buf);
                    state = ST_PATH_START;
                }
            } else ub_putc(&buf, c);
            break;

        case ST_PATH_START:
            if (SPECIAL) {
                state = ST_PATH;
                if (c != '/' && c != '\\') p--;
            } else if (!state_override && c == '?') { setstr(&u->query, ""); state = ST_QUERY; }
            else if (!state_override && c == '#') { setstr(&u->fragment, ""); state = ST_FRAGMENT; }
            else if (c != -1) {
                state = ST_PATH;
                if (c != '/') p--;
            } else if (state_override && u->host == 0) path_push(u, "", 0);
            break;

        case ST_PATH:
            if (c == -1 || c == '/' || (SPECIAL && c == '\\') ||
                (!state_override && (c == '?' || c == '#'))) {
                int dbl = is_double_dot(buf.p ? buf.p : "", buf.n);
                int sgl = is_single_dot(buf.p ? buf.p : "", buf.n);
                int slashish = (c == '/') || (SPECIAL && c == '\\');
                if (dbl) {
                    shorten_path(u);
                    if (!slashish) path_push(u, "", 0);
                } else if (sgl && !slashish) {
                    path_push(u, "", 0);
                } else if (!sgl) {
                    if (!strcmp(u->scheme, "file") && u->npath == 0 &&
                        is_win_drive(buf.p ? buf.p : "", buf.n))
                        buf.p[1] = ':';
                    path_push(u, buf.p ? buf.p : "", buf.n);
                }
                ub_clear(&buf);
                if (c == '?') { setstr(&u->query, ""); state = ST_QUERY; }
                if (c == '#') { setstr(&u->fragment, ""); state = ST_FRAGMENT; }
            } else {
                if (in_set(PCT_PATH, (unsigned char)c)) ub_pct(&buf, (unsigned char)c);
                else ub_putc(&buf, c);
            }
            break;

        case ST_OPAQUE_PATH:
            if (c == '?') { setstr(&u->query, ""); state = ST_QUERY; }
            else if (c == '#') { setstr(&u->fragment, ""); state = ST_FRAGMENT; }
            else if (c != -1) {
                ub t; ub_init(&t);
                ub_puts(&t, u->opath ? u->opath : "");
                /* A SPACE immediately before the query or fragment delimiter
                 * is encoded; the ones before it are not. That asymmetry is
                 * real and the corpus is precise about it:
                 *   "non-special:opaque  ?hi" -> path "opaque %20"
                 * A trailing space in an opaque path would otherwise vanish
                 * on the round trip, because the parser strips trailing
                 * spaces from the whole input. */
                if (c == ' ' && p + 1 < n && (s[p + 1] == '?' || s[p + 1] == '#'))
                    ub_pct(&t, ' ');
                else if (in_set(PCT_C0, (unsigned char)c)) ub_pct(&t, (unsigned char)c);
                else ub_putc(&t, c);
                free(u->opath); u->opath = ub_take(&t);
            }
            break;

        case ST_QUERY:
            if ((!state_override && c == '#') || c == -1) {
                int set = SPECIAL ? PCT_SPECIAL_QUERY : PCT_QUERY;
                ub t; ub_init(&t);
                ub_puts(&t, u->query ? u->query : "");
                ub_put_pct(&t, buf.p ? buf.p : "", buf.n, set);
                free(u->query); u->query = ub_take(&t);
                ub_clear(&buf);
                if (c == '#') { setstr(&u->fragment, ""); state = ST_FRAGMENT; }
            } else if (c != -1) ub_putc(&buf, c);
            break;

        case ST_FRAGMENT:
            if (c != -1) {
                ub t; ub_init(&t);
                ub_puts(&t, u->fragment ? u->fragment : "");
                if (in_set(PCT_FRAGMENT, (unsigned char)c)) ub_pct(&t, (unsigned char)c);
                else ub_putc(&t, c);
                free(u->fragment); u->fragment = ub_take(&t);
            }
            break;
        }

        if (p >= n) break;
        p++;
    }

done:
    ub_free(&buf);
    return rc;
#undef FAIL
#undef RET
#undef SPECIAL
#undef REM_IS
}

#else /* URL_NAIVE_SPLIT -- the negative control; see tests/url.mk */

/* The plausible-wrong parser: split the input on ':', '/', '?' and '#', keep
 * each piece verbatim, and let the serializer put them back together with the
 * same delimiters. It handles every URL a person would type, produces
 * correct-LOOKING output, and is wrong about normalization, default ports, dot
 * segments and every percent-encoding set -- which is exactly the failure
 * mode the corpus exists to catch. It is a build of the same shipping file,
 * not a stub: the record, the serializer, the setters and the bindings are all
 * the real ones. */
static int basic_parse(const char *s, int n, const urlrec *base,
                       urlrec *u, int state_override)
{
    /* setters: assign the field straight through */
    if (state_override) {
        switch (state_override) {
        case ST_SCHEME_START: {
            int e = n; while (e > 0 && s[e - 1] == ':') e--;
            setstrn(&u->scheme, s, e);
            for (int i = 0; u->scheme[i]; i++) u->scheme[i] = (char)ascii_lower((unsigned char)u->scheme[i]);
            return 0; }
        case ST_HOST: case ST_HOSTNAME: setstrn(&u->host, s, n); return 0;
        case ST_PORT: {
            long v = 0; for (int i = 0; i < n; i++) if (is_digit((unsigned char)s[i])) v = v * 10 + s[i] - '0';
            u->port = (int)v; return 0; }
        case ST_PATH_START: {
            path_clear(u);
            int st = 0;
            for (int i = 0; i <= n; i++)
                if (i == n || s[i] == '/') { if (!(i == 0)) path_push(u, s + st, i - st); st = i + 1; }
            return 0; }
        case ST_QUERY: setstrn(&u->query, s, n); return 0;
        case ST_FRAGMENT: setstrn(&u->fragment, s, n); return 0;
        }
        return 0;
    }

    /* the split */
    int i = 0, colon = -1;
    for (i = 0; i < n; i++) { if (s[i] == ':') { colon = i; break; } if (s[i] == '/' || s[i] == '?' || s[i] == '#') break; }
    int pos = 0;
    if (colon >= 0) { setstrn(&u->scheme, s, colon); pos = colon + 1; }
    else if (base) { setstr(&u->scheme, base->scheme); setstr(&u->host, base->host); u->port = base->port; }
    else return -1;
    for (int k = 0; u->scheme[k]; k++) u->scheme[k] = (char)ascii_lower((unsigned char)u->scheme[k]);

    if (pos + 1 < n && s[pos] == '/' && s[pos + 1] == '/') {
        pos += 2;
        int he = pos;
        while (he < n && s[he] != '/' && s[he] != '?' && s[he] != '#') he++;
        int at = -1;
        for (int k = pos; k < he; k++) if (s[k] == '@') at = k;
        if (at >= 0) {
            int c2 = -1;
            for (int k = pos; k < at; k++) if (s[k] == ':') { c2 = k; break; }
            if (c2 >= 0) { setstrn(&u->username, s, 0); setstrn(&u->username, s + pos, c2 - pos); setstrn(&u->password, s + c2 + 1, at - c2 - 1); }
            else setstrn(&u->username, s + pos, at - pos);
            pos = at + 1;
        }
        int pc = -1;
        for (int k = pos; k < he; k++) if (s[k] == ':') pc = k;
        if (pc >= 0) {
            setstrn(&u->host, s + pos, pc - pos);
            long v = 0; for (int k = pc + 1; k < he; k++) if (is_digit((unsigned char)s[k])) v = v * 10 + s[k] - '0';
            u->port = (int)v;
        } else setstrn(&u->host, s + pos, he - pos);
        pos = he;
    } else if (colon >= 0 && !(pos < n && s[pos] == '/')) {
        u->opaque = 1;
    }

    int qs = -1, hs = -1;
    for (int k = pos; k < n; k++) {
        if (s[k] == '?' && qs < 0 && hs < 0) qs = k;
        if (s[k] == '#' && hs < 0) { hs = k; break; }
    }
    int pe = qs >= 0 ? qs : (hs >= 0 ? hs : n);
    if (u->opaque) setstrn(&u->opath, s + pos, pe - pos);
    else {
        path_clear(u);
        if (pos < pe || u->host) {
            int st = pos;
            for (int k = pos; k <= pe; k++)
                if (k == pe || s[k] == '/') { if (!(k == pos && s[k] == '/')) path_push(u, s + st, k - st); st = k + 1; }
            if (pos < pe && s[pos] == '/') { /* leading slash consumed */ }
        }
    }
    if (qs >= 0) setstrn(&u->query, s + qs + 1, (hs >= 0 ? hs : n) - qs - 1);
    if (hs >= 0) setstrn(&u->fragment, s + hs + 1, n - hs - 1);
    if (!u->host && !u->opaque && u->npath == 0) path_push(u, "", 0);
    return 0;
}

#endif /* URL_NAIVE_SPLIT */

/* ===================================================================== *
 *  entry points
 * ===================================================================== */

static char *preprocess(const char *in, int len, int fresh, int *outlen)
{
    int sl = 0;
    char *s = utf8_sanitize(in ? in : "", len, &sl);
    if (!s) return 0;
    int a = 0, b = sl;
    if (fresh) {
        while (a < b && ((unsigned char)s[a] <= 0x20)) a++;
        while (b > a && ((unsigned char)s[b - 1] <= 0x20)) b--;
    }
    ub out; ub_init(&out);
    for (int i = a; i < b; i++) {
        char c = s[i];
        if (c == 0x09 || c == 0x0A || c == 0x0D) continue;
        ub_putc(&out, (unsigned char)c);
    }
    free(s);
    if (outlen) *outlen = out.n;
    return ub_take(&out);
}

urlrec *url_parse_w(const char *input, int len, const urlrec *base)
{
    if (len < 0) len = input ? (int)strlen(input) : 0;
    urlrec *u = url_new();
    if (!u) return 0;
    int n = 0;
    char *s = preprocess(input, len, 1, &n);
    if (!s) { url_free_w(u); return 0; }
    int rc = basic_parse(s, n, base, u, 0);
    free(s);
    if (rc != 0) { url_free_w(u); return 0; }
    return u;
}

/* ===================================================================== *
 *  serialization
 * ===================================================================== */

/* The URL PATH serializer -- what `pathname` returns. Note what is NOT here:
 * the "/." prefix belongs to the URL serializer below and not to this one, so
 * `new URL("non-spec:/.//").pathname` is "//" while its href keeps the "/.".
 * Putting it here instead passes every ordinary URL and fails twelve corpus
 * cases, which is the shape of most of the mistakes in this file. */
static void serialize_path(const urlrec *u, ub *b)
{
    if (u->opaque) { ub_puts(b, u->opath ? u->opath : ""); return; }
    for (int i = 0; i < u->npath; i++) { ub_putc(b, '/'); ub_puts(b, u->path[i]); }
}

static void serialize_href(const urlrec *u, ub *b, int exclude_fragment)
{
    ub_puts(b, u->scheme);
    ub_putc(b, ':');
    if (u->host) {
        ub_puts(b, "//");
        if (u->username[0] || u->password[0]) {
            ub_puts(b, u->username);
            if (u->password[0]) { ub_putc(b, ':'); ub_puts(b, u->password); }
            ub_putc(b, '@');
        }
        ub_puts(b, u->host);
        if (u->port >= 0) { ub_putc(b, ':'); ub_putu(b, (unsigned long)u->port); }
    } else if (!u->opaque && u->npath > 1 && u->path[0][0] == 0) {
        /* A null host and a path whose first segment is empty would serialize
         * to "//x", which re-parses as an AUTHORITY. The "/." makes the
         * round trip honest. */
        ub_puts(b, "/.");
    }
    serialize_path(u, b);
    if (u->query) { ub_putc(b, '?'); ub_puts(b, u->query); }
    if (!exclude_fragment && u->fragment) { ub_putc(b, '#'); ub_puts(b, u->fragment); }
}

static void serialize_origin(const urlrec *u, ub *b)
{
    if (!strcmp(u->scheme, "blob")) {
        /* the path, parsed as a URL; http/https/file give their origin */
        if (u->opaque && u->opath) {
            urlrec *p = url_parse_w(u->opath, -1, 0);
            if (p) {
                if (!strcmp(p->scheme, "http") || !strcmp(p->scheme, "https") ||
                    !strcmp(p->scheme, "file")) {
                    serialize_origin(p, b);
                    url_free_w(p);
                    return;
                }
                url_free_w(p);
            }
        }
        ub_puts(b, "null");
        return;
    }
    if (!strcmp(u->scheme, "ftp") || !strcmp(u->scheme, "http") ||
        !strcmp(u->scheme, "https") || !strcmp(u->scheme, "ws") ||
        !strcmp(u->scheme, "wss")) {
        ub_puts(b, u->scheme);
        ub_puts(b, "://");
        ub_puts(b, u->host ? u->host : "");
        if (u->port >= 0) { ub_putc(b, ':'); ub_putu(b, (unsigned long)u->port); }
        return;
    }
    ub_puts(b, "null");
}

char *url_get(const urlrec *u, int comp)
{
    ub b; ub_init(&b);
    switch (comp) {
    case URLC_HREF: serialize_href(u, &b, 0); break;
    case URLC_PROTOCOL: ub_puts(&b, u->scheme); ub_putc(&b, ':'); break;
    case URLC_USERNAME: ub_puts(&b, u->username); break;
    case URLC_PASSWORD: ub_puts(&b, u->password); break;
    case URLC_HOST:
        if (u->host) {
            ub_puts(&b, u->host);
            if (u->port >= 0) { ub_putc(&b, ':'); ub_putu(&b, (unsigned long)u->port); }
        }
        break;
    case URLC_HOSTNAME: if (u->host) ub_puts(&b, u->host); break;
    case URLC_PORT: if (u->port >= 0) ub_putu(&b, (unsigned long)u->port); break;
    case URLC_PATHNAME: serialize_path(u, &b); break;
    case URLC_SEARCH: if (u->query && u->query[0]) { ub_putc(&b, '?'); ub_puts(&b, u->query); } break;
    case URLC_HASH: if (u->fragment && u->fragment[0]) { ub_putc(&b, '#'); ub_puts(&b, u->fragment); } break;
    case URLC_ORIGIN: serialize_origin(u, &b); break;
    default: break;
    }
    return ub_take(&b);
}

const char *url_query_raw(const urlrec *u) { return u->query; }

void url_set_query_raw(urlrec *u, const char *q)
{
    setstr(&u->query, q);
    if (!q) strip_opaque_trailing_spaces(u);
}

/* ===================================================================== *
 *  the setters
 * ===================================================================== */

int url_set(urlrec *u, int comp, const char *value, int len)
{
    if (len < 0) len = value ? (int)strlen(value) : 0;

    if (comp == URLC_HREF) {
        urlrec *nu = url_parse_w(value, len, 0);
        if (!nu) return -1;
        /* move nu into u in place, so every JS reference keeps working */
        free(u->scheme); free(u->username); free(u->password); free(u->host);
        free(u->opath); free(u->query); free(u->fragment);
        path_clear(u); free(u->path);
        u->scheme = nu->scheme; u->username = nu->username; u->password = nu->password;
        u->host = nu->host; u->port = nu->port; u->opaque = nu->opaque;
        u->opath = nu->opath; u->path = nu->path; u->npath = nu->npath; u->cpath = nu->cpath;
        u->query = nu->query; u->fragment = nu->fragment;
        free(nu);
        return 0;
    }

    /* Everything below goes through the parser with a state override, so
     * every setter strips tab/newline and NONE of them strips leading or
     * trailing spaces -- the asymmetry url-setters-stripping.any.js is
     * entirely about. It falls out of `fresh = 0` here and `fresh = 1` in
     * url_parse_w, which is the spec's "if url is not given" condition. */
    int n = 0;
    char *s;

    switch (comp) {
    case URLC_PROTOCOL: {
        ub t; ub_init(&t);
        ub_put(&t, value ? value : "", len);
        ub_putc(&t, ':');
        int rawn = t.n;
        char *raw = ub_take(&t);
        s = preprocess(raw, rawn, 0, &n);
        free(raw);
        if (s) { basic_parse(s, n, 0, u, ST_SCHEME_START); free(s); }
        return 0; }

    case URLC_USERNAME:
    case URLC_PASSWORD: {
        if (url_no_creds_slot(u)) return 0;
        /* NOT preprocess(): these two setters do not run the basic URL parser
         * at all -- the spec percent-encodes the value directly -- so unlike
         * every other setter here they do not remove tab and newline either.
         * url-setters-stripping.any.js tests exactly that, and the two cases
         * in setters_tests.json want %09%0A%0D in the output. */
        s = utf8_sanitize(value ? value : "", len, &n);
        if (!s) return 0;
        ub t; ub_init(&t);
        ub_put_pct(&t, s, n, PCT_USERINFO);
        free(s);
        char **dst = (comp == URLC_USERNAME) ? &u->username : &u->password;
        free(*dst); *dst = ub_take(&t);
        return 0; }

    case URLC_HOST:
    case URLC_HOSTNAME: {
        if (u->opaque) return 0;
        s = preprocess(value, len, 0, &n);
        if (!s) return 0;
        basic_parse(s, n, 0, u, comp == URLC_HOST ? ST_HOST : ST_HOSTNAME);
        free(s);
        return 0; }

    case URLC_PORT: {
        if (url_no_creds_slot(u)) return 0;
        /* "If the given value is the empty string" is about the RAW value,
         * before tab/newline removal. `url.port = "

		"` is therefore
         * NOT the empty string: it goes to the parser, which strips it to
         * nothing, reaches port state at EOF with an empty buffer and returns
         * leaving the port ALONE. Testing the stripped length instead sets the
         * port to null, which is a different answer. */
        if (len == 0) { u->port = -1; return 0; }
        s = preprocess(value, len, 0, &n);
        if (!s) return 0;
        basic_parse(s, n, 0, u, ST_PORT);
        free(s);
        return 0; }

    case URLC_PATHNAME: {
        if (u->opaque) return 0;
        s = preprocess(value, len, 0, &n);
        if (!s) return 0;
        path_clear(u);
        basic_parse(s, n, 0, u, ST_PATH_START);
        free(s);
        return 0; }

    case URLC_SEARCH: {
        /* the raw value, as for port above */
        if (len == 0) {
            setstr(&u->query, 0);
            strip_opaque_trailing_spaces(u);
            return 0;
        }
        s = preprocess(value, len, 0, &n);
        if (!s) return 0;
        const char *in = s; int inl = n;
        if (in[0] == '?') { in++; inl--; }
        setstr(&u->query, "");
        basic_parse(in, inl, 0, u, ST_QUERY);
        free(s);
        return 0; }

    case URLC_HASH: {
        /* the raw value, as for port above */
        if (len == 0) {
            setstr(&u->fragment, 0);
            strip_opaque_trailing_spaces(u);
            return 0;
        }
        s = preprocess(value, len, 0, &n);
        if (!s) return 0;
        const char *in = s; int inl = n;
        if (in[0] == '#') { in++; inl--; }
        setstr(&u->fragment, "");
        basic_parse(in, inl, 0, u, ST_FRAGMENT);
        free(s);
        return 0; }

    default:
        return 0;    /* origin is readonly */
    }
}

/* ===================================================================== *
 *  application/x-www-form-urlencoded
 * ===================================================================== */

void usp_init(usplist *l) { l->v = 0; l->n = l->cap = 0; }

void usp_clear(usplist *l)
{
    for (int i = 0; i < l->n; i++) { free(l->v[i].name); free(l->v[i].value); }
    l->n = 0;
}

void usp_append(usplist *l, const char *name, int nlen, const char *value, int vlen)
{
    if (nlen < 0) nlen = name ? (int)strlen(name) : 0;
    if (vlen < 0) vlen = value ? (int)strlen(value) : 0;
    if (l->n + 1 > l->cap) {
        int nc = l->cap ? l->cap * 2 : 8;
        uspair *nv = (uspair *)xalloc((int)sizeof(uspair) * nc);
        if (!nv) return;
        for (int i = 0; i < l->n; i++) nv[i] = l->v[i];
        free(l->v);
        l->v = nv; l->cap = nc;
    }
    l->v[l->n].name = xstrndup(name ? name : "", nlen);
    l->v[l->n].nlen = nlen;
    l->v[l->n].value = xstrndup(value ? value : "", vlen);
    l->v[l->n].vlen = vlen;
    l->n++;
}

/* percent-decode after turning '+' into a space, then sanitise: the standard
 * says the result is UTF-8 decoded without BOM, so a byte sequence that is not
 * valid UTF-8 becomes U+FFFD rather than escaping into a JS string. */
static char *form_decode(const char *s, int n, int *outlen)
{
    ub t; ub_init(&t);
    for (int i = 0; i < n; i++) ub_putc(&t, s[i] == '+' ? ' ' : (unsigned char)s[i]);
    int pn = t.n;
    char *plus = ub_take(&t);
    int dl = 0;
    char *dec = url_percent_decode(plus, pn, &dl);
    free(plus);
    char *san = utf8_sanitize(dec, dl, outlen);
    free(dec);
    return san;
}

void usp_parse(usplist *l, const char *s, int len)
{
    if (len < 0) len = s ? (int)strlen(s) : 0;
    usp_clear(l);
    int start = 0;
    for (int i = 0; i <= len; i++) {
        if (i != len && s[i] != '&') continue;
        int seq = i - start;
        if (seq > 0) {
            int eq = -1;
            for (int k = 0; k < seq; k++) if (s[start + k] == '=') { eq = k; break; }
            char *name, *val;
            int nl = 0, vl = 0;
            if (eq >= 0) {
                name = form_decode(s + start, eq, &nl);
                val = form_decode(s + start + eq + 1, seq - eq - 1, &vl);
            } else {
                name = form_decode(s + start, seq, &nl);
                val = xstrdup("");
            }
            usp_append(l, name, nl, val, vl);
            free(name); free(val);
        }
        start = i + 1;
    }
}

static void form_serialize_one(ub *b, const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ') ub_putc(b, '+');
        else if (in_set(PCT_FORM, c)) ub_pct(b, c);
        else ub_putc(b, (int)c);
    }
}

int usp_serialize_len(const usplist *l, char **out)
{
    ub b; ub_init(&b);
    for (int i = 0; i < l->n; i++) {
        if (i) ub_putc(&b, '&');
        form_serialize_one(&b, l->v[i].name, l->v[i].nlen);
        ub_putc(&b, '=');
        form_serialize_one(&b, l->v[i].value, l->v[i].vlen);
    }
    int n = b.n;
    *out = ub_take(&b);
    return n;
}

char *usp_serialize(const usplist *l)
{
    char *s = 0;
    usp_serialize_len(l, &s);
    return s;
}

/* Compare two UTF-8 strings in UTF-16 code-unit order. A code point above the
 * BMP sorts as its LEADING SURROGATE, which is below U+E000 -- so an emoji
 * sorts before U+FFFD, which byte-wise UTF-8 comparison gets backwards. That
 * is not a hypothetical: urlsearchparams-sort.any.js has exactly that pair. */
static unsigned sortkey(const unsigned char *s, int n, int *i)
{
    unsigned cp = utf8_next(s, n, i);
    return cp < 0x10000 ? cp : (0xD800 + ((cp - 0x10000) >> 10));
}

static int u16cmp(const char *a, int an, const char *b, int bn)
{
    const unsigned char *ua = (const unsigned char *)a, *ub2 = (const unsigned char *)b;
    int i = 0, j = 0;
    while (i < an && j < bn) {
        int i0 = i, j0 = j;
        unsigned ka = sortkey(ua, an, &i), kb = sortkey(ub2, bn, &j);
        if (ka != kb) return ka < kb ? -1 : 1;
        /* a supplementary code point contributes two units; the trail
         * surrogates only differ if the code points did, so one comparison
         * is enough -- but guard against a stalled index. */
        if (i == i0 || j == j0) break;
    }
    if (i < an) return 1;
    if (j < bn) return -1;
    return 0;
}

void usp_sort(usplist *l)
{
    /* insertion sort: stable by construction, which the standard requires. */
    for (int i = 1; i < l->n; i++) {
        uspair t = l->v[i];
        int j = i - 1;
        while (j >= 0 && u16cmp(l->v[j].name, l->v[j].nlen, t.name, t.nlen) > 0)
            { l->v[j + 1] = l->v[j]; j--; }
        l->v[j + 1] = t;
    }
}

/* ===================================================================== *
 *  the JS bindings
 * ===================================================================== */
#ifndef URL_CORE_ONLY

#include "quickjs.h"

static JSClassID g_url_class;
static JSClassID g_usp_class;

/* A URL and its searchParams share one record. The link is TWO-WAY and both
 * directions are here, because a one-directional one looks right in a demo:
 *   url -> params   the `search` setter and `href` setter re-parse the list
 *   params -> url   every mutator re-serializes into url's query
 * The params object holds a strong reference to the URL wrapper so the URL
 * cannot be collected while script holds only the params. */
typedef struct jsusp {
    usplist list;
    JSValue url_obj;      /* JS_UNDEFINED for a standalone URLSearchParams */
    struct jsurl *owner;  /* NULL when standalone */
} jsusp;

typedef struct jsurl {
    urlrec *rec;
    JSValue params;       /* JS_UNDEFINED until first accessed */
} jsurl;

static void url_finalizer(JSRuntime *rt, JSValue val)
{
    jsurl *j = (jsurl *)JS_GetOpaque(val, g_url_class);
    if (!j) return;
    url_free_w(j->rec);
    JS_FreeValueRT(rt, j->params);
    free(j);
}

static void usp_finalizer(JSRuntime *rt, JSValue val)
{
    jsusp *j = (jsusp *)JS_GetOpaque(val, g_usp_class);
    if (!j) return;
    usp_clear(&j->list);
    free(j->list.v);
    JS_FreeValueRT(rt, j->url_obj);
    free(j);
}

static void url_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    jsurl *j = (jsurl *)JS_GetOpaque(val, g_url_class);
    if (j) JS_MarkValue(rt, j->params, mark_func);
}

static void usp_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    jsusp *j = (jsusp *)JS_GetOpaque(val, g_usp_class);
    if (j) JS_MarkValue(rt, j->url_obj, mark_func);
}

static JSClassDef url_class_def = { "URL", .finalizer = url_finalizer, .gc_mark = url_gc_mark };
static JSClassDef usp_class_def = { "URLSearchParams", .finalizer = usp_finalizer, .gc_mark = usp_gc_mark };

static JSValue str_take(JSContext *ctx, char *s)
{
    JSValue v = JS_NewString(ctx, s ? s : "");
    free(s);
    return v;
}

/* JS_ToCString stops at the first U+0000, and a JS string may contain one.
 * Three WPT subtests and one setter case turn on it, so every string that
 * crosses into this file crosses with its length. */
static char *js_str(JSContext *ctx, JSValueConst v, int *len)
{
    size_t n = 0;
    const char *p = JS_ToCStringLen(ctx, &n, v);
    if (!p) { *len = 0; return 0; }
    char *d = xstrndup(p, (int)n);
    JS_FreeCString(ctx, p);
    *len = (int)n;
    return d;
}

/* params -> url */
static void usp_update_url(jsusp *j)
{
    if (!j->owner) return;
    char *s = 0;
    int n = usp_serialize_len(&j->list, &s);
    /* An EMPTY list makes the query NULL, not "" -- otherwise deleting the
     * last parameter leaves a bare "?" on the href. */
    url_set_query_raw(j->owner->rec, n ? s : 0);
    free(s);
}

/* url -> params */
static void url_update_params(JSContext *ctx, jsurl *j)
{
    if (JS_IsUndefined(j->params)) return;
    jsusp *p = (jsusp *)JS_GetOpaque(j->params, g_usp_class);
    if (!p) return;
    const char *q = url_query_raw(j->rec);
    usp_parse(&p->list, q ? q : "", q ? (int)strlen(q) : 0);
}

/* ---- URLSearchParams ---------------------------------------------------- */

static jsusp *usp_self(JSContext *ctx, JSValueConst v)
{
    return (jsusp *)JS_GetOpaque2(ctx, v, g_usp_class);
}

static int usp_init_from(JSContext *ctx, jsusp *j, JSValueConst init)
{
    if (JS_IsUndefined(init) || JS_IsNull(init)) return 0;

    /* another URLSearchParams */
    jsusp *other = (jsusp *)JS_GetOpaque(init, g_usp_class);
    if (other) {
        for (int i = 0; i < other->list.n; i++)
            usp_append(&j->list, other->list.v[i].name, other->list.v[i].nlen,
                       other->list.v[i].value, other->list.v[i].vlen);
        return 0;
    }
    if (JS_IsObject(init)) {
        /* a sequence if it has Symbol.iterator, else a record */
        JSValue itf = JS_UNDEFINED;
        JSAtom sym = JS_ATOM_NULL;
        {
            JSValue g = JS_GetGlobalObject(ctx);
            JSValue symbol = JS_GetPropertyStr(ctx, g, "Symbol");
            JS_FreeValue(ctx, g);
            if (JS_IsObject(symbol)) {
                JSValue it = JS_GetPropertyStr(ctx, symbol, "iterator");
                sym = JS_ValueToAtom(ctx, it);
                JS_FreeValue(ctx, it);
            }
            JS_FreeValue(ctx, symbol);
        }
        if (sym != JS_ATOM_NULL) {
            itf = JS_GetProperty(ctx, init, sym);
            JS_FreeAtom(ctx, sym);
        }
        if (JS_IsFunction(ctx, itf)) {
            JS_FreeValue(ctx, itf);
            /* iterate as a sequence of 2-element sequences */
            JSValue len = JS_GetPropertyStr(ctx, init, "length");
            uint32_t n = 0;
            if (!JS_IsUndefined(len)) JS_ToUint32(ctx, &n, len);
            JS_FreeValue(ctx, len);
            for (uint32_t i = 0; i < n; i++) {
                JSValue pair = JS_GetPropertyUint32(ctx, init, i);
                JSValue a = JS_GetPropertyUint32(ctx, pair, 0);
                JSValue b = JS_GetPropertyUint32(ctx, pair, 1);
                JSValue c = JS_GetPropertyStr(ctx, pair, "length");
                uint32_t pl = 0;
                if (!JS_IsUndefined(c)) JS_ToUint32(ctx, &pl, c);
                JS_FreeValue(ctx, c);
                if (pl != 2) {
                    JS_FreeValue(ctx, a); JS_FreeValue(ctx, b); JS_FreeValue(ctx, pair);
                    JS_ThrowTypeError(ctx, "URLSearchParams: each element must be a pair");
                    return -1;
                }
                int la = 0, lb = 0;
                char *sa = js_str(ctx, a, &la);
                char *sb = js_str(ctx, b, &lb);
                usp_append(&j->list, sa ? sa : "", la, sb ? sb : "", lb);
                free(sa); free(sb);
                JS_FreeValue(ctx, a); JS_FreeValue(ctx, b); JS_FreeValue(ctx, pair);
            }
            return 0;
        }
        JS_FreeValue(ctx, itf);
        /* a record */
        JSPropertyEnum *tab = 0;
        uint32_t cnt = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &cnt, init, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < cnt; i++) {
                JSValue v = JS_GetProperty(ctx, init, tab[i].atom);
                JSValue kv = JS_AtomToValue(ctx, tab[i].atom);
                int lk = 0, lv = 0;
                char *k = js_str(ctx, kv, &lk);
                char *sv = js_str(ctx, v, &lv);
                usp_append(&j->list, k ? k : "", lk, sv ? sv : "", lv);
                free(k); free(sv);
                JS_FreeValue(ctx, kv);
                JS_FreeValue(ctx, v);
                JS_FreeAtom(ctx, tab[i].atom);
            }
            js_free(ctx, tab);
        }
        return 0;
    }
    /* a string */
    {
        int n = 0;
        char *s = js_str(ctx, init, &n);
        if (!s) return -1;
        const char *in = s;
        if (n > 0 && in[0] == '?') { in++; n--; }
        usp_parse(&j->list, in, n);
        free(s);
    }
    return 0;
}

static JSValue usp_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) return proto;
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, g_usp_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    jsusp *j = (jsusp *)xalloc((int)sizeof(jsusp));
    if (!j) { JS_FreeValue(ctx, obj); return JS_ThrowOutOfMemory(ctx); }
    memset(j, 0, sizeof(*j));
    usp_init(&j->list);
    j->url_obj = JS_UNDEFINED;
    JS_SetOpaque(obj, j);
    if (argc > 0 && usp_init_from(ctx, j, argv[0]) < 0) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    return obj;
}

enum { USP_APPEND, USP_DELETE, USP_GET, USP_GETALL, USP_HAS, USP_SET,
       USP_SORT, USP_TOSTRING, USP_FOREACH, USP_KEYS, USP_VALUES, USP_ENTRIES };

static JSValue usp_method(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv, int magic)
{
    jsusp *j = usp_self(ctx, this_val);
    if (!j) return JS_EXCEPTION;
    char *a = 0, *b = 0;
    int la = 0, lb = 0, have_b = 0;
    JSValue ret = JS_UNDEFINED;

    if (magic == USP_APPEND || magic == USP_SET) {
        if (argc < 2) return JS_ThrowTypeError(ctx, "2 arguments required");
    }
    if (magic <= USP_SET && magic != USP_SORT) {
        if (argc > 0) a = js_str(ctx, argv[0], &la);
        if (argc > 1 && !JS_IsUndefined(argv[1]) &&
            (magic == USP_APPEND || magic == USP_SET ||
             magic == USP_DELETE || magic == USP_HAS)) {
            b = js_str(ctx, argv[1], &lb);
            have_b = 1;
        }
    }
    /* Name and value are matched with their LENGTHS -- see the note on
     * uspair. strcmp here would make a name and that name plus a U+0000 and
     * more text the same parameter. */
#define NEQ(i) (a && (i).nlen == la && !memcmp((i).name, a, (size_t)la))
#define VEQ(i) (!have_b || ((i).vlen == lb && !memcmp((i).value, b, (size_t)lb)))

    switch (magic) {
    case USP_APPEND:
        usp_append(&j->list, a ? a : "", la, b ? b : "", lb);
        usp_update_url(j);
        break;
    case USP_DELETE: {
        int w = 0;
        for (int i = 0; i < j->list.n; i++) {
            int drop = NEQ(j->list.v[i]) && VEQ(j->list.v[i]);
            if (drop) { free(j->list.v[i].name); free(j->list.v[i].value); }
            else j->list.v[w++] = j->list.v[i];
        }
        j->list.n = w;
        usp_update_url(j);
        break; }
    case USP_GET:
        ret = JS_NULL;
        for (int i = 0; i < j->list.n; i++)
            if (NEQ(j->list.v[i])) {
                ret = JS_NewStringLen(ctx, j->list.v[i].value, (size_t)j->list.v[i].vlen);
                break;
            }
        break;
    case USP_GETALL: {
        ret = JS_NewArray(ctx);
        uint32_t k = 0;
        for (int i = 0; i < j->list.n; i++)
            if (NEQ(j->list.v[i]))
                JS_SetPropertyUint32(ctx, ret, k++,
                    JS_NewStringLen(ctx, j->list.v[i].value, (size_t)j->list.v[i].vlen));
        break; }
    case USP_HAS: {
        int found = 0;
        for (int i = 0; i < j->list.n; i++)
            if (NEQ(j->list.v[i]) && VEQ(j->list.v[i])) { found = 1; break; }
        ret = JS_NewBool(ctx, found);
        break; }
    case USP_SET: {
        int seen = -1, w = 0;
        for (int i = 0; i < j->list.n; i++) {
            if (NEQ(j->list.v[i])) {
                if (seen < 0) {
                    free(j->list.v[i].value);
                    j->list.v[i].value = xstrndup(b ? b : "", lb);
                    j->list.v[i].vlen = lb;
                    seen = w;
                    j->list.v[w++] = j->list.v[i];
                } else { free(j->list.v[i].name); free(j->list.v[i].value); }
            } else j->list.v[w++] = j->list.v[i];
        }
        j->list.n = w;
        if (seen < 0) usp_append(&j->list, a ? a : "", la, b ? b : "", lb);
        usp_update_url(j);
        break; }
    case USP_SORT:
        usp_sort(&j->list);
        usp_update_url(j);
        break;
    case USP_TOSTRING: {
        char *out = 0;
        int n = usp_serialize_len(&j->list, &out);
        ret = JS_NewStringLen(ctx, out ? out : "", (size_t)n);
        free(out);
        break; }
    case USP_FOREACH: {
        if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
            return JS_ThrowTypeError(ctx, "callback is not a function");
        JSValueConst thisArg = argc > 1 ? argv[1] : JS_UNDEFINED;
        for (int i = 0; i < j->list.n; i++) {
            JSValue args[3];
            args[0] = JS_NewStringLen(ctx, j->list.v[i].value, (size_t)j->list.v[i].vlen);
            args[1] = JS_NewStringLen(ctx, j->list.v[i].name, (size_t)j->list.v[i].nlen);
            args[2] = JS_DupValue(ctx, this_val);
            JSValue r = JS_Call(ctx, argv[0], thisArg, 3, (JSValueConst *)args);
            JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]); JS_FreeValue(ctx, args[2]);
            if (JS_IsException(r)) { free(a); free(b); return r; }
            JS_FreeValue(ctx, r);
        }
        break; }
    case USP_KEYS: case USP_VALUES: case USP_ENTRIES: {
        /* Materialise into an array and hand back its iterator. The observable
         * difference from a live iterator is mutation DURING iteration, which
         * the corpus does test -- see the note in tests/url.mk. */
        JSValue arr = JS_NewArray(ctx);
        for (int i = 0; i < j->list.n; i++) {
            JSValue item;
            JSValue nm = JS_NewStringLen(ctx, j->list.v[i].name, (size_t)j->list.v[i].nlen);
            JSValue vl = JS_NewStringLen(ctx, j->list.v[i].value, (size_t)j->list.v[i].vlen);
            if (magic == USP_KEYS) { item = nm; JS_FreeValue(ctx, vl); }
            else if (magic == USP_VALUES) { item = vl; JS_FreeValue(ctx, nm); }
            else {
                item = JS_NewArray(ctx);
                JS_SetPropertyUint32(ctx, item, 0, nm);
                JS_SetPropertyUint32(ctx, item, 1, vl);
            }
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, item);
        }
        JSValue vf = JS_GetPropertyStr(ctx, arr, "values");
        ret = JS_Call(ctx, vf, arr, 0, 0);
        JS_FreeValue(ctx, vf);
        JS_FreeValue(ctx, arr);
        break; }
    }

    free(a);
    free(b);
    return ret;
#undef NEQ
#undef VEQ
}

static JSValue usp_size_get(JSContext *ctx, JSValueConst this_val)
{
    jsusp *j = usp_self(ctx, this_val);
    if (!j) return JS_EXCEPTION;
    return JS_NewInt32(ctx, j->list.n);
}

/* ---- URL ---------------------------------------------------------------- */

static jsurl *url_self(JSContext *ctx, JSValueConst v)
{
    return (jsurl *)JS_GetOpaque2(ctx, v, g_url_class);
}

static JSValue url_make(JSContext *ctx, JSValueConst proto, urlrec *rec)
{
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, g_url_class);
    if (JS_IsException(obj)) { url_free_w(rec); return obj; }
    jsurl *j = (jsurl *)xalloc((int)sizeof(jsurl));
    if (!j) { JS_FreeValue(ctx, obj); url_free_w(rec); return JS_ThrowOutOfMemory(ctx); }
    j->rec = rec;
    j->params = JS_UNDEFINED;
    JS_SetOpaque(obj, j);
    return obj;
}

/* Shared by the constructor and the two statics. Returns NULL and leaves an
 * exception pending only when a string conversion threw. */
static urlrec *url_from_args(JSContext *ctx, int argc, JSValueConst *argv, int *threw)
{
    *threw = 0;
    int inl = 0;
    char *in = argc > 0 ? js_str(ctx, argv[0], &inl) : 0;
    if (argc > 0 && !in) { *threw = 1; return 0; }
    urlrec *base = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        int bl = 0;
        char *bs = js_str(ctx, argv[1], &bl);
        if (!bs) { free(in); *threw = 1; return 0; }
        base = url_parse_w(bs, bl, 0);
        free(bs);
        if (!base) { free(in); return 0; }
    }
    urlrec *u = url_parse_w(in ? in : "undefined", in ? inl : -1, base);
    url_free_w(base);
    free(in);
    return u;
}

static JSValue url_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "URL: 1 argument required");
    int threw = 0;
    urlrec *u = url_from_args(ctx, argc, argv, &threw);
    if (threw) return JS_EXCEPTION;
    if (!u) return JS_ThrowTypeError(ctx, "Failed to construct 'URL': Invalid URL");
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) { url_free_w(u); return proto; }
    JSValue obj = url_make(ctx, proto, u);
    JS_FreeValue(ctx, proto);
    return obj;
}

static JSValue url_get_prop(JSContext *ctx, JSValueConst this_val, int magic)
{
    jsurl *j = url_self(ctx, this_val);
    if (!j) return JS_EXCEPTION;
    return str_take(ctx, url_get(j->rec, magic));
}

static JSValue url_set_prop(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    jsurl *j = url_self(ctx, this_val);
    if (!j) return JS_EXCEPTION;
    int n = 0;
    char *s = js_str(ctx, val, &n);
    if (!s) return JS_EXCEPTION;
    int rc = url_set(j->rec, magic, s, n);
    free(s);
    if (rc < 0 && magic == URLC_HREF)
        return JS_ThrowTypeError(ctx, "Failed to set the 'href' property on 'URL': Invalid URL");
    if (magic == URLC_HREF || magic == URLC_SEARCH) url_update_params(ctx, j);
    return JS_UNDEFINED;
}

static JSValue url_search_params(JSContext *ctx, JSValueConst this_val)
{
    jsurl *j = url_self(ctx, this_val);
    if (!j) return JS_EXCEPTION;
    if (JS_IsUndefined(j->params)) {
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue ctor = JS_GetPropertyStr(ctx, g, "URLSearchParams");
        JS_FreeValue(ctx, g);
        JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
        JS_FreeValue(ctx, ctor);
        JSValue obj = JS_NewObjectProtoClass(ctx, proto, g_usp_class);
        JS_FreeValue(ctx, proto);
        if (JS_IsException(obj)) return obj;
        jsusp *p = (jsusp *)xalloc((int)sizeof(jsusp));
        if (!p) { JS_FreeValue(ctx, obj); return JS_ThrowOutOfMemory(ctx); }
        memset(p, 0, sizeof(*p));
        usp_init(&p->list);
        p->owner = j;
        p->url_obj = JS_DupValue(ctx, this_val);
        JS_SetOpaque(obj, p);
        const char *q = url_query_raw(j->rec);
        if (q) usp_parse(&p->list, q, (int)strlen(q));
        j->params = obj;
    }
    return JS_DupValue(ctx, j->params);
}

static JSValue url_tojson(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    jsurl *j = url_self(ctx, this_val);
    if (!j) return JS_EXCEPTION;
    return str_take(ctx, url_get(j->rec, URLC_HREF));
}

static JSValue url_tostring(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return url_tojson(ctx, this_val, argc, argv);
}

static JSValue url_static_canparse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "1 argument required");
    int threw = 0;
    urlrec *u = url_from_args(ctx, argc, argv, &threw);
    if (threw) return JS_EXCEPTION;
    int ok = u != 0;
    url_free_w(u);
    return JS_NewBool(ctx, ok);
}

static JSValue url_static_parse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "1 argument required");
    int threw = 0;
    urlrec *u = url_from_args(ctx, argc, argv, &threw);
    if (threw) return JS_EXCEPTION;
    if (!u) return JS_NULL;
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, g, "URL");
    JS_FreeValue(ctx, g);
    JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    JS_FreeValue(ctx, ctor);
    JSValue obj = url_make(ctx, proto, u);
    JS_FreeValue(ctx, proto);
    return obj;
}

static const JSCFunctionListEntry url_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("href", url_get_prop, url_set_prop, URLC_HREF),
    JS_CGETSET_MAGIC_DEF("protocol", url_get_prop, url_set_prop, URLC_PROTOCOL),
    JS_CGETSET_MAGIC_DEF("username", url_get_prop, url_set_prop, URLC_USERNAME),
    JS_CGETSET_MAGIC_DEF("password", url_get_prop, url_set_prop, URLC_PASSWORD),
    JS_CGETSET_MAGIC_DEF("host", url_get_prop, url_set_prop, URLC_HOST),
    JS_CGETSET_MAGIC_DEF("hostname", url_get_prop, url_set_prop, URLC_HOSTNAME),
    JS_CGETSET_MAGIC_DEF("port", url_get_prop, url_set_prop, URLC_PORT),
    JS_CGETSET_MAGIC_DEF("pathname", url_get_prop, url_set_prop, URLC_PATHNAME),
    JS_CGETSET_MAGIC_DEF("search", url_get_prop, url_set_prop, URLC_SEARCH),
    JS_CGETSET_MAGIC_DEF("hash", url_get_prop, url_set_prop, URLC_HASH),
    JS_CGETSET_MAGIC_DEF("origin", url_get_prop, 0, URLC_ORIGIN),
    JS_CGETSET_DEF("searchParams", url_search_params, 0),
    JS_CFUNC_DEF("toJSON", 0, url_tojson),
    JS_CFUNC_DEF("toString", 0, url_tostring),
};

static const JSCFunctionListEntry usp_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("append", 2, usp_method, USP_APPEND),
    JS_CFUNC_MAGIC_DEF("delete", 1, usp_method, USP_DELETE),
    JS_CFUNC_MAGIC_DEF("get", 1, usp_method, USP_GET),
    JS_CFUNC_MAGIC_DEF("getAll", 1, usp_method, USP_GETALL),
    JS_CFUNC_MAGIC_DEF("has", 1, usp_method, USP_HAS),
    JS_CFUNC_MAGIC_DEF("set", 2, usp_method, USP_SET),
    JS_CFUNC_MAGIC_DEF("sort", 0, usp_method, USP_SORT),
    JS_CFUNC_MAGIC_DEF("toString", 0, usp_method, USP_TOSTRING),
    JS_CFUNC_MAGIC_DEF("forEach", 1, usp_method, USP_FOREACH),
    JS_CFUNC_MAGIC_DEF("keys", 0, usp_method, USP_KEYS),
    JS_CFUNC_MAGIC_DEF("values", 0, usp_method, USP_VALUES),
    JS_CFUNC_MAGIC_DEF("entries", 0, usp_method, USP_ENTRIES),
    JS_CGETSET_DEF("size", usp_size_get, 0),
};

void js_url_install(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue g = JS_GetGlobalObject(ctx);

    JS_NewClassID(&g_url_class);
    JS_NewClass(rt, g_url_class, &url_class_def);
    JS_NewClassID(&g_usp_class);
    JS_NewClass(rt, g_usp_class, &usp_class_def);

    /* URLSearchParams first: URL's searchParams getter looks the constructor
     * up on the global to find its prototype. */
    JSValue up = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, up, usp_proto_funcs, (int)(sizeof(usp_proto_funcs) / sizeof(usp_proto_funcs[0])));
    JSValue uc = JS_NewCFunction2(ctx, usp_ctor, "URLSearchParams", 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, uc, up);
    JS_SetClassProto(ctx, g_usp_class, JS_DupValue(ctx, up));
    /* Symbol.iterator === entries, so `for (const [k,v] of params)` works. */
    {
        JSValue ent = JS_GetPropertyStr(ctx, up, "entries");
        JSValue sym = JS_GetGlobalObject(ctx);
        JSValue symbol = JS_GetPropertyStr(ctx, sym, "Symbol");
        JS_FreeValue(ctx, sym);
        if (JS_IsObject(symbol)) {
            JSValue it = JS_GetPropertyStr(ctx, symbol, "iterator");
            JSAtom a = JS_ValueToAtom(ctx, it);
            JS_FreeValue(ctx, it);
            if (a != JS_ATOM_NULL) { JS_SetProperty(ctx, up, a, JS_DupValue(ctx, ent)); JS_FreeAtom(ctx, a); }
        }
        JS_FreeValue(ctx, symbol);
        JS_FreeValue(ctx, ent);
    }
    JS_FreeValue(ctx, up);
    JS_SetPropertyStr(ctx, g, "URLSearchParams", uc);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, url_proto_funcs, (int)(sizeof(url_proto_funcs) / sizeof(url_proto_funcs[0])));
    JSValue ctor = JS_NewCFunction2(ctx, url_ctor, "URL", 1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, g_url_class, proto);
    JS_SetPropertyStr(ctx, ctor, "canParse",
                      JS_NewCFunction(ctx, url_static_canparse, "canParse", 1));
    JS_SetPropertyStr(ctx, ctor, "parse",
                      JS_NewCFunction(ctx, url_static_parse, "parse", 1));
    JS_SetPropertyStr(ctx, g, "URL", ctor);
    /* The legacy alias every polyfill checks for. */
    JS_FreeValue(ctx, g);
}

#endif /* URL_CORE_ONLY */
