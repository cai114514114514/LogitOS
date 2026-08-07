/* ASan+UBSan fuzz for the ring-3 HTTP client: c/net/http/{http1,cookies,hpool}.c.
 *
 * WHY THIS FILE EXISTS. The code it replaces -- the kernel's c/net/http/http.c --
 * parses attacker-chosen bytes in RING 0 and has never been fuzzed or unit
 * tested. Response parsing is the browser's widest untrusted-input surface
 * after the HTML tokenizer: every header, every chunk length, every
 * Content-Encoding footer is a number or a length chosen by whoever answered
 * the connection.
 *
 * It is not a crash-only fuzzer. Not crashing is the floor; each phase also
 * asserts a PROPERTY that a memory-safe but wrong parser would still violate:
 *
 *   P1  feed() consumes between 0 and len bytes, or reports an error.
 *   P2  a finished response is self-consistent: body_len within the cap, the
 *       NUL terminator present, no state left half-set.
 *   P3  GRANULARITY INDEPENDENCE -- the same bytes delivered one at a time, in
 *       threes, and all at once must produce the identical outcome. This is
 *       the strongest property here: it is exactly what the kernel's parser
 *       cannot satisfy (it re-scans one accumulating buffer), and it is what
 *       makes "the response arrived in two TCP segments" stop being a bug
 *       class.
 *   P4  a request that builds successfully contains no injected header: the
 *       header section has exactly one CRLF per header plus the terminator.
 *   P5  the cookie jar never returns a cookie to a host that does not
 *       domain-match and path-match it, whatever bytes it was fed.
 *   P6  the pool never exceeds its caps and never closes an in-use connection.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "http1.h"
#include "cookies.h"
#include "hpool.h"

/* The rust staticlib (inflate) also carries png.rs; these satisfy the link. */
void *kmalloc(unsigned long n) { return malloc((size_t)n); }
void  kfree(void *p) { free(p); }
int   img_register(void *d) { (void)d; return 0; }

static int fails;
#define REQUIRE(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d ", __FILE__, __LINE__); \
                                printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* Deterministic PRNG: a fuzz failure has to be reproducible from the seed. */
static uint64_t rng_s = 0x243F6A8885A308D3ULL;
static uint32_t rnd(void)
{
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return (uint32_t)(rng_s >> 32);
}
static uint32_t rnd_n(uint32_t n) { return n ? rnd() % n : 0; }

/* ------------------------------------------------------------ the corpus */

/* Well-formed seeds. Mutation of a valid message reaches far more of the state
 * machine than random bytes, which almost always die on the status line. */
static const char *const seeds[] = {
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Content-Length: 26\r\n\r\n<html><body>hi</body></html>",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3;ext=1;q=\"v\"\r\nabc\r\n0\r\nX-Trailer: 1\r\nX-Other: 2\r\n\r\n",
    "HTTP/1.1 301 Moved Permanently\r\nLocation: http://example.com/x\r\n"
        "Content-Length: 0\r\n\r\n",
    "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok",
    "HTTP/1.1 204 No Content\r\n\r\n",
    "HTTP/1.1 200 OK\r\nSet-Cookie: a=1; Path=/; HttpOnly\r\n"
        "Set-Cookie: b=2; Domain=example.com; Secure\r\nContent-Length: 0\r\n\r\n",
    "HTTP/1.1 200 OK\r\nX-Folded: one\r\n  two\r\n\tthree\r\nContent-Length: 1\r\n\r\nz",
    "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nbody until eof",
    "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n",
    "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 8\r\n\r\n\x1f\x8b\x08\x00\x00\x00\x00\x00",
    NULL
};

/* -------------------------------------------------- parse-outcome summary */

struct outcome {
    int state, err, code, hdrn, trailn, body_len, keep_alive, must_close, interim;
    uint32_t body_hash;
};

