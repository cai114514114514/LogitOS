/* HTTP byte ranges, end to end, over the REAL fetch path.
 *
 * WHAT IS REAL HERE AND WHAT IS NOT.  The thing under test is
 * c/apps/browser/browser_rt.c -- the Range header it builds, and the verdict it
 * reaches about the response -- sitting on top of the REAL HTTP/1.1 parser in
 * c/net/http/http1.c and the REAL connection pool in c/net/http/hpool.c.  The
 * only things replaced are the six socket syscalls underneath, through
 * tests/unit/h2stub/logit.h, exactly as tests/unit/h2mux_test.c does it.
 *
 * tests/unit/loader_fakebfetch.c is deliberately NOT used: it IMPLEMENTS
 * bfetch.h in memory, so a test built on it would be asserting that a fake
 * honours a contract, and the request bytes -- which are half of what a Range
 * is -- would never exist.  Here the client writes real HTTP/1.1 onto a pipe
 * and a fixture origin at the other end answers real HTTP/1.1, so the test can
 * read the `Range:` line the browser actually sent.
 *
 * WHY IT MATTERS, measured from a real bilibili page: DASH delivers video as
 * ONE fragmented-MP4 per stream, fetched with Range and answered 206.  Without
 * Range the measured 9,133-second 1080P stream is ~1.9 GB that must arrive
 * before the first frame.
 *
 * WHAT IS OUT OF SCOPE, said out loud: multipart/byteranges.  A multi-range
 * request cannot be expressed by this API and a multipart answer is refused by
 * name rather than parsed -- undoing MIME boundaries is a second body parser,
 * and DASH never asks for one.  Part 7 asserts the refusal.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "logit_abi.h"
#include "http1.h"
#include "bfetch.h"

/* Defined by browser_rt.c; layout.c's name for a sub-resource fetch. Part 10
 * uses it to prove the whole-resource path through the prefetch cache is
 * untouched by any of this. */
int res_fetch(const char *src, unsigned char **buf, int *len);

/* http1.c's content-decoding path, stubbed and DELIBERATELY a hard failure.
 * Every request here asks for `identity` (a ranged one is forced to -- a byte
 * range names bytes of the encoded representation), so if a decode is ever
 * attempted the response side let an encoded slice through and the test must
 * see that, not quietly inflate it. */
int zlib_decompress(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen)
{ (void)in; (void)inlen; (void)out; (void)outcap; if (outlen) *outlen = 0; return -1; }
int inflate_raw(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen)
{ (void)in; (void)inlen; (void)out; (void)outcap; if (outlen) *outlen = 0; return -1; }

