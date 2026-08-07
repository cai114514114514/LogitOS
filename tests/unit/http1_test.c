/* Host unit test for c/lib/http/http1.c -- the ring-3 HTTP/1.1 client.
 *
 * The kernel's c/net/http/http.c has no test at all, which is remarkable for
 * the one component that parses attacker-chosen bytes in ring 0. This file is
 * the replacement's counter-argument. It never opens a socket: http1.c takes
 * its transport as a vtable, so every case below is a byte string fed to a
 * state machine, including the ones a real network could only produce by
 * accident once in a thousand loads (a header split mid-name, a chunked body
 * arriving one byte at a time, two responses in one read).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "http1.h"

/* http1.c decodes gzip/deflate through the same safe-Rust inflater the PNG
 * path uses, so the test links rust/. That staticlib also carries png.rs,
 * which references the kernel allocator -- these stubs exist only to satisfy
 * the linker (same shape as tests/unit/png_test.c) and are never called. */
void *kmalloc(unsigned long n) { return malloc((size_t)n); }
void  kfree(void *p) { free(p); }
int   img_register(void *d) { (void)d; return 0; }

static int fails;
#define OK(cond) do { if (cond) printf("ok   %s\n", #cond); \
                      else { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

/* Feed `s` (length n) to r in `step`-byte pieces. step <= 0 means all at once.
 * Returns the total consumed, or the first negative error. */
static int feed_steps(struct h1_response *r, const void *s, int n, int step)
{
    const uint8_t *p = (const uint8_t *)s;
    int off = 0;
    if (step <= 0) step = n ? n : 1;
    while (off < n) {
        int take = n - off < step ? n - off : step;
        int used = h1_response_feed(r, p + off, take);
        if (used < 0) return used;
        off += used;
        if (used < take) break;            /* message ended inside the buffer */
    }
    return off;
}

/* Parse a whole wire message at a given granularity; returns the response. */
static void parse_at(struct h1_response *r, const char *wire, int step, int eof)
{
    h1_response_init(r);
    int rc = feed_steps(r, wire, (int)strlen(wire), step);
    if (rc >= 0 && eof) h1_response_eof(r);
}

static int body_is(const struct h1_response *r, const char *s)
{
    int n = (int)strlen(s);
    return r->body_len == n && (n == 0 || memcmp(r->body, s, (size_t)n) == 0);
}

/* ---------------------------------------------------------------- basics */

static void t_content_length(void)
{
    const char *w = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 11\r\n\r\nhello world";
    /* Every split point, not just the convenient one. A header boundary that
     * lands mid-"Content-Len" is exactly the case a naive parser gets wrong,
     * and on a real network it happens whenever the response crosses an MSS. */
    for (int step = 1; step <= (int)strlen(w); step++) {
        struct h1_response r;
        parse_at(&r, w, step, 0);
        if (!(r.state == H1_ST_DONE && r.code == 200 && body_is(&r, "hello world"))) {
            printf("FAIL content-length at step=%d (state=%d code=%d len=%d)\n",
                   step, r.state, r.code, r.body_len);
            fails++;
            h1_response_free(&r);
            return;
        }
        h1_response_free(&r);
    }
    printf("ok   content-length parses identically at every split point\n");

    struct h1_response r;
    parse_at(&r, w, 0, 0);
    OK(r.code == 200);
    OK(!strcmp(r.reason, "OK"));
    OK(r.minor == 1);
    OK(!strcmp(h1_headers_get(&r.hdr, "content-type"), "text/html"));
    OK(!strcmp(h1_headers_get(&r.hdr, "CONTENT-TYPE"), "text/html"));   /* case-insensitive */
    OK(h1_headers_get(&r.hdr, "x-absent") == NULL);
    OK(r.keep_alive == 1);
    h1_response_free(&r);
}

static void t_status_line(void)
{
    struct h1_response r;
    parse_at(&r, "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n", 0, 0);
    OK(r.code == 404 && r.minor == 0 && !strcmp(r.reason, "Not Found"));
    OK(r.state == H1_ST_DONE);
    OK(r.keep_alive == 0);                          /* HTTP/1.0 defaults to close */
    h1_response_free(&r);

    parse_at(&r, "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n", 0, 0);
    OK(r.keep_alive == 1);                          /* ...unless it opts in */
    h1_response_free(&r);

    parse_at(&r, "HTTP/1.1 204 \r\n\r\n", 0, 0);
    OK(r.code == 204 && r.state == H1_ST_DONE && r.reason[0] == 0);
    h1_response_free(&r);

    /* Malformed status lines are rejected, not guessed at. A response that is
     * not HTTP is a mis-dialled port or a captive portal, and pretending it is
     * HTTP/0.9 renders a login page as the site. */
    const char *bad[] = {
        "ICY 200 OK\r\n\r\n",
        "HTTP/1.1 20 OK\r\n\r\n",
        "HTTP/1.1 2000 OK\r\n\r\n",
        "HTTP/1.1200 OK\r\n\r\n",
        "HTTP/2.0 200 OK\r\n\r\n",
        "HTTP/1.x 200 OK\r\n\r\n",
        "HTTP/1.1 abc OK\r\n\r\n",
        "<html>oops</html>\r\n\r\n",
        "\r\n",
        NULL
    };
    for (int i = 0; bad[i]; i++) {
        parse_at(&r, bad[i], 0, 1);
        if (r.state != H1_ST_ERROR) { printf("FAIL accepted bad status: %s", bad[i]); fails++; }
        h1_response_free(&r);
    }
    printf("ok   %d malformed status lines all rejected\n", 9);
}

/* ---------------------------------------------------------------- headers */

static void t_headers(void)
{
    struct h1_response r;

    /* Duplicates are kept, in order. Set-Cookie is the reason this matters:
     * a login response sends several and a "last one wins" map loses all but
     * one of them. */
    parse_at(&r, "HTTP/1.1 200 OK\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\n"
                 "Content-Length: 0\r\n\r\n", 0, 0);
    OK(h1_headers_count(&r.hdr, "set-cookie") == 2);
    OK(!strcmp(h1_headers_nth(&r.hdr, "set-cookie", 0), "a=1"));
    OK(!strcmp(h1_headers_nth(&r.hdr, "set-cookie", 1), "b=2"));
    OK(!strcmp(h1_headers_get(&r.hdr, "set-cookie"), "a=1"));   /* get == first */
    h1_response_free(&r);

    /* obs-fold: deprecated, still emitted by old servers. */
    parse_at(&r, "HTTP/1.1 200 OK\r\nX-Long: one\r\n  two\r\n\ttwo-and-a-half\r\n"
                 "Content-Length: 0\r\n\r\n", 0, 0);
    OK(r.state == H1_ST_DONE);
    OK(!strcmp(h1_headers_get(&r.hdr, "x-long"), "one two two-and-a-half"));
    h1_response_free(&r);

    /* A fold before any header has nothing to continue. */
    parse_at(&r, "HTTP/1.1 200 OK\r\n  orphan\r\n\r\n", 0, 1);
    OK(r.state == H1_ST_ERROR && r.err == H1_E_SYNTAX);
    h1_response_free(&r);

    /* Whitespace before the colon: RFC 9112 calls this out by name because it
     * is how a header gets interpreted differently by us and by a proxy. */
    parse_at(&r, "HTTP/1.1 200 OK\r\nContent-Length : 5\r\n\r\nhello", 0, 1);
    OK(r.state == H1_ST_ERROR);
    h1_response_free(&r);

    parse_at(&r, "HTTP/1.1 200 OK\r\nno-colon-here\r\n\r\n", 0, 1);
    OK(r.state == H1_ST_ERROR);
    h1_response_free(&r);

    parse_at(&r, "HTTP/1.1 200 OK\r\n: empty-name\r\n\r\n", 0, 1);
    OK(r.state == H1_ST_ERROR);
    h1_response_free(&r);

    /* Values are trimmed of optional whitespace, tabs included. */
    parse_at(&r, "HTTP/1.1 200 OK\r\nX-A:\t  spaced \t\r\nContent-Length: 0\r\n\r\n", 0, 0);
    OK(!strcmp(h1_headers_get(&r.hdr, "x-a"), "spaced"));
    h1_response_free(&r);

    /* An empty value is legal. */
    parse_at(&r, "HTTP/1.1 200 OK\r\nX-Empty:\r\nContent-Length: 0\r\n\r\n", 0, 0);
    OK(h1_headers_get(&r.hdr, "x-empty") && !strcmp(h1_headers_get(&r.hdr, "x-empty"), ""));
    h1_response_free(&r);

    /* Bare LF as a line terminator: tolerated (RFC 9112 permits recognizing
     * it), while a bare CR *inside* a value is rejected -- that is injection. */
    parse_at(&r, "HTTP/1.1 200 OK\nX-A: b\nContent-Length: 2\n\nhi", 0, 0);
    OK(r.state == H1_ST_DONE && body_is(&r, "hi") && !strcmp(h1_headers_get(&r.hdr, "x-a"), "b"));
    h1_response_free(&r);

    parse_at(&r, "HTTP/1.1 200 OK\r\nX-A: b\rEvil: yes\r\nContent-Length: 0\r\n\r\n", 0, 1);
    OK(r.state == H1_ST_ERROR && r.err == H1_E_SYNTAX);
    h1_response_free(&r);

    /* Too many headers, and one header longer than the line limit. */
    {
        char *big = (char *)malloc(400 * 40 + 128);
        int o = sprintf(big, "HTTP/1.1 200 OK\r\n");
        for (int i = 0; i < 400; i++) o += sprintf(big + o, "X-%d: v\r\n", i);
        sprintf(big + o, "\r\n");
        parse_at(&r, big, 0, 1);
        OK(r.state == H1_ST_ERROR && r.err == H1_E_TOOLARGE);
        h1_response_free(&r);
        free(big);
    }
    {
        char *big = (char *)malloc(H1_LINE_MAX + 4096);
        int o = sprintf(big, "HTTP/1.1 200 OK\r\nX-Big: ");
        for (int i = 0; i < H1_LINE_MAX + 100; i++) big[o++] = 'a';
        o += sprintf(big + o, "\r\n\r\n");
        big[o] = 0;
        parse_at(&r, big, 0, 1);
        OK(r.state == H1_ST_ERROR && r.err == H1_E_TOOLARGE);
        h1_response_free(&r);
        free(big);
    }
}

/* ---------------------------------------------------------------- framing */

static void t_content_length_conflicts(void)
{
    struct h1_response r;

    /* Two Content-Lengths that disagree is the textbook request-smuggling
     * primitive. Picking one is not an option; the message is malformed. */
    parse_at(&r, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!", 0, 1);
    OK(r.state == H1_ST_ERROR && r.err == H1_E_SYNTAX);
    h1_response_free(&r);

    /* Two that agree, and the comma-list spelling of the same thing, are fine. */
    parse_at(&r, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello", 0, 0);
    OK(r.state == H1_ST_DONE && body_is(&r, "hello"));
    h1_response_free(&r);

    parse_at(&r, "HTTP/1.1 200 OK\r\nContent-Length: 5, 5\r\n\r\nhello", 0, 0);
    OK(r.state == H1_ST_DONE && body_is(&r, "hello"));
    h1_response_free(&r);

    parse_at(&r, "HTTP/1.1 200 OK\r\nContent-Length: 5, 6\r\n\r\nhello!", 0, 1);
    OK(r.state == H1_ST_ERROR);
    h1_response_free(&r);

    const char *bad[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: +5\r\n\r\nhello",
        "HTTP/1.1 200 OK\r\nContent-Length: 0x10\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: \r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 5abc\r\n\r\nhello",
        "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n",
        NULL
    };
    for (int i = 0; bad[i]; i++) {
        parse_at(&r, bad[i], 0, 1);
        if (r.state != H1_ST_ERROR) { printf("FAIL accepted bad CL #%d\n", i); fails++; }
        h1_response_free(&r);
    }
    printf("ok   6 malformed Content-Lengths all rejected\n");

    /* Plausible but larger than we will hold: refused before a byte is
     * allocated, so the peer never picks our allocation size. */
    parse_at(&r, "HTTP/1.1 200 OK\r\nContent-Length: 4294967290\r\n\r\n", 0, 1);
    OK(r.state == H1_ST_ERROR && r.err == H1_E_TOOLARGE);
    h1_response_free(&r);

    /* TE and CL together: TE frames the message, and the connection must not
     * be reused afterwards. */
    parse_at(&r, "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "5\r\nhello\r\n0\r\n\r\n", 0, 0);
    OK(r.state == H1_ST_DONE && body_is(&r, "hello"));
    OK(r.must_close == 1 && r.keep_alive == 0);
    h1_response_free(&r);
}

static void t_no_body(void)
{
    struct h1_response r;

    /* 204 and 304 never have a body even when they claim one. Reading the
     * declared bytes would eat the next response off a pooled connection. */
    parse_at(&r, "HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\n", 0, 0);
    OK(r.state == H1_ST_DONE && r.body_len == 0 && r.no_body);
    h1_response_free(&r);

    parse_at(&r, "HTTP/1.1 304 Not Modified\r\nContent-Length: 1234\r\n\r\n", 0, 0);
    OK(r.state == H1_ST_DONE && r.body_len == 0);
    h1_response_free(&r);

    /* HEAD: the parser cannot know from the response alone; the caller says so. */
    h1_response_init(&r);
    h1_response_head(&r, 1);
    const char *w = "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n";
    OK(h1_response_feed(&r, w, (int)strlen(w)) == (int)strlen(w));
    OK(r.state == H1_ST_DONE && r.body_len == 0 && r.clen == 1234);
    h1_response_free(&r);
}

static void t_eof_delimited(void)
{
    struct h1_response r;

    /* No Content-Length, no chunked: the body is whatever arrives until the
     * peer closes. Legal, and the reason h1_response_eof exists. */
    h1_response_init(&r);
    const char *w = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nthe body";
    h1_response_feed(&r, w, (int)strlen(w));
    OK(r.state == H1_ST_BODY_EOF);
    OK(h1_response_eof(&r) == H1_OK);
    OK(r.state == H1_ST_DONE && body_is(&r, "the body"));
    OK(r.keep_alive == 0 && r.must_close == 1);
    h1_response_free(&r);

    /* Truncation must be an ERROR, not a short body: a page cut off by a
     * dropped connection would otherwise render as a silently partial one. */
    h1_response_init(&r);
    const char *t = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly ten b";
    h1_response_feed(&r, t, (int)strlen(t));
    OK(r.state == H1_ST_BODY_LEN && r.remain == 90);
    OK(h1_response_eof(&r) == H1_E_TRUNC);
    h1_response_free(&r);

    /* Truncated mid-headers. */
    h1_response_init(&r);
    const char *h = "HTTP/1.1 200 OK\r\nContent-Len";
    h1_response_feed(&r, h, (int)strlen(h));
    OK(h1_response_eof(&r) == H1_E_TRUNC);
    h1_response_free(&r);

    /* Truncated mid-chunk. */
    h1_response_init(&r);
    const char *c = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel";
    h1_response_feed(&r, c, (int)strlen(c));
    OK(h1_response_eof(&r) == H1_E_TRUNC);
    h1_response_free(&r);

    /* Nothing at all. */
    h1_response_init(&r);
    OK(h1_response_eof(&r) == H1_E_TRUNC);
    h1_response_free(&r);
}

/* ---------------------------------------------------------------- chunked */

static void t_chunked(void)
{
    struct h1_response r;
    const char *w =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4\r\nWiki\r\n5\r\npedia\r\nD\r\n in\r\n\r\nchunks\r\n0\r\n\r\n";

    /* One byte at a time is the interesting granularity: every state
     * transition happens on a buffer boundary. */
    for (int step = 1; step <= 8; step++) {
        parse_at(&r, w, step, 0);
        if (!(r.state == H1_ST_DONE && body_is(&r, "Wikipedia in\r\n\r\nchunks"))) {
            printf("FAIL chunked at step=%d state=%d len=%d\n", step, r.state, r.body_len);
            fails++;
        }
        h1_response_free(&r);
    }
    printf("ok   chunked body reassembles at 1..8 bytes per read\n");

    parse_at(&r, w, 0, 0);
    OK(r.state == H1_ST_DONE && body_is(&r, "Wikipedia in\r\n\r\nchunks"));
    OK(r.chunked == 1 && r.keep_alive == 1);
    h1_response_free(&r);

    /* Chunk extensions, uppercase hex, an empty body. */
    parse_at(&r, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "4;name=value;x\r\nWiki\r\nA;q\r\n0123456789\r\n0\r\n\r\n", 0, 0);
    OK(r.state == H1_ST_DONE && body_is(&r, "Wiki0123456789"));
    h1_response_free(&r);

    parse_at(&r, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 0, 0);
    OK(r.state == H1_ST_DONE && r.body_len == 0);
    h1_response_free(&r);

    /* Trailers. The kernel version gives up on these outright (http.c:103
     * "Trailers ... are not recognized"), which turns a perfectly good
     * response into a timeout. */
    parse_at(&r, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "3\r\nabc\r\n0\r\nX-Checksum: deadbeef\r\nX-More: 1\r\n\r\n", 0, 0);
    OK(r.state == H1_ST_DONE && body_is(&r, "abc"));
    OK(r.trailer.n == 2);
    OK(!strcmp(h1_headers_get(&r.trailer, "x-checksum"), "deadbeef"));
    OK(h1_headers_get(&r.hdr, "x-checksum") == NULL);   /* kept out of the headers */
    h1_response_free(&r);

    /* Trailers, one byte at a time. */
    parse_at(&r, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "3\r\nabc\r\n0\r\nX-C: d\r\n\r\n", 1, 0);
    OK(r.state == H1_ST_DONE && body_is(&r, "abc") && r.trailer.n == 1);
    h1_response_free(&r);

    /* Only the LAST transfer coding frames the message. */
    parse_at(&r, "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"
                 "3\r\nabc\r\n0\r\n\r\n", 0, 0);
    OK(r.state == H1_ST_DONE && r.chunked == 1);
    h1_response_free(&r);

    h1_response_init(&r);
    const char *nc = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip\r\n\r\nrawbytes";
    h1_response_feed(&r, nc, (int)strlen(nc));
    OK(r.chunked == 0 && r.state == H1_ST_BODY_EOF);    /* unframed, close-delimited */
    h1_response_free(&r);

    /* Malformed chunk framing. A "negative" size is not a number at all in
     * hex, and 17 hex digits is an overflow attempt, not a big chunk. */
    const char *bad[] = {
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n-5\r\nhello\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFF\r\nx\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nxx\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0x3\r\nabc\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcXX0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3 junk\r\nabc\r\n0\r\n\r\n",
        NULL
    };
    for (int i = 0; bad[i]; i++) {
        parse_at(&r, bad[i], 0, 1);
        if (r.state != H1_ST_ERROR) { printf("FAIL accepted bad chunking #%d\n", i); fails++; }
        h1_response_free(&r);
        parse_at(&r, bad[i], 1, 1);                     /* and byte-at-a-time */
        if (r.state != H1_ST_ERROR) { printf("FAIL accepted bad chunking #%d (step 1)\n", i); fails++; }
        h1_response_free(&r);
    }
    printf("ok   7 malformed chunkings rejected whole and byte-at-a-time\n");

    /* A single chunk larger than the body cap is refused up front. */
    h1_response_init(&r);
    h1_response_limit(&r, 4096);
    const char *huge = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFF\r\n";
    OK(h1_response_feed(&r, huge, (int)strlen(huge)) == H1_E_TOOLARGE);
    h1_response_free(&r);

    /* Many small chunks that together exceed the cap are refused too. */
    h1_response_init(&r);
    h1_response_limit(&r, 64);
    const char *hdr = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    h1_response_feed(&r, hdr, (int)strlen(hdr));
    int over = 0;
    for (int i = 0; i < 20 && !over; i++)
        if (h1_response_feed(&r, "8\r\nabcdefgh\r\n", 13) < 0) over = 1;
    OK(over && r.err == H1_E_TOOLARGE);
    h1_response_free(&r);
}

/* ------------------------------------------------------------ 1xx interim */

static void t_interim(void)
{
    struct h1_response r;

    /* 100-continue: the interim response must be consumed and the real one
     * parsed on the same connection. A parser that stops at the first blank
     * line reports "200 with an empty body" for every upload. */
    const char *w = "HTTP/1.1 100 Continue\r\n\r\n"
                    "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok";
    for (int step = 0; step <= 3; step++) {
        parse_at(&r, w, step, 0);
        if (!(r.state == H1_ST_DONE && r.code == 201 && r.interim == 1 && body_is(&r, "ok"))) {
            printf("FAIL 100-continue at step=%d code=%d interim=%d\n", step, r.code, r.interim);
            fails++;
        }
        h1_response_free(&r);
    }
    printf("ok   100-continue then 201, at 4 granularities\n");

    /* Interim responses carry headers of their own, which must not leak into
     * the final response's header set. */
    parse_at(&r, "HTTP/1.1 103 Early Hints\r\nLink: </s.css>; rel=preload\r\n\r\n"
                 "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx", 0, 0);
    OK(r.code == 200 && h1_headers_get(&r.hdr, "link") == NULL && body_is(&r, "x"));
    h1_response_free(&r);

    /* An endless 1xx stream is a denial of service, so it is bounded. */
    {
        char buf[4096]; int o = 0;
        for (int i = 0; i < 40; i++) o += sprintf(buf + o, "HTTP/1.1 100 Continue\r\n\r\n");
        parse_at(&r, buf, 0, 1);
        OK(r.state == H1_ST_ERROR);
        h1_response_free(&r);
    }

    /* 101 is final, not interim, and ends the connection's HTTP life. */
    parse_at(&r, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n", 0, 0);
    OK(r.state == H1_ST_DONE && r.code == 101 && r.keep_alive == 0);
    h1_response_free(&r);
}

/* ------------------------------------------------------ pipelining/spill */

static void t_message_boundary(void)
{
    /* feed() stops at the end of the message and reports how much it used.
     * That is what lets a pooled connection carry the next response: without
     * it, the leftover bytes are either lost or fed to a finished parser. */
    const char *two = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nAA"
                      "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nBB";
    struct h1_response r;
    h1_response_init(&r);
    int used = h1_response_feed(&r, two, (int)strlen(two));
    OK(used > 0 && used < (int)strlen(two));
    OK(r.state == H1_ST_DONE && body_is(&r, "AA"));

    struct h1_response r2;
    h1_response_init(&r2);
    int used2 = h1_response_feed(&r2, two + used, (int)strlen(two) - used);
    OK(used2 == (int)strlen(two) - used);
    OK(r2.state == H1_ST_DONE && body_is(&r2, "BB"));
    h1_response_free(&r);
    h1_response_free(&r2);

    /* reset() recycles the parser for the next message on the same socket. */
    h1_response_init(&r);
    h1_response_feed(&r, two, (int)strlen(two));
    h1_response_reset(&r);
    OK(r.state == H1_ST_STATUS && r.body_len == 0 && r.hdr.n == 0 && r.code == 0);
    h1_response_feed(&r, two + used, (int)strlen(two) - used);
    OK(r.state == H1_ST_DONE && body_is(&r, "BB"));
    h1_response_free(&r);
}

/* -------------------------------------------------------- request building */

static void t_request(void)
{
    struct h1_request q;
    char *out = NULL; int n = 0;

    OK(h1_request_init(&q, "GET", "/index.html") == H1_OK);
    OK(h1_request_set_header(&q, "Host", "example.com") == H1_OK);
    OK(h1_request_set_header(&q, "Accept-Encoding", h1_accept_encoding()) == H1_OK);
    /* The thing the kernel client structurally cannot do. */
    OK(h1_request_set_header(&q, "Cookie", "sid=abc; theme=dark") == H1_OK);
    OK(h1_request_build(&q, &out, &n) == H1_OK);
    OK(!strcmp(out,
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Accept-Encoding: gzip, deflate\r\n"
        "Cookie: sid=abc; theme=dark\r\n\r\n"));
    free(out);
    h1_request_free(&q);

    /* A body, with Content-Length supplied automatically. */
    OK(h1_request_init(&q, "POST", "/api") == H1_OK);
    h1_request_set_header(&q, "Host", "example.com");
    h1_request_set_header(&q, "Content-Type", "application/json");
    h1_request_set_body(&q, "{\"a\":1}", 7);
    OK(h1_request_build(&q, &out, &n) == H1_OK);
    OK(strstr(out, "POST /api HTTP/1.1\r\n") == out);
    OK(strstr(out, "Content-Length: 7\r\n") != NULL);
    OK(strstr(out, "\r\n\r\n{\"a\":1}") != NULL);
    OK(n == (int)strlen(out));
    free(out);
    h1_request_free(&q);

    /* set replaces, add duplicates. */
    OK(h1_request_init(&q, "GET", "/") == H1_OK);
    h1_request_add_header(&q, "X-A", "1");
    h1_request_add_header(&q, "X-A", "2");
    OK(h1_headers_count(&q.hdr, "x-a") == 2);
    h1_request_set_header(&q, "X-A", "3");
    OK(h1_headers_count(&q.hdr, "x-a") == 1);
    OK(!strcmp(h1_headers_get(&q.hdr, "x-a"), "3"));
    h1_request_free(&q);

    /* HEADER INJECTION. A cookie value or a redirect target carrying CRLF must
     * never be able to append a header or a second request. This is refused at
     * the setter, so a caller cannot build the bad request in the first place. */
    OK(h1_request_init(&q, "GET", "/") == H1_OK);
    OK(h1_request_set_header(&q, "X-A", "v\r\nX-Evil: yes") == H1_E_ARG);
    OK(h1_request_set_header(&q, "X-A", "v\rX-Evil: yes") == H1_E_ARG);
    OK(h1_request_set_header(&q, "X-A", "v\nX-Evil: yes") == H1_E_ARG);
    OK(h1_request_set_header(&q, "X-A\r\nX-Evil: yes", "v") == H1_E_ARG);
    OK(h1_request_set_header(&q, "X A", "v") == H1_E_ARG);      /* space is not a token char */
    OK(h1_request_set_header(&q, "X:A", "v") == H1_E_ARG);
    OK(h1_request_set_header(&q, "", "v") == H1_E_ARG);
    OK(q.hdr.n == 0);                                           /* nothing got through */
    /* And in the request target. */
    OK(h1_request_set_target(&q, "/a b") == H1_E_ARG);
    OK(h1_request_set_target(&q, "/a\r\nGET /evil HTTP/1.1") == H1_E_ARG);
    OK(!strcmp(q.target, "/"));
    h1_request_free(&q);

    OK(h1_request_init(&q, "GE T", "/") == H1_E_ARG);
    h1_request_free(&q);
    OK(h1_request_init(&q, "GET", "") == H1_E_ARG);
    h1_request_free(&q);
    OK(h1_request_init(&q, "VERYLONGMETHODNAMEHERE", "/") == H1_E_ARG);
    h1_request_free(&q);
}

static void t_redirect(void)
{
    OK(h1_is_redirect(301) && h1_is_redirect(302) && h1_is_redirect(303) &&
       h1_is_redirect(307) && h1_is_redirect(308));
    OK(!h1_is_redirect(200) && !h1_is_redirect(304) && !h1_is_redirect(300) &&
       !h1_is_redirect(305) && !h1_is_redirect(399));

    char m[H1_METHOD_MAX]; int drop = -1;
    OK(h1_redirect_method(303, "POST", m, sizeof m, &drop) == H1_OK && !strcmp(m, "GET") && drop == 1);
    OK(h1_redirect_method(302, "POST", m, sizeof m, &drop) == H1_OK && !strcmp(m, "GET") && drop == 1);
    OK(h1_redirect_method(307, "POST", m, sizeof m, &drop) == H1_OK && !strcmp(m, "POST") && drop == 0);
    OK(h1_redirect_method(308, "PUT", m, sizeof m, &drop) == H1_OK && !strcmp(m, "PUT") && drop == 0);
    OK(h1_redirect_method(301, "HEAD", m, sizeof m, &drop) == H1_OK && !strcmp(m, "HEAD") && drop == 0);
    OK(h1_redirect_method(301, "GET", m, sizeof m, &drop) == H1_OK && !strcmp(m, "GET"));
}

/* -------------------------------------------------- transport + redirects */

/* A scripted in-memory transport: it hands out a fixed script in `chunk`-byte
 * pieces, then reports EOF. This is the whole reason http1.c takes a vtable --
 * a redirect chain becomes a string, not a network. */
struct memtr {
    const char *script[8];
    int nscript, cur, off;
    int chunk;
    char sent[8][2048];
    int  nsent;
    int  sent_len;
};

static int mem_read(void *ctx, void *buf, int len)
{
    struct memtr *m = (struct memtr *)ctx;
    if (m->cur >= m->nscript) return H1_EOF;
    const char *s = m->script[m->cur];
    int total = (int)strlen(s);
    if (m->off >= total) return H1_EOF;
    int take = total - m->off;
    if (m->chunk > 0 && take > m->chunk) take = m->chunk;
    if (take > len) take = len;
    memcpy(buf, s + m->off, (size_t)take);
    m->off += take;
    return take;
}

static int mem_write(void *ctx, const void *buf, int len)
{
    struct memtr *m = (struct memtr *)ctx;
    if (m->nsent < 8) {
        int room = (int)sizeof m->sent[0] - 1 - m->sent_len;
        int take = len < room ? len : room;
        memcpy(m->sent[m->nsent] + m->sent_len, buf, (size_t)take);
        m->sent_len += take;
        m->sent[m->nsent][m->sent_len] = 0;
    }
    return len;
}

static void t_conn_and_redirect_chain(void)
{
    struct memtr m;
    memset(&m, 0, sizeof m);
    m.script[0] = "HTTP/1.1 301 Moved\r\nLocation: /two\r\nContent-Length: 0\r\n\r\n";
    m.script[1] = "HTTP/1.1 302 Found\r\nLocation: /three\r\nContent-Length: 0\r\n\r\n";
    m.script[2] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone";
    m.nscript = 3;
    m.chunk = 7;                     /* awkward on purpose */

    struct h1_transport t;
    t.read = mem_read; t.write = mem_write; t.poll = NULL; t.ctx = &m;

    char target[64];
    strcpy(target, "/one");
    char method[H1_METHOD_MAX];
    strcpy(method, "POST");
    int hops = 0, final_code = 0;

    for (; hops < 5; hops++) {
        struct h1_request q;
        char *req = NULL; int reqlen = 0;
        OK(h1_request_init(&q, method, target) == H1_OK);
        h1_request_set_header(&q, "Host", "example.com");
        OK(h1_request_build(&q, &req, &reqlen) == H1_OK);
        h1_request_free(&q);

        struct h1_conn c;
        OK(h1_conn_start(&c, &t, req, reqlen) == H1_OK);
        int guard = 0;
        while (h1_conn_pump(&c) < H1_C_DONE && ++guard < 10000) { }
        OK(c.state == H1_C_DONE);
        final_code = c.resp.code;
        if (h1_is_redirect(final_code)) {
            const char *loc = h1_headers_get(&c.resp.hdr, "location");
            OK(loc != NULL);
            int drop = 0;
            h1_redirect_method(final_code, method, method, sizeof method, &drop);
            strcpy(target, loc ? loc : "/");
            h1_conn_free(&c);
            m.cur++; m.off = 0; m.nsent++; m.sent_len = 0;
            continue;
        }
        OK(body_is(&c.resp, "done"));
        h1_conn_free(&c);
        break;
    }
    OK(hops == 2 && final_code == 200);
    /* 301 after a POST becomes a GET, as every deployed browser does. */
    OK(!strcmp(method, "GET"));
    OK(strstr(m.sent[0], "POST /one HTTP/1.1\r\n") == m.sent[0]);
    OK(strstr(m.sent[1], "GET /two HTTP/1.1\r\n") == m.sent[1]);
    OK(strstr(m.sent[2], "GET /three HTTP/1.1\r\n") == m.sent[2]);
}

static int agn_calls;
static int agn_read(void *ctx, void *buf, int len)
{
    /* Return H1_AGAIN most of the time: a non-blocking parser must make
     * progress across many empty polls without losing state. */
    if (++agn_calls % 4) return H1_AGAIN;
    return mem_read(ctx, buf, len);
}

static void t_conn_would_block(void)
{
    struct memtr m;
    memset(&m, 0, sizeof m);
    m.script[0] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
    m.nscript = 1; m.chunk = 1;
    agn_calls = 0;

    struct h1_transport t;
    t.read = agn_read; t.write = mem_write; t.poll = NULL; t.ctx = &m;
    struct h1_conn c;
    char *req = strdup("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    OK(h1_conn_start(&c, &t, req, (int)strlen(req)) == H1_OK);
    int guard = 0;
    while (h1_conn_pump(&c) < H1_C_DONE && ++guard < 100000) { }
    OK(c.state == H1_C_DONE && body_is(&c.resp, "abc"));
    h1_conn_free(&c);
}

static int dead_read(void *ctx, void *buf, int len) { (void)ctx; (void)buf; (void)len; return H1_TERR; }
static int dead_write(void *ctx, const void *buf, int len) { (void)ctx; (void)buf; (void)len; return H1_TERR; }

static void t_conn_transport_error(void)
{
    struct h1_transport t;
    t.read = dead_read; t.write = dead_write; t.poll = NULL; t.ctx = NULL;
    struct h1_conn c;
    char *req = strdup("GET / HTTP/1.1\r\n\r\n");
    OK(h1_conn_start(&c, &t, req, (int)strlen(req)) == H1_OK);
    OK(h1_conn_pump(&c) == H1_C_ERROR && c.err == H1_E_TRANSPORT);
    h1_conn_free(&c);
}

/* --------------------------------------------------- content-encoding ---- */

/* A DEFLATE "stored" block (RFC 1951 3.2.4) is a valid deflate stream that
 * needs no compressor to produce: BFINAL|BTYPE=00, pad to a byte, LEN, ~LEN,
 * raw data. That lets these tests exercise the gzip/zlib WRAPPER handling --
 * which is what http1.c owns -- without depending on a deflater. */
static int stored_deflate(const uint8_t *in, int n, uint8_t *out)
{
    int o = 0;
    out[o++] = 0x01;                                   /* BFINAL=1, BTYPE=00 */
    out[o++] = (uint8_t)(n & 0xff);
    out[o++] = (uint8_t)((n >> 8) & 0xff);
    out[o++] = (uint8_t)(~n & 0xff);
    out[o++] = (uint8_t)((~n >> 8) & 0xff);
    memcpy(out + o, in, (size_t)n);
    return o + n;
}

static uint32_t crc32_ref(const uint8_t *p, int n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return c ^ 0xFFFFFFFFu;
}

static uint32_t adler32_ref(const uint8_t *p, int n)
{
    uint32_t a = 1, b = 0;
    for (int i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

static int make_gzip(const uint8_t *in, int n, uint8_t *out, int flg, const char *name)
{
    int o = 0;
    out[o++] = 0x1f; out[o++] = 0x8b; out[o++] = 0x08; out[o++] = (uint8_t)flg;
    out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 0;          /* MTIME */
    out[o++] = 0; out[o++] = 0xff;                                   /* XFL, OS */
    if (flg & 0x04) { out[o++] = 2; out[o++] = 0; out[o++] = 'x'; out[o++] = 'y'; }
    if (flg & 0x08) { for (const char *p = name; *p; p++) out[o++] = (uint8_t)*p; out[o++] = 0; }
    if (flg & 0x10) { out[o++] = '!'; out[o++] = 0; }
    if (flg & 0x02) { out[o++] = 0xaa; out[o++] = 0xbb; }
    o += stored_deflate(in, n, out + o);
    uint32_t crc = crc32_ref(in, n);
    out[o++] = (uint8_t)crc; out[o++] = (uint8_t)(crc >> 8);
    out[o++] = (uint8_t)(crc >> 16); out[o++] = (uint8_t)(crc >> 24);
    out[o++] = (uint8_t)n; out[o++] = (uint8_t)(n >> 8);
    out[o++] = (uint8_t)(n >> 16); out[o++] = (uint8_t)(n >> 24);
    return o;
}

static int make_zlib(const uint8_t *in, int n, uint8_t *out)
{
    int o = 0;
    out[o++] = 0x78; out[o++] = 0x01;
    o += stored_deflate(in, n, out + o);
    uint32_t ad = adler32_ref(in, n);
    out[o++] = (uint8_t)(ad >> 24); out[o++] = (uint8_t)(ad >> 16);
    out[o++] = (uint8_t)(ad >> 8);  out[o++] = (uint8_t)ad;
    return o;
}

/* Assemble a response with `enc` as Content-Encoding around `payload`. */
static void feed_encoded(struct h1_response *r, const char *enc,
                         const uint8_t *payload, int plen)
{
    char head[256];
    int hl = sprintf(head, "HTTP/1.1 200 OK\r\nContent-Encoding: %s\r\nContent-Length: %d\r\n\r\n",
                     enc, plen);
    h1_response_init(r);
    h1_response_feed(r, head, hl);
    h1_response_feed(r, payload, plen);
}

static void t_content_encoding(void)
{
    static const char text[] =
        "<html><body>The kernel client sent Accept-Encoding: identity because "
        "the inflater was in the other ring.</body></html>";
    int tlen = (int)strlen(text);
    static uint8_t buf[1024];
    struct h1_response r;

    OK(!strcmp(h1_accept_encoding(), "gzip, deflate"));

    /* gzip, minimal header. */
    int n = make_gzip((const uint8_t *)text, tlen, buf, 0x00, NULL);
    feed_encoded(&r, "gzip", buf, n);
    OK(r.state == H1_ST_DONE);
    OK(h1_decode_body(&r) == H1_OK);
    OK(r.body_len == tlen && !memcmp(r.body, text, (size_t)tlen));
    OK(h1_headers_get(&r.hdr, "content-encoding") == NULL);   /* now decoded */
    OK(h1_decode_body(&r) == H1_OK && r.body_len == tlen);    /* idempotent */
    h1_response_free(&r);

    /* gzip with every optional header field present: FEXTRA|FNAME|FCOMMENT|FHCRC. */
    n = make_gzip((const uint8_t *)text, tlen, buf, 0x04 | 0x08 | 0x10 | 0x02, "page.html");
    feed_encoded(&r, "x-gzip", buf, n);
    OK(h1_decode_body(&r) == H1_OK && r.body_len == tlen && !memcmp(r.body, text, (size_t)tlen));
    h1_response_free(&r);

    /* deflate as zlib (what the spec says). */
    n = make_zlib((const uint8_t *)text, tlen, buf);
    feed_encoded(&r, "deflate", buf, n);
    OK(h1_decode_body(&r) == H1_OK && r.body_len == tlen && !memcmp(r.body, text, (size_t)tlen));
    h1_response_free(&r);

    /* deflate as a bare RFC 1951 stream (what a meaningful minority of servers
     * actually send). Real browsers fall back; so do we. */
    n = stored_deflate((const uint8_t *)text, tlen, buf);
    feed_encoded(&r, "deflate", buf, n);
    OK(h1_decode_body(&r) == H1_OK && r.body_len == tlen && !memcmp(r.body, text, (size_t)tlen));
    h1_response_free(&r);

    /* identity, and an absent header, are both no-ops. */
    feed_encoded(&r, "identity", (const uint8_t *)text, tlen);
    OK(h1_decode_body(&r) == H1_OK && r.body_len == tlen);
    h1_response_free(&r);

    /* Corruption must be caught: gzip carries a CRC32 and a length, and both
     * are checked. A wrong CRC that we accept is a silently corrupt page. */
    n = make_gzip((const uint8_t *)text, tlen, buf, 0x00, NULL);
    buf[n - 5] ^= 0xff;                              /* damage the CRC */
    feed_encoded(&r, "gzip", buf, n);
    OK(h1_decode_body(&r) == H1_E_ENCODING);
    h1_response_free(&r);

    n = make_gzip((const uint8_t *)text, tlen, buf, 0x00, NULL);
    buf[n - 1] ^= 0x40;                              /* damage ISIZE */
    feed_encoded(&r, "gzip", buf, n);
    OK(h1_decode_body(&r) == H1_E_ENCODING);
    h1_response_free(&r);

    /* Every truncation of a gzip stream must be rejected, never accepted as a
     * short body. */
    n = make_gzip((const uint8_t *)text, tlen, buf, 0x04 | 0x08, "p");
    int bad = 0;
    for (int k = 0; k < n; k++) {
        feed_encoded(&r, "gzip", buf, k);
        if (h1_decode_body(&r) == H1_OK) bad++;
        h1_response_free(&r);
    }
    OK(bad == 0);

    /* Reserved flag bits set: not a gzip member we understand. */
    n = make_gzip((const uint8_t *)text, tlen, buf, 0x00, NULL);
    buf[3] = 0x20;
    feed_encoded(&r, "gzip", buf, n);
    OK(h1_decode_body(&r) == H1_E_ENCODING);
    h1_response_free(&r);

    /* A gzip whose FEXTRA length runs off the end of the buffer. */
    n = make_gzip((const uint8_t *)text, tlen, buf, 0x04, NULL);
    buf[10] = 0xff; buf[11] = 0xff;
    feed_encoded(&r, "gzip", buf, n);
    OK(h1_decode_body(&r) == H1_E_ENCODING);
    h1_response_free(&r);

    /* Codings we do not implement are an explicit error, not silent garbage. */
    n = make_gzip((const uint8_t *)text, tlen, buf, 0x00, NULL);
    feed_encoded(&r, "br", buf, n);
    OK(h1_decode_body(&r) == H1_E_ENCODING);
    h1_response_free(&r);
    feed_encoded(&r, "zstd", buf, n);
    OK(h1_decode_body(&r) == H1_E_ENCODING);
    h1_response_free(&r);

    /* A decoded body larger than the cap is refused. */
    n = make_gzip((const uint8_t *)text, tlen, buf, 0x00, NULL);
    feed_encoded(&r, "gzip", buf, n);
    h1_response_limit(&r, 16);
    OK(h1_decode_body(&r) != H1_OK);
    h1_response_free(&r);

    /* An empty body with a Content-Encoding is not decodable. */
    h1_response_init(&r);
    const char *e = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 0\r\n\r\n";
    h1_response_feed(&r, e, (int)strlen(e));
    OK(h1_decode_body(&r) == H1_E_ENCODING);
    h1_response_free(&r);
}

int main(void)
{
    t_content_length();
    t_status_line();
    t_headers();
    t_content_length_conflicts();
    t_no_body();
    t_eof_delimited();
    t_chunked();
    t_interim();
    t_message_boundary();
    t_request();
    t_redirect();
    t_conn_and_redirect_chain();
    t_conn_would_block();
    t_conn_transport_error();
    t_content_encoding();

    if (fails) { printf("\n%d FAILURES\n", fails); return 1; }
    printf("\nhttp1_test: ALL PASS\n");
    return 0;
}