static uint32_t fnv(const uint8_t *p, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* Parse `n` bytes at `step` granularity (0 = all at once), then signal EOF. */
static void run_parse(const uint8_t *p, int n, int step, int body_max, struct outcome *o)
{
    struct h1_response r;
    h1_response_init(&r);
    if (body_max > 0) h1_response_limit(&r, body_max);

    int off = 0;
    int s = step > 0 ? step : (n ? n : 1);
    while (off < n) {
        int take = n - off < s ? n - off : s;
        int used = h1_response_feed(&r, p + off, take);
        /* P1: the return is an error, or a consumed count inside the window. */
        REQUIRE(used < 0 || (used >= 0 && used <= take), "feed returned %d for %d bytes", used, take);
        if (used < 0) break;
        off += used;
        if (used < take) break;                 /* message boundary inside the buffer */
    }
    h1_response_eof(&r);

    /* P2: a finished parse is internally consistent. */
    REQUIRE(r.state >= H1_ST_STATUS && r.state <= H1_ST_ERROR, "bogus state %d", r.state);
    REQUIRE(r.body_len >= 0 && r.body_len <= r.body_max, "body_len %d vs cap %d", r.body_len, r.body_max);
    if (r.body) REQUIRE(r.body[r.body_len] == 0, "body not NUL-terminated");
    if (r.state == H1_ST_ERROR) REQUIRE(r.err < 0, "error state with err=%d", r.err);

    o->state = r.state; o->err = r.err; o->code = r.code;
    o->hdrn = r.hdr.n; o->trailn = r.trailer.n; o->body_len = r.body_len;
    o->keep_alive = r.keep_alive; o->must_close = r.must_close; o->interim = r.interim;
    o->body_hash = r.body ? fnv(r.body, r.body_len) : 0;

    h1_response_free(&r);
}

/* P3: granularity independence. */
static void check_granularity(const uint8_t *p, int n, int body_max)
{
    struct outcome a, b, c;
    run_parse(p, n, 0, body_max, &a);
    run_parse(p, n, 1, body_max, &b);
    run_parse(p, n, 3 + (int)rnd_n(29), body_max, &c);
    if (memcmp(&a, &b, sizeof a) || memcmp(&a, &c, sizeof a)) {
        printf("FAIL granularity mismatch: all=(st %d err %d code %d len %d h %08x) "
               "one=(st %d err %d code %d len %d h %08x) "
               "rnd=(st %d err %d code %d len %d h %08x)\n",
               a.state, a.err, a.code, a.body_len, a.body_hash,
               b.state, b.err, b.code, b.body_len, b.body_hash,
               c.state, c.err, c.code, c.body_len, c.body_hash);
        fails++;
    }
}

/* ------------------------------------------------------------- mutations */

static const uint8_t interesting[] = {
    0x00, 0x09, 0x0a, 0x0d, 0x20, '0', '9', 'a', 'f', 'F',
    ':', ';', ',', '-', '+', '.', '/', '=', 0x7f, 0x80, 0xff
};

static void mutate(uint8_t *b, int n)
{
    if (n <= 0) return;
    int k = 1 + (int)rnd_n(6);
    for (int i = 0; i < k; i++) {
        int at = (int)rnd_n((uint32_t)n);
        switch (rnd_n(4)) {
        case 0: b[at] ^= (uint8_t)(1 + rnd_n(255)); break;
        case 1: b[at] = interesting[rnd_n(sizeof interesting)]; break;
        case 2: b[at] = (uint8_t)rnd_n(256); break;
        /* Splice a CR/LF pair in: this is the injection and the framing-
         * confusion mutation at the same time. */
        case 3: if (at + 1 < n) { b[at] = '\r'; b[at + 1] = '\n'; } break;
        }
    }
}

static void phase_mutate_seeds(int iters)
{
    for (int it = 0; it < iters; it++) {
        const char *seed = seeds[rnd_n(12)];
        int sn = (int)strlen(seed);
        int n = sn;
        if (rnd_n(3) == 0) n = 1 + (int)rnd_n((uint32_t)sn);      /* truncate too */
        uint8_t *b = (uint8_t *)malloc((size_t)n + 1);
        memcpy(b, seed, (size_t)n);
        mutate(b, n);
        int cap = (rnd_n(4) == 0) ? (int)(16 + rnd_n(4096)) : 0;
        check_granularity(b, n, cap);
        free(b);
    }
    printf("ok   phase 1: %d mutated well-formed messages, granularity-consistent\n", iters);
}

static void phase_truncations(void)
{
    int total = 0;
    for (int s = 0; seeds[s]; s++) {
        int n = (int)strlen(seeds[s]);
        for (int k = 0; k <= n; k++) {
            check_granularity((const uint8_t *)seeds[s], k, 0);
            total++;
        }
    }
    printf("ok   phase 2: every one of %d truncations parses without crashing\n", total);
}

static void phase_random_bytes(int iters)
{
    for (int it = 0; it < iters; it++) {
        int n = 1 + (int)rnd_n(512);
        uint8_t *b = (uint8_t *)malloc((size_t)n);
        for (int i = 0; i < n; i++) {
            /* Mostly printable so the parser gets past the first byte often
             * enough to be interesting. */
            b[i] = (rnd_n(4) == 0) ? (uint8_t)rnd_n(256)
                                   : (uint8_t)(0x20 + rnd_n(0x5f));
        }
        struct outcome o;
        run_parse(b, n, 1 + (int)rnd_n(8), 0, &o);
        free(b);
    }
    printf("ok   phase 3: %d random byte strings\n", iters);
}

/* ---------------------------- structured attacks on the numeric fields --- */

/* Content-Length and chunk-size are where a parser turns a peer-controlled
 * string into a length. Every shape that has ever broken one goes here. */
static const char *const nasty_numbers[] = {
    "0", "-0", "-1", "+1", " 1", "1 ", "1\t", "0x10", "010", "1e3", "1.0",
    "99999999999999999999999999", "18446744073709551616", "9223372036854775808",
    "4294967296", "4294967295", "2147483648", "65536", "",
    "1,1", "1,2", "1, 1", "1;1", "\xff\xfe", "０", "NaN", "inf",
    "00000000000000000000001", "0000000000000000000000000000005",
    NULL
};
static const char *const nasty_hex[] = {
    "0", "-1", "+1", "-0", "FFFFFFFF", "ffffffffffffffff", "FFFFFFFFFFFFFFFFF",
    "10000000000000000", "0x3", "3 ", " 3", "3\t", "", ";", "3;", "3;a=b",
    "3;a=\"b;c\"", "g", "3g", "\xff", "00000000000000000003", "7fffffffffffffff",
    "80000000000000000", "3\r", "3\n",
    NULL
};

static void phase_numbers(void)
{
    char buf[512];
    int n = 0;
    for (int i = 0; nasty_numbers[i]; i++) {
        int len = sprintf(buf, "HTTP/1.1 200 OK\r\nContent-Length: %s\r\n\r\nhello world",
                          nasty_numbers[i]);
        check_granularity((const uint8_t *)buf, len, 0);
        len = sprintf(buf, "HTTP/1.1 200 OK\r\nContent-Length: %s\r\nContent-Length: %s\r\n\r\nx",
                      nasty_numbers[i], nasty_numbers[(i + 1) % 20]);
        check_granularity((const uint8_t *)buf, len, 0);
        n += 2;
    }
    for (int i = 0; nasty_hex[i]; i++) {
        int len = sprintf(buf, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                               "%s\r\nabcdefgh\r\n0\r\n\r\n", nasty_hex[i]);
        check_granularity((const uint8_t *)buf, len, 0);
        len = sprintf(buf, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                           "3\r\nabc\r\n%s\r\nxyz\r\n0\r\n\r\n", nasty_hex[i]);
        check_granularity((const uint8_t *)buf, len, 0);
        n += 2;
    }
    printf("ok   phase 4: %d hostile Content-Length / chunk-size fields\n", n);
}

/* Header sections designed to blow a bound: thousands of headers, one enormous
 * header, deep folding, a header name that is all colons. */
static void phase_header_bombs(void)
{
    int cases = 0;
    {   /* 10k headers */
        size_t cap = 10000 * 24 + 64;
        char *b = (char *)malloc(cap);
        int o = sprintf(b, "HTTP/1.1 200 OK\r\n");
        for (int i = 0; i < 10000; i++) o += sprintf(b + o, "X-%05d: v\r\n", i);
        o += sprintf(b + o, "\r\n");
        struct outcome out;
        run_parse((const uint8_t *)b, o, 0, 0, &out);
        REQUIRE(out.state == H1_ST_ERROR, "10k headers accepted");
        REQUIRE(out.hdrn <= H1_MAX_HEADERS, "kept %d headers", out.hdrn);
        free(b); cases++;
    }
    {   /* one 1 MiB header line */
        int n = 1 << 20;
        char *b = (char *)malloc((size_t)n + 64);
        int o = sprintf(b, "HTTP/1.1 200 OK\r\nX: ");
        memset(b + o, 'a', (size_t)n); o += n;
        o += sprintf(b + o, "\r\n\r\n");
        struct outcome out;
        run_parse((const uint8_t *)b, o, 0, 0, &out);
        REQUIRE(out.state == H1_ST_ERROR && out.err == H1_E_TOOLARGE, "huge header not capped");
        free(b); cases++;
    }
    {   /* 50k fold continuations onto one header */
        size_t cap = 50000 * 6 + 64;
        char *b = (char *)malloc(cap);
        int o = sprintf(b, "HTTP/1.1 200 OK\r\nX: v\r\n");
        for (int i = 0; i < 50000; i++) o += sprintf(b + o, " f\r\n");
        o += sprintf(b + o, "\r\n");
        struct outcome out;
        run_parse((const uint8_t *)b, o, 0, 0, &out);
        REQUIRE(out.state == H1_ST_ERROR, "unbounded folding accepted");
        free(b); cases++;
    }
    {   /* a wall of trailers after the 0-chunk */
        size_t cap = 5000 * 24 + 128;
        char *b = (char *)malloc(cap);
        int o = sprintf(b, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nz\r\n0\r\n");
        for (int i = 0; i < 5000; i++) o += sprintf(b + o, "T-%05d: v\r\n", i);
        o += sprintf(b + o, "\r\n");
        struct outcome out;
        run_parse((const uint8_t *)b, o, 0, 0, &out);
        REQUIRE(out.trailn <= H1_MAX_HEADERS, "kept %d trailers", out.trailn);
        free(b); cases++;
    }
    {   /* pathological punctuation where a header name belongs */
        const char *odd[] = {
            "HTTP/1.1 200 OK\r\n::::::\r\n\r\n",
            "HTTP/1.1 200 OK\r\n:\r\n\r\n",
            "HTTP/1.1 200 OK\r\nX:\r\n\r\n",
            "HTTP/1.1 200 OK\r\n \r\n\r\n",
            "HTTP/1.1 200 OK\r\n\t\r\n\r\n",
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: \r\n\r\nabc",
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: ,,,\r\n\r\nabc",
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunkedx\r\n\r\nabc",
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: xchunked\r\n\r\nabc",
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: CHUNKED\r\n\r\n0\r\n\r\n",
            "HTTP/1.1 200 OK\r\nConnection: ,close,\r\nContent-Length: 0\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",
            NULL
        };
        for (int i = 0; odd[i]; i++) { check_granularity((const uint8_t *)odd[i], (int)strlen(odd[i]), 0); cases++; }
    }
    printf("ok   phase 5: %d header/trailer bombs bounded\n", cases);
}

/* ------------------------------------------------- request-side injection */

static void phase_request_injection(int iters)
{
    static const char *inject[] = {
        "\r\n", "\n", "\r", "\r\nX-Evil: 1", "\r\n\r\nGET /evil HTTP/1.1\r\n",
        "\x00", " ", "\t", "\x7f", "\xff", "%0d%0a", "a", "", NULL
    };
    int built = 0, refused = 0;
    for (int it = 0; it < iters; it++) {
        char name[64], value[128];
        int nl = (int)rnd_n(20), vl = (int)rnd_n(40);
        for (int i = 0; i < nl; i++) name[i] = (char)(0x20 + rnd_n(0x60));
        name[nl] = 0;
        for (int i = 0; i < vl; i++) value[i] = (char)(0x20 + rnd_n(0x60));
        value[vl] = 0;
        if (rnd_n(2)) {                       /* splice in a hostile fragment */
            const char *f = inject[rnd_n(13)];
            strncat(value, f, sizeof value - strlen(value) - 1);
        }
        if (rnd_n(4) == 0) {
            const char *f = inject[rnd_n(13)];
            strncat(name, f, sizeof name - strlen(name) - 1);
        }

        struct h1_request q;
        if (h1_request_init(&q, "GET", "/") != H1_OK) { h1_request_free(&q); continue; }
        h1_request_set_header(&q, "Host", "example.com");
        int rc = h1_request_set_header(&q, name, value);
        char *out = NULL; int on = 0;
        if (h1_request_build(&q, &out, &on) == H1_OK) {
            /* P4: the header section holds exactly (nheaders + 1) CRLFs, so no
             * value smuggled a line of its own into it. */
            const char *end = strstr(out, "\r\n\r\n");
            REQUIRE(end != NULL, "built request has no header terminator");
            if (end) {
                /* `end` points at the CRLF that closes the last header line,
                 * so [out, end) holds the request line plus n-1 header lines:
                 * exactly n CRLFs, one per line. Any extra one came out of a
                 * value and would be a smuggled header. */
                int crlf = 0, bare = 0;
                for (const char *p = out; p < end; ) {
                    if (p[0] == '\r' && p + 1 < end + 2 && p[1] == '\n') { crlf++; p += 2; continue; }
                    if (p[0] == '\r' || p[0] == '\n') { bare++; }
                    p++;
                }
                REQUIRE(crlf == q.hdr.n, "header section has %d CRLF for %d headers",
                        crlf, q.hdr.n);
                REQUIRE(bare == 0, "%d bare CR/LF inside the header section", bare);
            }
            built++;
            free(out);
        }
        if (rc != H1_OK) refused++;
        h1_request_free(&q);
    }
    printf("ok   phase 6: %d request builds, %d hostile headers refused, no injection\n",
           built, refused);
}

/* --------------------------------------------------------- content-coding */

static void phase_decode(int iters)
{
    static const char *const enc[] = { "gzip", "deflate", "x-gzip", "identity",
                                       "br", "gzip, gzip", "deflate, gzip",
                                       "GZIP", "gzip;q=1", "", NULL };
    for (int it = 0; it < iters; it++) {
        int n = 1 + (int)rnd_n(300);
        uint8_t *b = (uint8_t *)malloc((size_t)n);
        for (int i = 0; i < n; i++) b[i] = (uint8_t)rnd_n(256);
        if (rnd_n(2)) { b[0] = 0x1f; if (n > 1) b[1] = 0x8b; if (n > 2) b[2] = 8; }
        if (rnd_n(4) == 0) { b[0] = 0x78; if (n > 1) b[1] = 0x01; }

        char head[160];
        int hl = sprintf(head, "HTTP/1.1 200 OK\r\nContent-Encoding: %s\r\nContent-Length: %d\r\n\r\n",
                         enc[rnd_n(10)], n);
        struct h1_response r;
        h1_response_init(&r);
        if (rnd_n(4) == 0) h1_response_limit(&r, 64 + (int)rnd_n(8192));
        h1_response_feed(&r, head, hl);
        h1_response_feed(&r, b, n);
        int rc = h1_decode_body(&r);
        REQUIRE(rc == H1_OK || rc == H1_E_ENCODING || rc == H1_E_NOMEM,
                "decode returned %d", rc);
        REQUIRE(r.body_len >= 0 && r.body_len <= r.body_max, "decoded len %d", r.body_len);
        if (r.body) REQUIRE(r.body[r.body_len] == 0, "decoded body not terminated");
        h1_response_free(&r);
        free(b);
    }
    printf("ok   phase 7: %d random Content-Encoding bodies decoded or refused\n", iters);
}

/* -------------------------------------------------------------- cookies */

static const char *const hosts[] = {
    "example.com", "a.example.com", "b.a.example.com", "evil.com",
    "notexample.com", "example.com.evil.com", "co.uk", "bbc.co.uk",
    "www.bbc.co.uk", "10.0.2.2", "localhost", "xn--80ak6aa92e.com"
};
static const char *const paths[] = { "/", "/a", "/a/b", "/a/b/c", "/ab", "/a?q=1", "/a#f" };

/* The RFC 6265 5.4 "should this cookie be sent" predicate, written out here
 * independently of cookies.c so the two have to agree. */
static int fz_applicable(const struct cookie *c, const struct cookie_ctx *q, int64_t now)
{
    if (c->persistent && c->expires <= now) return 0;
    if (c->host_only) { if (strcmp(c->domain, q->host) != 0) return 0; }
    else if (!cookie_domain_match(q->host, c->domain)) return 0;
    if (!cookie_path_match(q->path, c->path)) return 0;
    if (c->secure && !q->secure) return 0;
    if (c->http_only && !q->http_api) return 0;
    return 1;
}

static void phase_cookies(int iters)
{
    static const char *const frag[] = {
        "a=1", "sid=abc", "=v", "novalue", "n=", "  sp  =  v  ",
        "; Domain=example.com", "; Domain=.example.com", "; Domain=com",
        "; Domain=co.uk", "; Domain=evil.com", "; Domain=", "; Domain=.",
        "; Path=/", "; Path=/a", "; Path=nope", "; Path=",
        "; Secure", "; HttpOnly", "; SameSite=None", "; SameSite=Lax",
        "; SameSite=Strict", "; SameSite=", "; Max-Age=100", "; Max-Age=0",
        "; Max-Age=-1", "; Max-Age=abc", "; Max-Age=99999999999999999999",
        "; Expires=Wed, 21 Oct 2015 07:28:00 GMT", "; Expires=garbage",
        "; Expires=", ";;;", "; ", "\r\nEvil: 1", "\r\n", "\x01", "\x7f",
        "; Priority=High", "; Partitioned", "=", ";"
    };
    struct cookie_jar j;
    cookie_jar_init(&j);

    for (int it = 0; it < iters; it++) {
        char sc[512];
        int o = 0;
        int nf = 1 + (int)rnd_n(6);
        for (int i = 0; i < nf && o < (int)sizeof sc - 80; i++) {
            const char *f = frag[rnd_n(41)];
            int l = (int)strlen(f);
            memcpy(sc + o, f, (size_t)l); o += l;
        }
        sc[o] = 0;
        if (rnd_n(8) == 0) {                   /* occasionally corrupt a byte */
            if (o) sc[rnd_n((uint32_t)o)] = (char)rnd_n(256);
        }

        struct cookie_ctx ctx;
        ctx.host = hosts[rnd_n(12)];
        ctx.path = paths[rnd_n(7)];
        ctx.secure = (int)rnd_n(2);
        ctx.http_api = (int)rnd_n(2);
        cookie_set(&j, &ctx, sc, 1700000000 + (int64_t)rnd_n(1000));

        /* P5: every cookie that comes back out must be one the RFC rules allow
         * for that host, path and scheme. Checked token by token against the
         * jar rather than by searching for a cookie's text in the output --
         * two jar entries can serialize to the same "name=value", so a text
         * search would credit a leak to the wrong (innocent) entry. */
        for (int h = 0; h < 12; h++) {
            char buf[4096];
            struct cookie_ctx q;
            q.host = hosts[h]; q.path = "/a/b"; q.secure = (int)rnd_n(2); q.http_api = 1;
            int n = cookie_header(&j, &q, 1700000000, buf, (int)sizeof buf);
            REQUIRE(n >= 0 && n < (int)sizeof buf, "cookie_header returned %d", n);
            if (n <= 0) continue;

            int allowed = 0;
            for (int k = 0; k < j.n; k++) if (fz_applicable(&j.v[k], &q, 1700000000)) allowed++;

            int tokens = 0;
            for (char *tok = buf; tok && *tok; ) {
                char *sep = strstr(tok, "; ");
                int tl = sep ? (int)(sep - tok) : (int)strlen(tok);
                int matched = 0;
                for (int k = 0; k < j.n && !matched; k++) {
                    char nv[600];
                    int nvl = snprintf(nv, sizeof nv, "%s=%s", j.v[k].name, j.v[k].value);
                    if (nvl == tl && !memcmp(nv, tok, (size_t)tl) &&
                        fz_applicable(&j.v[k], &q, 1700000000)) matched = 1;
                }
                REQUIRE(matched, "sent a cookie to %s (secure=%d) that no rule allows: %.*s",
                        q.host, q.secure, tl, tok);
                tokens++;
                tok = sep ? sep + 2 : NULL;
            }
            /* And nothing that should have been sent was silently dropped
             * (only meaningful when the output buffer was not the limit). */
            if (n < (int)sizeof buf - 600)
                REQUIRE(tokens == allowed, "sent %d cookies to %s, %d were applicable",
                        tokens, q.host, allowed);
        }
        if (rnd_n(64) == 0) cookie_jar_gc(&j, 1700000000 + (int64_t)rnd_n(100000));
        REQUIRE(cookie_jar_count(&j) <= j.max_total, "jar over cap: %d", cookie_jar_count(&j));
    }
    cookie_jar_free(&j);
    printf("ok   phase 8: %d fuzzed Set-Cookie headers, no cross-host leak\n", iters);
}

static void phase_cookie_dates(int iters)
{
    static const char *const bits[] = {
        "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun", "Jan", "Feb", "Mar",
        "Dec", "1", "12", "31", "99", "1970", "2015", "2038", "9999", "70",
        "00", "07:28:00", "25:99:99", "0:0:0", ":", ",", " ", "-", "GMT", "UTC",
        "", "999999999999999", "\xff", "+"
    };
    for (int it = 0; it < iters; it++) {
        char d[160];
        int o = 0, n = (int)rnd_n(10);
        for (int i = 0; i < n && o < (int)sizeof d - 20; i++) {
            const char *b = bits[rnd_n(34)];
            int l = (int)strlen(b);
            memcpy(d + o, b, (size_t)l); o += l;
            if (rnd_n(2)) d[o++] = " ,-:/"[rnd_n(5)];
        }
        d[o] = 0;
        int64_t t = cookie_parse_date(d);
        /* A date either fails (0) or lands in a sane range -- an Expires the
         * jar misreads as the year 30000 is a cookie that never expires. */
        REQUIRE(t == 0 || (t > -12000000000LL && t < 260000000000LL),
                "date '%s' -> %lld", d, (long long)t);
    }
    printf("ok   phase 9: %d fuzzed cookie-dates in range or rejected\n", iters);
}

/* ----------------------------------------------------------------- pool */

static int inuse_closed;
static int fz_watching;                 /* 1 while inside a non-caller-initiated call */
static struct hpool *fz_pool;
/* The pool may close an in-use connection when the CALLER asks (release with
 * keep-alive off, drop, close_all). What it must never do is close one behind
 * the caller's back, to make room or to expire an idle slot -- that would pull
 * the socket out from under a request in flight. So the check is armed only
 * around acquire/may_open/expire. */
static void fz_close(int fd, void *ctx, void *user)
{
    (void)ctx; (void)user;
    if (!fz_watching) return;
    for (int i = 0; i < HP_MAX_CONNS; i++)
        if (fz_pool->v[i].used && fz_pool->v[i].fd == fd && fz_pool->v[i].in_use)
            inuse_closed++;
}

static void phase_pool(int iters)
{
    struct hpool p;
    hpool_init(&p);
    hpool_config(&p, 1 + (int)rnd_n(HP_MAX_CONNS), 1 + (int)rnd_n(6), 1000);
    hpool_set_closer(&p, fz_close, NULL);
    fz_pool = &p; inuse_closed = 0;

    int held[HP_MAX_CONNS], nheld = 0, fd = 1;
    int64_t now = 0;
    for (int it = 0; it < iters; it++) {
        now += (int64_t)rnd_n(400);
        const char *h = hosts[rnd_n(12)];
        int port = (int)(80 + rnd_n(3) * 363);
        int tls = (int)rnd_n(2);
        switch (rnd_n(6)) {
        case 0: case 1: {
            fz_watching = 1;
            int s = hpool_acquire(&p, h, port, tls, now);
            if (s < 0 && hpool_may_open(&p, h, port, tls, now))
                s = hpool_admit(&p, h, port, tls, fd++, NULL, now);
            fz_watching = 0;
            if (s >= 0 && nheld < HP_MAX_CONNS) held[nheld++] = s;
            break;
        }
        case 2: case 3:
            if (nheld) {
                int k = (int)rnd_n((uint32_t)nheld);
                hpool_release(&p, held[k], (int)rnd_n(2), now);
                held[k] = held[--nheld];
            }
            break;
        case 4:
            if (nheld) {
                int k = (int)rnd_n((uint32_t)nheld);
                hpool_drop(&p, held[k]);
                held[k] = held[--nheld];
            }
            break;
        case 5:
            fz_watching = 1;
            hpool_expire(&p, now);
            fz_watching = 0;
            break;
        }
        /* P6: the caps hold at every step, not just at the end. */
        REQUIRE(hpool_count(&p) <= p.max_total, "pool over total cap: %d > %d",
                hpool_count(&p), p.max_total);
        for (int k = 0; k < 12; k++)
            REQUIRE(hpool_count_origin(&p, hosts[k], port, tls) <= p.max_per_host,
                    "origin over per-host cap");
    }
    hpool_close_all(&p);
    REQUIRE(inuse_closed == 0, "%d in-use connections were closed under the caller", inuse_closed);
    REQUIRE(hpool_count(&p) == 0, "close_all left %d", hpool_count(&p));
    printf("ok   phase 10: %d random pool operations, caps held, no in-use close\n", iters);
}

/* -------------------------------------------------- transport-level fuzz */

struct fzstream { const uint8_t *p; int n, off; int chunk; };
static int fz_read(void *ctx, void *buf, int len)
{
    struct fzstream *s = (struct fzstream *)ctx;
    if (s->off >= s->n) return H1_EOF;
    if (rnd_n(4) == 0) return H1_AGAIN;
    int take = s->n - s->off;
    if (take > s->chunk) take = s->chunk;
    if (take > len) take = len;
    memcpy(buf, s->p + s->off, (size_t)take);
    s->off += take;
    return take;
}
static int fz_write(void *ctx, const void *buf, int len)
{
    (void)ctx; (void)buf;
    return rnd_n(3) == 0 ? H1_AGAIN : (int)(1 + rnd_n((uint32_t)len));  /* partial writes */
}

static void phase_conn(int iters)
{
    for (int it = 0; it < iters; it++) {
        const char *seed = seeds[rnd_n(12)];
        int n = (int)strlen(seed);
        if (rnd_n(3) == 0) n = 1 + (int)rnd_n((uint32_t)n);
        uint8_t *b = (uint8_t *)malloc((size_t)n + 1);
        memcpy(b, seed, (size_t)n);
        if (rnd_n(2)) mutate(b, n);

        struct fzstream st = { b, n, 0, 1 + (int)rnd_n(64) };
        struct h1_transport t;
        t.read = fz_read; t.write = fz_write; t.poll = NULL; t.ctx = &st;
        struct h1_conn c;
        char *req = (char *)malloc(64);
        int rl = sprintf(req, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        if (h1_conn_start(&c, &t, req, rl) == H1_OK) {
            int guard = 0;
            while (h1_conn_pump(&c) < H1_C_DONE && ++guard < 200000) { }
            REQUIRE(guard < 200000, "pump did not terminate");
            REQUIRE(c.spill_len >= 0 && c.spill_len <= (int)sizeof c.spill, "spill %d", c.spill_len);
            h1_conn_free(&c);
        } else {
            free(req);
        }
        free(b);
    }
    printf("ok   phase 11: %d exchanges over a flaky transport (partial writes, EAGAIN)\n", iters);
}

int main(int argc, char **argv)
{
    int scale = 1;
    if (argc > 1) scale = atoi(argv[1]);
    if (scale < 1) scale = 1;
    if (argc > 2) rng_s = (uint64_t)strtoull(argv[2], NULL, 0) | 1;
    uint64_t seed = rng_s;              /* echoed at the end so a failure replays */

    phase_mutate_seeds(20000 * scale);
    phase_truncations();
    phase_random_bytes(20000 * scale);
    phase_numbers();
    phase_header_bombs();
    phase_request_injection(20000 * scale);
    phase_decode(5000 * scale);
    phase_cookies(3000 * scale);
    phase_cookie_dates(20000 * scale);
    phase_pool(20000 * scale);
    phase_conn(5000 * scale);

    if (fails) {
        printf("\n%d FAILURES -- replay with: make test-http-fuzz SCALE=%d SEED=0x%llx\n",
               fails, scale, (unsigned long long)seed);
        return 1;
    }
    printf("\nHTTP1 FUZZ DONE (scale %d, seed 0x%llx)\n", scale, (unsigned long long)seed);
    return 0;
}