static int fails, checks;
#define OK(cond) do { checks++; if (cond) printf("ok   %s\n", #cond); \
                      else { printf("FAIL %s\n", #cond); fails++; } } while (0)

/* ======================= the resource under test ========================= */

/* Deterministic and position-dependent, so a slice delivered at the WRONG
 * offset is a content mismatch rather than a length that happens to agree. */
static unsigned char body_byte(long long i) { return (unsigned char)((i * 31 + 7) & 0xff); }

/* ============================ a pipe buffer ============================== */

struct pipebuf { unsigned char *b; int len, cap, off; };

static void pb_put(struct pipebuf *p, const void *d, int n)
{
    if (n <= 0) return;
    if (p->len + n > p->cap) {
        int c = p->cap ? p->cap : 1024;
        while (c < p->len + n) c *= 2;
        p->b = realloc(p->b, (size_t)c);
        p->cap = c;
    }
    memcpy(p->b + p->len, d, (size_t)n);
    p->len += n;
}
static void pb_puts(struct pipebuf *p, const char *s) { pb_put(p, s, (int)strlen(s)); }
static int  pb_avail(struct pipebuf *p) { return p->len - p->off; }
static int  pb_take(struct pipebuf *p, void *d, int max)
{
    int n = pb_avail(p);
    if (n > max) n = max;
    if (n <= 0) return 0;
    memcpy(d, p->b + p->off, (size_t)n);
    p->off += n;
    if (p->off == p->len) p->off = p->len = 0;
    return n;
}
static void pb_free(struct pipebuf *p) { free(p->b); memset(p, 0, sizeof *p); }

/* ========================== the fixture origin =========================== */

enum {
    FX_HONOR = 0,   /* a well-behaved range server */
    FX_IGNORE,      /* answers 200 with the whole body, Range or not */
    FX_416,         /* always unsatisfiable, discloses the length */
    FX_MULTIPART,   /* 206 with Content-Type: multipart/byteranges */
    FX_NOCR,        /* 206 with no Content-Range at all */
    FX_BADLEN,      /* 206 whose Content-Range and body length disagree */
    FX_SHIFT,       /* 206 whose slice starts somewhere else */
    FX_GZIP,        /* 206 claiming Content-Encoding: gzip */
    FX_SPONTANEOUS, /* 206 to a request that carried no Range */
    FX_STAR,        /* 206 with an unknown complete length */
    FX_REDIRECT,    /* 302 to /full.bin */
    FX_MULTIRANGE   /* echoes a two-range Content-Range on one line */
};

struct fixture { const char *path; long long size; int mode; };
static const struct fixture g_fx[] = {
    { "/full.bin",    4096, FX_HONOR       },
    { "/short.bin",    500, FX_HONOR       },
    { "/norange.bin",  800, FX_IGNORE      },
    { "/oob.bin",      500, FX_416         },
    { "/multi.bin",   4096, FX_MULTIPART   },
    { "/nocr.bin",    4096, FX_NOCR        },
    { "/badlen.bin",  4096, FX_BADLEN      },
    { "/shift.bin",   4096, FX_SHIFT       },
    { "/gz.bin",      4096, FX_GZIP        },
    { "/spont.bin",   4096, FX_SPONTANEOUS },
    { "/star.bin",    4096, FX_STAR        },
    { "/moved.bin",   4096, FX_REDIRECT    },
    { "/mr.bin",      4096, FX_MULTIRANGE  },
};

/* The last request head the origin saw, whole, so a check can read the exact
 * bytes the browser put on the wire rather than a summary of them. */
static char g_head[4096];
static int  g_nreq;

/* Case-insensitive search for a header line; returns its value into `out`. */
static int head_get(const char *head, const char *name, char *out, int cap)
{
    size_t nl = strlen(name);
    const char *p = head;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        if (!eol || eol == p) break;
        if ((size_t)(eol - p) > nl && p[nl] == ':' &&
#ifdef _WIN32
            _strnicmp(p, name, nl) == 0
#else
            strncasecmp(p, name, nl) == 0
#endif
           ) {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            int n = (int)(eol - v);
            if (n >= cap) n = cap - 1;
            memcpy(out, v, (size_t)n);
            out[n] = 0;
            return 1;
        }
        p = eol + 2;
    }
    out[0] = 0;
    return 0;
}

/* Parse `bytes=first-last` / `bytes=first-`. Returns 0 on success. */
static int parse_req_range(const char *v, long long *f, long long *l)
{
    if (strncmp(v, "bytes=", 6) != 0) return -1;
    v += 6;
    char *e;
    *f = strtoll(v, &e, 10);
    if (e == v || *e != '-') return -1;
    v = e + 1;
    if (!*v) { *l = -1; return 0; }
    *l = strtoll(v, &e, 10);
    if (e == v || *e) return -1;
    return 0;
}

static void put_slice(struct pipebuf *out, long long f, long long n)
{
    for (long long i = 0; i < n; i++) {
        unsigned char c = body_byte(f + i);
        pb_put(out, &c, 1);
    }
}

static void serve_one(const struct fixture *fx, const char *head, struct pipebuf *out)
{
    char rv[128];
    int has_range = head_get(head, "Range", rv, sizeof rv);
    long long f = 0, l = -1;
    int good_range = has_range && parse_req_range(rv, &f, &l) == 0;
    char line[256];

    if (fx->mode == FX_REDIRECT) {
        pb_puts(out, "HTTP/1.1 302 Found\r\nLocation: /full.bin\r\n"
                     "Content-Length: 0\r\n\r\n");
        return;
    }
    if (fx->mode == FX_IGNORE || !good_range) {
        if (fx->mode == FX_SPONTANEOUS && !has_range) {
            /* A 206 nobody asked for. Buggy CDNs and caches do this. */
            snprintf(line, sizeof line,
                     "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-99/%lld\r\n"
                     "Content-Length: 100\r\n\r\n", fx->size);
            pb_puts(out, line);
            put_slice(out, 0, 100);
            return;
        }
        snprintf(line, sizeof line,
                 "HTTP/1.1 200 OK\r\nContent-Length: %lld\r\n"
                 "Accept-Ranges: bytes\r\n\r\n", fx->size);
        pb_puts(out, line);
        put_slice(out, 0, fx->size);
        return;
    }

    if (fx->mode == FX_416 || f >= fx->size) {
        snprintf(line, sizeof line,
                 "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%lld\r\n"
                 "Content-Length: 0\r\n\r\n", fx->size);
        pb_puts(out, line);
        return;
    }

    long long last = (l < 0 || l >= fx->size) ? fx->size - 1 : l;   /* clamp */
    long long n = last - f + 1;

    switch (fx->mode) {
    case FX_MULTIPART:
        snprintf(line, sizeof line,
                 "HTTP/1.1 206 Partial Content\r\n"
                 "Content-Type: multipart/byteranges; boundary=SEP\r\n"
                 "Content-Length: %d\r\n\r\n", 44);
        pb_puts(out, line);
        pb_puts(out, "--SEP\r\nContent-Range: bytes 0-3/4096\r\n\r\nABCD");
        return;
    case FX_NOCR:
        snprintf(line, sizeof line,
                 "HTTP/1.1 206 Partial Content\r\nContent-Length: %lld\r\n\r\n", n);
        pb_puts(out, line);
        put_slice(out, f, n);
        return;
    case FX_BADLEN:
        /* Says one slice, sends a shorter one. Content-Length frames the body,
         * so the message is well-formed HTTP and only the cross-check sees it. */
        snprintf(line, sizeof line,
                 "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %lld-%lld/%lld\r\n"
                 "Content-Length: %lld\r\n\r\n", f, last, fx->size, n - 1);
        pb_puts(out, line);
        put_slice(out, f, n - 1);
        return;
    case FX_SHIFT:
        snprintf(line, sizeof line,
                 "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %lld-%lld/%lld\r\n"
                 "Content-Length: %lld\r\n\r\n", f + 8, last + 8, fx->size, n);
        pb_puts(out, line);
        put_slice(out, f + 8, n);
        return;
    case FX_GZIP:
        snprintf(line, sizeof line,
                 "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %lld-%lld/%lld\r\n"
                 "Content-Encoding: gzip\r\nContent-Length: %lld\r\n\r\n", f, last, fx->size, n);
        pb_puts(out, line);
        put_slice(out, f, n);
        return;
    case FX_STAR:
        snprintf(line, sizeof line,
                 "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %lld-%lld/*\r\n"
                 "Content-Length: %lld\r\n\r\n", f, last, n);
        pb_puts(out, line);
        put_slice(out, f, n);
        return;
    case FX_MULTIRANGE:
        /* One Content-Range line naming two ranges: not legal, and exactly the
         * shape a lenient parser would read the first half of and believe. */
        snprintf(line, sizeof line,
                 "HTTP/1.1 206 Partial Content\r\n"
                 "Content-Range: bytes %lld-%lld/%lld, %lld-%lld/%lld\r\n"
                 "Content-Length: %lld\r\n\r\n",
                 f, last, fx->size, f + 100, last + 100, fx->size, n);
        pb_puts(out, line);
        put_slice(out, f, n);
        return;
    default:
        snprintf(line, sizeof line,
                 "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %lld-%lld/%lld\r\n"
                 "Content-Length: %lld\r\nAccept-Ranges: bytes\r\n\r\n",
                 f, last, fx->size, n);
        pb_puts(out, line);
        put_slice(out, f, n);
        return;
    }
}

static void h1_serve(struct pipebuf *in, struct pipebuf *out)
{
    for (;;) {
        int avail = pb_avail(in);
        if (avail <= 0) return;
        char *b = (char *)in->b + in->off;
        char *e = 0;
        for (int i = 0; i + 3 < avail; i++)
            if (!memcmp(b + i, "\r\n\r\n", 4)) { e = b + i + 4; break; }
        if (!e) return;
        int hlen = (int)(e - b);
        if (hlen >= (int)sizeof g_head) hlen = (int)sizeof g_head - 1;
        memcpy(g_head, b, (size_t)hlen);
        g_head[hlen] = 0;
        in->off += (int)(e - b);
        if (in->off == in->len) in->off = in->len = 0;
        g_nreq++;

        /* request-target out of "GET <target> HTTP/1.1" */
        char target[256]; target[0] = 0;
        const char *sp = strchr(g_head, ' ');
        if (sp) {
            const char *sp2 = strchr(sp + 1, ' ');
            int n = sp2 ? (int)(sp2 - sp - 1) : 0;
            if (n > 0 && n < (int)sizeof target) { memcpy(target, sp + 1, (size_t)n); target[n] = 0; }
        }
        const struct fixture *fx = 0;
        for (size_t i = 0; i < sizeof g_fx / sizeof g_fx[0]; i++)
            if (!strcmp(g_fx[i].path, target)) { fx = &g_fx[i]; break; }
        if (!fx) { pb_puts(out, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"); continue; }
        serve_one(fx, g_head, out);
    }
}

/* ============================ the socket stub ============================ */

#define NSOCK 16
struct hsock { int used; struct pipebuf c2s, s2c; };
static struct hsock g_sk[NSOCK];
static unsigned long long g_clock = 1000;
static int g_opens;

int hstub_open(const char *host, int port, int flags)
{
    (void)host; (void)port; (void)flags;
    for (int i = 0; i < NSOCK; i++) {
        if (g_sk[i].used) continue;
        memset(&g_sk[i], 0, sizeof g_sk[i]);
        g_sk[i].used = 1;
        g_opens++;
        return i;
    }
    return -1;
}
static struct hsock *sk(int fd)
{ return (fd >= 0 && fd < NSOCK && g_sk[fd].used) ? &g_sk[fd] : 0; }

int hstub_poll(int fd)
{
    struct hsock *s = sk(fd);
    if (!s) return -1;
    int bits = SOCK_P_CONNECTED | SOCK_P_WRITABLE;
    if (pb_avail(&s->s2c) > 0) bits |= SOCK_P_READABLE;
    return bits;
}
int hstub_send(int fd, const void *buf, int len)
{
    struct hsock *s = sk(fd);
    if (!s) return -1;
    pb_put(&s->c2s, buf, len);
    h1_serve(&s->c2s, &s->s2c);
    return len;
}
int hstub_recv(int fd, void *buf, int max)
{
    struct hsock *s = sk(fd);
    if (!s) return -1;
    return pb_take(&s->s2c, buf, max);
}
int hstub_close(int fd)
{
    struct hsock *s = sk(fd);
    if (!s) return -1;
    pb_free(&s->c2s); pb_free(&s->s2c);
    memset(s, 0, sizeof *s);
    return 0;
}
int hstub_alpn(int fd, char *buf, int max)
{
    (void)fd;
    const char *p = "http/1.1";
    int n = (int)strlen(p);
    if (n >= max) n = max - 1;
    memcpy(buf, p, (size_t)n); buf[n] = 0;
    return n;
}
unsigned long long hstub_now(void) { return g_clock; }

/* ============================== the harness ============================== */

/* Run one request to completion. bfetch_wait() would do it, but a bounded loop
 * makes a hang a failed check instead of a hung CI job. */
static int settle(int id)
{
    for (int i = 0; i < 20000; i++) {
        bfetch_pump();
        g_clock += 1;
        if (bfetch_state(id) != BF_PENDING) return bfetch_state(id);
    }
    return BF_PENDING;
}

static int slice_ok(const unsigned char *p, int n, long long off)
{
    for (int i = 0; i < n; i++) if (p[i] != body_byte(off + i)) return 0;
    return 1;
}

int main(void)
{
    char hv[256];
    bfetch_init();
    bfetch_set_base("http://origin.test/index.html");

    /* ---- 1. the REQUEST: a Range header exists, and only when asked ---- */
    printf("\n-- 1. the request side --\n");
    {
        int id = bfetch_start("http://origin.test/full.bin");
        OK(id >= 0);
        OK(settle(id) == BF_DONE);
        OK(head_get(g_head, "Range", hv, sizeof hv) == 0);   /* none, unasked */
        OK(bfetch_status(id) == 200);
        OK(bfetch_range_result(id, 0, 0, 0) == BF_R_NONE);
        bfetch_release(id);

        id = bfetch_start_range("http://origin.test/full.bin", 100, 199);
        OK(id >= 0);
        OK(settle(id) == BF_DONE);
        OK(head_get(g_head, "Range", hv, sizeof hv) == 1);
        OK(!strcmp(hv, "bytes=100-199"));
        /* A byte range names bytes of the ENCODED representation, and our
         * inflater is one-shot -- so a ranged GET must not invite gzip. */
        OK(head_get(g_head, "Accept-Encoding", hv, sizeof hv) && !strcmp(hv, "identity"));
        /* ONE range, by construction: no comma can appear in this header. */
        OK(head_get(g_head, "Range", hv, sizeof hv) && strchr(hv, ',') == 0);
        bfetch_release(id);

        /* Open-ended: `bytes=first-`, which is how DASH asks for a tail. */
        id = bfetch_start_range("http://origin.test/full.bin", 4000, -1);
        OK(settle(id) == BF_DONE);
        OK(head_get(g_head, "Range", hv, sizeof hv) && !strcmp(hv, "bytes=4000-"));
        bfetch_release(id);
    }

    /* ---- 2. a 206 accepted, parsed, and the BYTES delivered ---- */
    printf("\n-- 2. 206 Partial Content --\n");
    {
        int id = bfetch_start_range("http://origin.test/full.bin", 1000, 1099);
        OK(settle(id) == BF_DONE);
        OK(bfetch_status(id) == 206);
        long long f = -9, l = -9, t = -9;
        OK(bfetch_range_result(id, &f, &l, &t) == BF_R_PARTIAL);
        OK(f == 1000 && l == 1099 && t == 4096);
        int n = 0;
        const unsigned char *p = bfetch_body(id, &n);
        OK(n == 100);
        OK(p && slice_ok(p, n, 1000));         /* the RIGHT 100 bytes */
        bfetch_release(id);

        /* bfetch_take must hand over the partial body and its partial length,
         * not silently believe it holds a whole resource. */
        id = bfetch_start_range("http://origin.test/full.bin", 0, 15);
        OK(settle(id) == BF_DONE);
        unsigned char *own = 0;
        int got = bfetch_take(id, &own);
        OK(got == 16 && own && slice_ok(own, 16, 0));
        free(own);

        /* A server CLAMPING to the end of a shorter resource is legal. */
        id = bfetch_start_range("http://origin.test/short.bin", 400, 999);
        OK(settle(id) == BF_DONE);
        OK(bfetch_range_result(id, &f, &l, &t) == BF_R_PARTIAL);
        OK(f == 400 && l == 499 && t == 500);
        OK(bfetch_body(id, &n) && n == 100);
        bfetch_release(id);

        /* Unknown complete length: `bytes 100-199/*` is legal and total is -1. */
        id = bfetch_start_range("http://origin.test/star.bin", 100, 199);
        OK(settle(id) == BF_DONE);
        OK(bfetch_range_result(id, &f, &l, &t) == BF_R_PARTIAL);
        OK(f == 100 && l == 199 && t == -1);
        bfetch_release(id);

        /* The convenience entry point, whole trip in one call. */
        unsigned char *buf = 0; int blen = 0; long long total = -9;
        OK(bfetch_sync_range("http://origin.test/full.bin", 2048, 2303,
                             &buf, &blen, &total) == 0);
        OK(blen == 256 && total == 4096 && buf && slice_ok(buf, blen, 2048));
        free(buf);
    }

    /* ---- 3. 200 to a Range request: REPORTED, never mistaken for a slice ---- */
    printf("\n-- 3. the server ignored the Range --\n");
    {
        int id = bfetch_start_range("http://origin.test/norange.bin", 100, 199);
        OK(settle(id) == BF_DONE);
        /* The status is not rewritten to 206 to make the caller feel better. */
        OK(bfetch_status(id) == 200);
        long long f = -9, l = -9, t = -9;
        OK(bfetch_range_result(id, &f, &l, &t) == BF_R_IGNORED);
        /* And the body really is the WHOLE resource, not 100 bytes. */
        int n = 0;
        const unsigned char *p = bfetch_body(id, &n);
        OK(n == 800 && p && slice_ok(p, n, 0));
        OK(f == 0 && l == 799 && t == 800);
        bfetch_release(id);

        /* The synchronous entry point refuses it: a caller with nowhere to put
         * the discrepancy would write 800 whole-resource bytes at offset 100. */
        unsigned char *buf = 0; int blen = 0;
        OK(bfetch_sync_range("http://origin.test/norange.bin", 100, 199,
                             &buf, &blen, 0) == -1);
    }

    /* ---- 4. 416 is a NAMED failure ---- */
    printf("\n-- 4. 416 Range Not Satisfiable --\n");
    {
        int id = bfetch_start_range("http://origin.test/oob.bin", 9000, 9099);
        OK(settle(id) == BF_FAILED);
        OK(bfetch_status(id) == 416);
        long long f, l, t = -9;
        OK(bfetch_range_result(id, &f, &l, &t) == BF_R_UNSATISFIABLE);
        OK(t == 500);                                  /* the disclosed length */
        OK(strstr(bfetch_error(id), "416") != 0);
        bfetch_release(id);
    }

    /* ---- 5. a range that cannot be honestly interpreted is refused ---- */
    printf("\n-- 5. malformed and dishonest 206s --\n");
    {
        struct { const char *url; const char *want; } bad[] = {
            { "http://origin.test/nocr.bin",   "without Content-Range"   },
            { "http://origin.test/badlen.bin", "disagrees"               },
            { "http://origin.test/shift.bin",  "does not start where"    },
            { "http://origin.test/gz.bin",     "content-encoded"         },
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            int id = bfetch_start_range(bad[i].url, 200, 299);
            int st = settle(id);
            const char *e = bfetch_error(id);
            printf("     %-32s -> %s\n", bad[i].url, e);
            checks++;
            if (st == BF_FAILED && strstr(e, bad[i].want)) printf("ok   refused: %s\n", bad[i].want);
            else { printf("FAIL not refused by name: %s\n", bad[i].url); fails++; }
            bfetch_release(id);
        }
    }

    /* ---- 6. a 206 nobody asked for ----
     * bfetch_sync() accepts any 2xx, and the prefetch cache is keyed by URL
     * alone, so an unrequested 206 would file a partial body as a whole one. */
    printf("\n-- 6. a 206 to a request with no Range --\n");
    {
        int id = bfetch_start("http://origin.test/spont.bin");
        OK(settle(id) == BF_FAILED);
        OK(strstr(bfetch_error(id), "no Range") != 0);
        bfetch_release(id);
        unsigned char *buf = 0; int blen = 0;
        OK(bfetch_sync("http://origin.test/spont.bin", &buf, &blen) == -1);
    }

    /* ---- 7. multi-range: refused loudly, never mis-parsed ----
     * OUT OF SCOPE, stated: multipart/byteranges is a MIME body with generated
     * boundaries and a Content-Range per part; undoing it is a second body
     * parser and DASH never asks for one. Two prongs: the API cannot express a
     * multi-range request (part 1 asserted the header holds no comma), and an
     * answer that arrives as one is named rather than read as payload. */
    printf("\n-- 7. multipart/byteranges is out of scope, and says so --\n");
    {
        int id = bfetch_start_range("http://origin.test/multi.bin", 0, 3);
        OK(settle(id) == BF_FAILED);
        OK(strstr(bfetch_error(id), "multipart/byteranges") != 0);
        /* And emphatically NOT the boundary text handed back as the slice. */
        int n = -1;
        OK(bfetch_body(id, &n) == 0 && n == 0);
        bfetch_release(id);

        /* A single Content-Range line naming two ranges: a lenient parser reads
         * the first pair and believes it. This one refuses the whole line. */
        id = bfetch_start_range("http://origin.test/mr.bin", 0, 99);
        OK(settle(id) == BF_FAILED);
        OK(strstr(bfetch_error(id), "malformed") != 0);
        bfetch_release(id);
    }

    /* ---- 8. a redirected range still asks for the range ---- */
    printf("\n-- 8. Range survives a redirect --\n");
    {
        int id = bfetch_start_range("http://origin.test/moved.bin", 300, 349);
        OK(settle(id) == BF_DONE);
        OK(head_get(g_head, "Range", hv, sizeof hv) && !strcmp(hv, "bytes=300-349"));
        long long f, l, t;
        OK(bfetch_range_result(id, &f, &l, &t) == BF_R_PARTIAL);
        OK(f == 300 && l == 349);
        int n = 0;
        const unsigned char *p = bfetch_body(id, &n);
        OK(n == 50 && p && slice_ok(p, n, 300));
        bfetch_release(id);
    }

    /* ---- 9. an impossible ask is refused before it costs a round trip ---- */
    printf("\n-- 9. the ask itself --\n");
    {
        int before = g_nreq;
        OK(bfetch_start_range("http://origin.test/full.bin", 200, 100) == -1);
        OK(bfetch_start_range("http://origin.test/full.bin", -5, 100) == -1);
        OK(g_nreq == before);                       /* nothing was sent */
    }

    /* ---- 10. the whole-resource path is unchanged ---- */
    printf("\n-- 10. no regression on the unranged path --\n");
    {
        unsigned char *buf = 0; int blen = 0;
        OK(bfetch_sync("http://origin.test/full.bin", &buf, &blen) == 0);
        OK(blen == 4096 && buf && slice_ok(buf, blen, 0));
        free(buf);
        bfetch_prefetch("http://origin.test/short.bin");
        bfetch_prefetch_wait();
        OK(res_fetch("http://origin.test/short.bin", &buf, &blen) == 0);
        OK(blen == 500 && buf && slice_ok(buf, blen, 0));
        free(buf);
        bfetch_cache_clear();
    }

    bfetch_close_all();
    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
