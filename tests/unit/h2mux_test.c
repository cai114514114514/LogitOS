/* h2mux_test -- the browser's fetch transport, over HTTP/2, on the host.
 *
 * WHAT IS ACTUALLY UNDER TEST.  c/net/http/http2.c and hpack.c were already
 * complete and already had tests (make test-h2).  What had never been executed
 * was the WIRING: the layer in browser_rt.c that chooses a protocol from ALPN,
 * re-encodes a serialized HTTP/1.1 request as HPACK, hands one connection to
 * many concurrent requests, and materialises an HTTP/2 stream back into the
 * `struct h1_response` the fetch state machine reads.  So browser_rt.c itself
 * is compiled here, against tests/unit/h2stub/logit.h -- the six socket
 * syscalls are the only thing replaced.  Nothing in this file reimplements a
 * line of the code it is asserting about.
 *
 * WHY THE COUNTERS ARE SERVER-SIDE.  "It multiplexed" is exactly the claim a
 * client cannot make about itself: a client that opened four connections and
 * ran one request on each would report four successful requests and look
 * identical from above.  So the number that decides the test is how many
 * connections the NETWORK saw (g_accepts, incremented in the stub's open) and
 * how many streams arrived on one of them -- both counted underneath the code
 * under test, not by it.
 *
 * THE NEGATIVE CONTROL is the same file compiled against a browser_rt.c built
 * with -DBXFER_H1_ONLY, where ALPN never offers h2.  The multiplexing
 * assertions must FAIL there and every HTTP/1.1 assertion must still pass; if
 * they all pass in both builds this file is measuring nothing.  `make
 * test-h2mux-control` runs it and requires the failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "logit_abi.h"
#include "http1.h"
#include "http2.h"
#include "hpack.h"
#include "bfetch.h"

/* http1.c's content-decoding path, stubbed. Never reached: every request here
 * asks for `identity` and every response is served as such, which is also what
 * keeps the byte-for-byte assertions meaningful -- a decompressed body is not
 * the body the server sent. */
int zlib_decompress(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen)
{ (void)in; (void)inlen; (void)out; (void)outcap; if (outlen) *outlen = 0; return -1; }
int inflate_raw(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen)
{ (void)in; (void)inlen; (void)out; (void)outcap; if (outlen) *outlen = 0; return -1; }

static int fails, checks;
#define OK(cond) do { checks++; if (!(cond)) { \
        printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)
#define OKM(cond, ...) do { checks++; if (!(cond)) { \
        printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ======================================================== byte queues ===== */

struct pipebuf { uint8_t *b; int len, cap, off; };

static void pb_put(struct pipebuf *p, const void *d, int n)
{
    if (n <= 0) return;
    if (p->len + n > p->cap) {
        int c = p->cap ? p->cap : 4096;
        while (c < p->len + n) c *= 2;
        p->b = (uint8_t *)realloc(p->b, (size_t)c);
        p->cap = c;
    }
    memcpy(p->b + p->len, d, (size_t)n);
    p->len += n;
}
static int pb_avail(const struct pipebuf *p) { return p->len - p->off; }
static int pb_take(struct pipebuf *p, void *d, int max)
{
    int n = p->len - p->off;
    if (n > max) n = max;
    if (n <= 0) return 0;
    memcpy(d, p->b + p->off, (size_t)n);
    p->off += n;
    if (p->off == p->len) { p->off = p->len = 0; }
    return n;
}
static void pb_free(struct pipebuf *p) { free(p->b); memset(p, 0, sizeof *p); }

/* =================================================== the HTTP/2 server ==== */

#define SRV_STREAMS 32
#define BODY_UNIT   700          /* bytes per released DATA frame */

struct h2srv {
    struct pipebuf *out;
    struct hpack_enc enc;
    struct hpack_dec dec;

    uint8_t  hb[H2_HEADER_BLOCK_MAX];
    int      hb_len; uint32_t hb_sid;

    int      preface_ok;
    int64_t  conn_win, init_win, stream_win[SRV_STREAMS];

    struct hpack_list req[SRV_STREAMS];
    int      req_seen[SRV_STREAMS];
    int      end_stream_in[SRV_STREAMS];
    uint8_t *data_in[SRV_STREAMS];
    int      data_len[SRV_STREAMS];

    int      hdr_sent[SRV_STREAMS];
    int      body_off[SRV_STREAMS];   /* how much of the reply has gone out */
    int      done_sent[SRV_STREAMS];
    char     reply[SRV_STREAMS][2048];
    int      reply_len[SRV_STREAMS];

    int      hold_body;               /* 1 = emit body only when released */
    int      max_streams_seen;        /* the most open at one moment */
    int      streams_seen;            /* distinct client streams ever */
};

static int sidx(uint32_t id) { return (id && (id & 1)) ? (int)((id - 1) / 2) : -1; }

static void srv_init(struct h2srv *s, struct pipebuf *out)
{
    memset(s, 0, sizeof *s);
    s->out = out;
    hpack_enc_init(&s->enc, HPACK_DEFAULT_CAP);
    hpack_dec_init(&s->dec, HPACK_DEFAULT_CAP);
    s->conn_win = 65535; s->init_win = 65535;
    for (int i = 0; i < SRV_STREAMS; i++) { s->stream_win[i] = 65535; hpack_list_init(&s->req[i]); }
}
static void srv_free(struct h2srv *s)
{
    hpack_enc_free(&s->enc); hpack_dec_free(&s->dec);
    for (int i = 0; i < SRV_STREAMS; i++) { hpack_list_free(&s->req[i]); free(s->data_in[i]); }
}

static void srv_send(struct h2srv *s, uint8_t type, uint8_t flags, uint32_t sid,
                     const void *p, int len)
{
    uint8_t h[H2_FRAME_HDR];
    h2_frame_write(h, (uint32_t)len, type, flags, sid);
    pb_put(s->out, h, H2_FRAME_HDR);
    if (len) pb_put(s->out, p, len);
}

static void srv_headers(struct h2srv *s, uint32_t sid, const char *const *kv, int end_stream)
{
    struct hpack_list l;
    hpack_list_init(&l);
    for (int i = 0; kv[i]; i += 2) hpack_list_add(&l, kv[i], -1, kv[i + 1], -1, 0);
    uint8_t *b = NULL; int n = 0;
    hpack_encode(&s->enc, &l, &b, &n);
    hpack_list_free(&l);
    srv_send(s, H2_F_HEADERS, (uint8_t)(H2_FLAG_END_HEADERS | (end_stream ? H2_FLAG_END_STREAM : 0)),
             sid, b, n);
    free(b);
}

/* The reply for a path. Deterministic and distinct per stream, so an assertion
 * that stream 3 got stream 3's bytes is a real one -- interleaved DATA frames
 * delivered to the wrong stream is precisely the bug multiplexing can have and
 * a uniform body could not see. */
static void srv_make_reply(struct h2srv *s, int i, const char *path)
{
    /* /bin answers with bytes no UTF-8 round trip could restore: a NUL that
     * truncates a C string, an 0xFF that is not valid UTF-8 anywhere, and a
     * lone 0xC3 -- a two-byte lead with nothing following it, which any decode
     * turns into U+FFFD and cannot turn back. */
    if (path && strstr(path, "bin")) {
        static const uint8_t pat[] = { 0x00, 0xFF, 0xC3, 0x41, 0x00, 0x00, 0xFF, 0xC3, 0x80, 0x7F };
        for (int k = 0; k < 400; k++) s->reply[i][k] = (char)pat[k % (int)sizeof pat];
        s->reply_len[i] = 400;
        return;
    }
    int n = 0;
    n += snprintf(s->reply[i] + n, sizeof s->reply[i] - n, "REPLY(%s):", path ? path : "?");
    while (n < 900) s->reply[i][n] = (char)('a' + ((n + i * 7) % 26)), n++;
    s->reply[i][n] = 0;
    s->reply_len[i] = n;
}

/* Push out whatever the reply still owes, within both windows. */
static void srv_flush_bodies(struct h2srv *s)
{
    for (int i = 0; i < SRV_STREAMS; i++) {
        if (!s->req_seen[i] || !s->end_stream_in[i] || s->done_sent[i]) continue;
        uint32_t sid = (uint32_t)(i * 2 + 1);
        if (!s->hdr_sent[i]) {
            char cl[16];
            snprintf(cl, sizeof cl, "%d", s->reply_len[i]);
            const char *kv[] = { ":status", "200", "content-type", "text/plain",
                                 "content-length", cl, NULL };
            srv_headers(s, sid, kv, 0);
            s->hdr_sent[i] = 1;
        }
        if (s->hold_body) continue;
        while (s->body_off[i] < s->reply_len[i]) {
            int want = s->reply_len[i] - s->body_off[i];
            if (want > BODY_UNIT) want = BODY_UNIT;
            int64_t win = s->conn_win < s->stream_win[i] ? s->conn_win : s->stream_win[i];
            if (win <= 0) return;
            if (want > win) want = (int)win;
            int last = (s->body_off[i] + want == s->reply_len[i]);
            srv_send(s, H2_F_DATA, (uint8_t)(last ? H2_FLAG_END_STREAM : 0), sid,
                     s->reply[i] + s->body_off[i], want);
            s->conn_win -= want; s->stream_win[i] -= want;
            s->body_off[i] += want;
            if (last) s->done_sent[i] = 1;
        }
    }
}

/* Release exactly one DATA frame on one stream, for the streaming assertion:
 * the point is that the page sees bytes while the stream is still open. */
static int srv_release_one(struct h2srv *s, int i)
{
    if (!s->hdr_sent[i] || s->done_sent[i]) return 0;
    uint32_t sid = (uint32_t)(i * 2 + 1);
    int want = s->reply_len[i] - s->body_off[i];
    if (want > BODY_UNIT) want = BODY_UNIT;
    if (want <= 0) return 0;
    int last = (s->body_off[i] + want == s->reply_len[i]);
    srv_send(s, H2_F_DATA, (uint8_t)(last ? H2_FLAG_END_STREAM : 0), sid,
             s->reply[i] + s->body_off[i], want);
    s->conn_win -= want; s->stream_win[i] -= want;
    s->body_off[i] += want;
    if (last) s->done_sent[i] = 1;
    return want;
}

static void srv_poll(struct h2srv *s, struct pipebuf *in)
{
    if (!s->preface_ok) {
        if (pb_avail(in) < H2_PREFACE_LEN) return;
        if (memcmp(in->b + in->off, H2_PREFACE, H2_PREFACE_LEN)) { s->preface_ok = -1; return; }
        s->preface_ok = 1;
        in->off += H2_PREFACE_LEN;
        uint8_t st[6] = { 0, H2_SET_MAX_CONCURRENT_STREAMS, 0, 0, 0, 100 };
        srv_send(s, H2_F_SETTINGS, 0, 0, st, 6);
    }
    for (;;) {
        if (pb_avail(in) < H2_FRAME_HDR) break;
        uint32_t len, sid; uint8_t type, flags;
        h2_frame_parse(in->b + in->off, &len, &type, &flags, &sid);
        if ((uint32_t)pb_avail(in) < H2_FRAME_HDR + len) break;
        const uint8_t *pl = in->b + in->off + H2_FRAME_HDR;
        int i = sidx(sid);

        switch (type) {
        case H2_F_SETTINGS:
            if (!(flags & H2_FLAG_ACK)) {
                for (uint32_t k = 0; k + 6 <= len; k += 6) {
                    uint16_t id = (uint16_t)((pl[k] << 8) | pl[k + 1]);
                    uint32_t v = ((uint32_t)pl[k+2] << 24) | ((uint32_t)pl[k+3] << 16) |
                                 ((uint32_t)pl[k+4] << 8) | pl[k+5];
                    if (id == H2_SET_INITIAL_WINDOW_SIZE) {
                        int64_t d = (int64_t)v - s->init_win;
                        s->init_win = v;
                        for (int q = 0; q < SRV_STREAMS; q++) s->stream_win[q] += d;
                    }
                }
                srv_send(s, H2_F_SETTINGS, H2_FLAG_ACK, 0, NULL, 0);
            }
            break;
        case H2_F_WINDOW_UPDATE: {
            uint32_t inc = (((uint32_t)pl[0] << 24) | ((uint32_t)pl[1] << 16) |
                            ((uint32_t)pl[2] << 8) | pl[3]) & 0x7FFFFFFFu;
            if (!sid) s->conn_win += inc;
            else if (i >= 0 && i < SRV_STREAMS) s->stream_win[i] += inc;
            break;
        }
        case H2_F_HEADERS:
        case H2_F_CONTINUATION: {
            const uint8_t *frag = pl; uint32_t flen = len;
            if (type == H2_F_HEADERS) {
                s->hb_len = 0; s->hb_sid = sid;
                if (flags & H2_FLAG_PADDED) { frag++; flen -= 1u + frag[-1]; }
                if (flags & H2_FLAG_PRIORITY) { frag += 5; flen -= 5; }
            }
            if (s->hb_len + (int)flen <= (int)sizeof s->hb) {
                memcpy(s->hb + s->hb_len, frag, flen);
                s->hb_len += (int)flen;
            }
            if (flags & H2_FLAG_END_HEADERS) {
                int j = sidx(s->hb_sid);
                struct hpack_list l; hpack_list_init(&l);
                hpack_decode(&s->dec, s->hb, s->hb_len, &l);
                if (j >= 0 && j < SRV_STREAMS) {
                    hpack_list_free(&s->req[j]);
                    s->req[j] = l;
                    if (!s->req_seen[j]) {
                        s->req_seen[j] = 1;
                        s->streams_seen++;
                        srv_make_reply(s, j, hpack_list_get(&s->req[j], ":path"));
                    }
                } else hpack_list_free(&l);
            }
            if (type == H2_F_HEADERS && (flags & H2_FLAG_END_STREAM) && i >= 0 && i < SRV_STREAMS)
                s->end_stream_in[i] = 1;
            break;
        }
        case H2_F_DATA:
            if (i >= 0 && i < SRV_STREAMS) {
                if (len) {
                    s->data_in[i] = (uint8_t *)realloc(s->data_in[i], (size_t)(s->data_len[i] + len));
                    memcpy(s->data_in[i] + s->data_len[i], pl, len);
                    s->data_len[i] += (int)len;
                }
                s->conn_win += len; s->stream_win[i] += len;   /* we never stall the client */
                if (flags & H2_FLAG_END_STREAM) s->end_stream_in[i] = 1;
            }
            break;
        default: break;
        }
        in->off += H2_FRAME_HDR + (int)len;
        if (in->off == in->len) { in->off = in->len = 0; }
    }

    /* How many streams were open at once, counted where it cannot be faked. */
    int open_now = 0;
    for (int k = 0; k < SRV_STREAMS; k++) if (s->req_seen[k] && !s->done_sent[k]) open_now++;
    if (open_now > s->max_streams_seen) s->max_streams_seen = open_now;

    srv_flush_bodies(s);
}

/* =================================================== the HTTP/1.1 server == */

/* Deliberately minimal and deliberately SERIAL: one request, one response.
 * That is the property the fallback has to respect, and the reason four
 * concurrent requests to an http/1.1 origin must cost four connections. */
struct h1srv { int reqs; int inflight; };

static void h1_serve(struct h1srv *s, struct pipebuf *in, struct pipebuf *out)
{
    for (;;) {
        int avail = pb_avail(in);
        if (avail <= 0) return;
        const char *b = (const char *)in->b + in->off;
        const char *hdr_end = 0;
        for (int i = 0; i + 3 < avail; i++)
            if (!memcmp(b + i, "\r\n\r\n", 4)) { hdr_end = b + i + 4; break; }
        if (!hdr_end) return;
        int hlen = (int)(hdr_end - b);
        /* Consume a declared body too, so a POST does not leave bytes behind. */
        int clen = 0;
        for (int i = 0; i < hlen - 16; i++) {
            if ((b[i] == 'C' || b[i] == 'c') && !strncmp(b + i + 1, "ontent-Length:", 14)) {
                clen = (int)strtol(b + i + 15, 0, 10);
                break;
            }
        }
        if (avail < hlen + clen) return;
        in->off += hlen + clen;
        if (in->off == in->len) { in->off = in->len = 0; }
        s->reqs++;
        char body[256];
        int n = snprintf(body, sizeof body, "h1-reply-%d", s->reqs);
        char head[256];
        int hn = snprintf(head, sizeof head,
                          "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nContent-Type: text/plain\r\n\r\n", n);
        pb_put(out, head, hn);
        pb_put(out, body, n);
    }
}

/* ================================================= the socket stub ======== */

#define NSOCK 32

struct hsock {
    int  used, closed;
    int  h2;
    char host[128];
    int  port;
    struct pipebuf c2s, s2c;
    struct h2srv h2s;
    struct h1srv h1s;
};

static struct hsock g_sk[NSOCK];
static int g_accepts;                  /* connections the network saw */
static int g_offer_h2_seen;            /* ALPN offers that included h2 */
static unsigned long long g_clock = 1000;

/* Which origins speak HTTP/2. A single run therefore covers both an h2 origin
 * and an http/1.1 one, which no live server pair can be relied on to do. */
static int origin_speaks_h2(const char *host)
{ return strncmp(host, "h2.", 3) == 0; }

int hstub_open(const char *host, int port, int flags)
{
    if (flags & SOCK_F_ALPN_H2) g_offer_h2_seen++;
    for (int i = 0; i < NSOCK; i++) {
        if (g_sk[i].used) continue;
        struct hsock *s = &g_sk[i];
        memset(s, 0, sizeof *s);
        s->used = 1;
        s->port = port;
        snprintf(s->host, sizeof s->host, "%s", host ? host : "");
        s->h2 = (flags & SOCK_F_ALPN_H2) && origin_speaks_h2(s->host);
        if (s->h2) srv_init(&s->h2s, &s->s2c);
        g_accepts++;
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
    if (s->h2) srv_poll(&s->h2s, &s->c2s);
    else h1_serve(&s->h1s, &s->c2s, &s->s2c);
    return len;
}

int hstub_recv(int fd, void *buf, int max)
{
    struct hsock *s = sk(fd);
    if (!s) return -1;
    if (s->h2) srv_poll(&s->h2s, &s->c2s);
    return pb_take(&s->s2c, buf, max);
}

int hstub_close(int fd)
{
    struct hsock *s = sk(fd);
    if (!s) return -1;
    if (s->h2) srv_free(&s->h2s);
    pb_free(&s->c2s); pb_free(&s->s2c);
    memset(s, 0, sizeof *s);
    return 0;
}

int hstub_alpn(int fd, char *buf, int max)
{
    struct hsock *s = sk(fd);
    if (!s) return 0;
    const char *p = s->h2 ? "h2" : "http/1.1";
    int n = (int)strlen(p);
    if (n >= max) n = max - 1;
    if (n < 0) n = 0;
    memcpy(buf, p, (size_t)n);
    buf[n] = 0;
    return n;
}

unsigned long long hstub_now(void) { return g_clock; }

/* ================================================= the exchange harness === */

/* One request, in exactly the shape fetch_send_request hands to bxfer_start:
 * a serialized HTTP/1.1 request and a socket from bxfer_open. */
struct xfer {
    struct h1_conn c;
    int   fd;
    int   started;
    /* incremental delivery, for the streaming assertion */
    uint8_t sunk[8192];
    int     sunk_len;
    int     first_sink_at;      /* pump number of the first sunk byte, 0 = never */
    int     pumps;
    int     done_at;            /* pump number the exchange completed */
};

static int x_sink(void *ctx, const uint8_t *d, int n)
{
    struct xfer *x = (struct xfer *)ctx;
    if (!x->first_sink_at) x->first_sink_at = x->pumps;
    if (x->sunk_len + n <= (int)sizeof x->sunk) {
        memcpy(x->sunk + x->sunk_len, d, (size_t)n);
        x->sunk_len += n;
    }
    return H1_OK;
}

static char *build_req(const char *method, const char *path, const char *host,
                       const uint8_t *body, int blen, int *outlen)
{
    struct h1_request q;
    h1_request_init(&q, method, path);
    h1_request_set_header(&q, "Host", host);
    h1_request_set_header(&q, "User-Agent", "Mozilla/5.0 (LogitOS) Logit/1.0");
    h1_request_set_header(&q, "Accept", "*/*");
    h1_request_set_header(&q, "Accept-Encoding", "identity");
    h1_request_set_header(&q, "Connection", "close");
    if (body && blen > 0) h1_request_set_body(&q, body, blen);
    char *raw = 0; int n = 0;
    int rc = h1_request_build(&q, &raw, &n);
    h1_request_free(&q);
    if (rc != H1_OK) return 0;
    *outlen = n;
    return raw;
}

static int x_start(struct xfer *x, const char *method, const char *path,
                   const char *host, int port, const uint8_t *body, int blen, int sink)
{
    memset(x, 0, sizeof *x);
    x->fd = bxfer_open(host, port, 1);
    if (x->fd < 0) return -1;
    int rawlen = 0;
    char *raw = build_req(method, path, host, body, blen, &rawlen);
    if (!raw) return -1;
    struct h1_transport t = { 0, 0, 0, 0 };
    /* browser_rt's bxfer builds its own transport for the h2 path and only
     * needs read/write for the h1 one; these mirror js_webapi's tr_read/write. */
    extern int h2mux_tr_read(void *, void *, int);
    extern int h2mux_tr_write(void *, const void *, int);
    t.read = h2mux_tr_read; t.write = h2mux_tr_write; t.ctx = (void *)(long)x->fd;
    if (bxfer_start(&x->c, &t, raw, rawlen, &x->fd, host, port, 1) != H1_OK) { free(raw); return -1; }
    x->started = 1;
    if (sink) h1_response_sink(&x->c.resp, x_sink, x);
    return 0;
}

int h2mux_tr_read(void *ctx, void *buf, int len)
{
    int fd = (int)(long)ctx;
    int n = hstub_recv(fd, buf, len);
    if (n > 0) return n;
    if (n < 0) return H1_EOF;
    return H1_AGAIN;
}
int h2mux_tr_write(void *ctx, const void *buf, int len)
{
    int fd = (int)(long)ctx;
    int n = hstub_send(fd, buf, len);
    return n >= 0 ? n : H1_TERR;
}

static void x_free(struct xfer *x)
{
    if (x->started) bxfer_free(&x->c);
    if (x->fd >= 0) bxfer_close(x->fd);
    x->started = 0; x->fd = -1;
}

/* Step every exchange once, round-robin, exactly as the fetch loop does. */
static void x_spin(struct xfer **v, int n, int budget)
{
    for (int r = 0; r < budget; r++) {
        int live = 0;
        for (int i = 0; i < n; i++) {
            struct xfer *x = v[i];
            if (!x->started || x->c.state == H1_C_DONE || x->c.state == H1_C_ERROR) continue;
            x->pumps++;
            bxfer_pump(&x->c);
            if (x->c.state == H1_C_DONE && !x->done_at) x->done_at = x->pumps;
            live++;
        }
        g_clock += 5;
        if (!live) return;
    }
}

static void reset_world(void)
{
    bxfer_close_all();
    bfetch_close_all();
    for (int i = 0; i < NSOCK; i++) if (g_sk[i].used) hstub_close(i);
    g_accepts = 0;
    g_offer_h2_seen = 0;
    bxfer_reset_stats();
}

/* ============================================================ the tests === */

/* THE ONE THAT MATTERS.  Four concurrent requests to one HTTP/2 origin must
 * arrive as four streams on ONE connection. The connection count is taken from
 * the stub's accept counter -- underneath the code under test -- and the
 * stream count from the server's own decode of the HEADERS frames. */
static void t_multiplex(void)
{
    reset_world();
    printf("-- multiplexing: 4 concurrent requests to one h2 origin\n");

    struct xfer x[4];
    struct xfer *v[4];
    const char *paths[4] = { "/a", "/b", "/c", "/d" };
    for (int i = 0; i < 4; i++) {
        v[i] = &x[i];
        OK(x_start(&x[i], "GET", paths[i], "h2.example", 443, 0, 0, 0) == 0);
    }
    x_spin(v, 4, 4000);

    OKM(g_accepts == 1, "connections: expected 1, got %d", g_accepts);
    OKM(g_offer_h2_seen >= 1, "ALPN never offered h2 (%d offers)", g_offer_h2_seen);

    /* Every request landed on the SAME socket, and that socket saw 4 streams. */
    int fd0 = x[0].fd;
    int same = 1;
    for (int i = 1; i < 4; i++) if (x[i].fd != fd0) same = 0;
    OKM(same, "the four requests did not share one socket (%d %d %d %d)",
        x[0].fd, x[1].fd, x[2].fd, x[3].fd);

    struct hsock *s = sk(fd0);
    OK(s != 0);
    if (s && s->h2) {
        OKM(s->h2s.streams_seen == 4, "streams on the connection: expected 4, got %d",
            s->h2s.streams_seen);
        OKM(s->h2s.max_streams_seen >= 2,
            "streams were never concurrent (peak %d) -- this is serialisation, not multiplexing",
            s->h2s.max_streams_seen);
    } else {
        OKM(0, "the shared socket is not an HTTP/2 connection");
    }

    /* Distinct stream ids, and the right body on each -- interleaved DATA
     * delivered to the wrong stream is the failure a uniform body hides. */
    for (int i = 0; i < 4; i++) {
        OKM(x[i].c.state == H1_C_DONE, "request %d did not complete (state %d err %d)",
            i, x[i].c.state, x[i].c.err);
        OKM(x[i].c.resp.code == 200, "request %d status %d", i, x[i].c.resp.code);
        char want[64];
        snprintf(want, sizeof want, "REPLY(%s):", paths[i]);
        OKM(x[i].c.resp.body && !strncmp((char *)x[i].c.resp.body, want, strlen(want)),
            "request %d got the wrong stream's body: %.24s", i,
            x[i].c.resp.body ? (char *)x[i].c.resp.body : "(null)");
        OKM(h1_headers_get(&x[i].c.resp.hdr, "content-type") != 0,
            "request %d lost its response headers", i);
        /* Pseudo-headers are framing, not fields: :status must not appear. */
        OKM(h1_headers_get(&x[i].c.resp.hdr, ":status") == 0,
            "request %d leaked a pseudo-header into the field list", i);
    }

    int conns = 0, h2conns = 0, streams = 0, peak = 0;
    bxfer_stats(&conns, &h2conns, &streams, &peak);
    printf("   bxfer: conns=%d h2conns=%d streams=%d peak=%d | network accepts=%d, "
           "server streams=%d peak=%d\n",
           conns, h2conns, streams, peak, g_accepts,
           s && s->h2 ? s->h2s.streams_seen : 0, s && s->h2 ? s->h2s.max_streams_seen : 0);
    OKM(conns == 1 && h2conns == 1, "bxfer's own count disagrees: conns=%d h2=%d", conns, h2conns);

    for (int i = 0; i < 4; i++) x_free(&x[i]);
}

/* A binary request body must reach the wire byte for byte, with a byte-counted
 * Content-Length -- over HTTP/2 as over HTTP/1.1. The payload carries 0x00,
 * 0xFF and a lone 0xC3: a NUL truncates a C string, 0xFF is not valid UTF-8 at
 * all, and a lone 0xC3 is a two-byte lead with nothing after it, so any round
 * trip through text turns it into U+FFFD and cannot turn it back. */
static void t_binary_body(void)
{
    reset_world();
    printf("-- a binary request body over h2\n");

    uint8_t payload[512];
    for (int i = 0; i < 300; i++) payload[i] = (uint8_t)(i * 7);
    payload[7] = 0x00; payload[8] = 0xFF; payload[9] = 0xC3; payload[10] = 0x00;
    payload[11] = 0xFF; payload[12] = 0xC3; payload[13] = 0x41;
    int plen = 300;

    struct xfer x; struct xfer *v[1] = { &x };
    OK(x_start(&x, "POST", "/upload", "h2.example", 443, payload, plen, 0) == 0);
    x_spin(v, 1, 4000);

    OKM(x.c.state == H1_C_DONE, "the POST did not complete (state %d err %d)", x.c.state, x.c.err);
    struct hsock *s = sk(x.fd);
    OK(s && s->h2);
    if (s && s->h2) {
        OKM(s->h2s.data_len[0] == plen, "server received %d body bytes, expected %d",
            s->h2s.data_len[0], plen);
        int bad = -1;
        for (int i = 0; i < plen && i < s->h2s.data_len[0]; i++)
            if (s->h2s.data_in[0][i] != payload[i]) { bad = i; break; }
        OKM(bad < 0, "request body differs at byte %d: got 0x%02X want 0x%02X",
            bad, bad >= 0 ? s->h2s.data_in[0][bad] : 0, bad >= 0 ? payload[bad] : 0);
        const char *cl = hpack_list_get(&s->h2s.req[0], "content-length");
        OKM(cl && atoi(cl) == plen, "content-length on the wire: %s, expected %d",
            cl ? cl : "(absent)", plen);
        const char *m = hpack_list_get(&s->h2s.req[0], ":method");
        const char *pa = hpack_list_get(&s->h2s.req[0], ":path");
        const char *au = hpack_list_get(&s->h2s.req[0], ":authority");
        const char *sc = hpack_list_get(&s->h2s.req[0], ":scheme");
        OKM(m && !strcmp(m, "POST"), ":method = %s", m ? m : "(absent)");
        OKM(pa && !strcmp(pa, "/upload"), ":path = %s", pa ? pa : "(absent)");
        OKM(au && !strcmp(au, "h2.example"), ":authority = %s", au ? au : "(absent)");
        OKM(sc && !strcmp(sc, "https"), ":scheme = %s", sc ? sc : "(absent)");
        /* Connection-specific fields are malformed in HTTP/2; the caller sent
         * `Connection: close` and it must not have survived the re-encoding. */
        OKM(hpack_list_get(&s->h2s.req[0], "connection") == 0, "Connection: leaked into HTTP/2");
        OKM(hpack_list_get(&s->h2s.req[0], "host") == 0, "Host: leaked (it is :authority in h2)");
    }
    x_free(&x);
}

/* A response body must be delivered as DATA frames land, not when the stream
 * closes. Asserted by holding the body: the sink must have the first frame
 * while the stream is still open, which is a claim about WHEN and which a
 * buffered implementation fails. */
static void t_streaming(void)
{
    reset_world();
    printf("-- streamed delivery over h2\n");

    struct xfer x; struct xfer *v[1] = { &x };
    OK(x_start(&x, "GET", "/events", "h2.example", 443, 0, 0, 1) == 0);

    struct hsock *s = 0;
    /* Get the request out and the headers back, with the body withheld. */
    for (int i = 0; i < 200; i++) {
        s = sk(x.fd);
        if (s && s->h2) s->h2s.hold_body = 1;
        x.pumps++;
        bxfer_pump(&x.c);
        g_clock += 5;
        if (h1_response_headers_done(&x.c.resp)) break;
    }
    OKM(h1_response_headers_done(&x.c.resp), "headers never arrived");
    OKM(x.c.resp.code == 200, "status %d", x.c.resp.code);
    OKM(h1_response_streaming(&x.c.resp), "the response is not streaming");
    OKM(x.sunk_len == 0, "body bytes arrived before any DATA frame was sent");
    OKM(x.c.state != H1_C_DONE, "the exchange completed before the body was sent");

    /* One DATA frame, stream still open. */
    s = sk(x.fd);
    OK(s && s->h2);
    int sent = s && s->h2 ? srv_release_one(&s->h2s, 0) : 0;
    OKM(sent > 0, "the server released no data");
    for (int i = 0; i < 40; i++) { x.pumps++; bxfer_pump(&x.c); g_clock += 5; }

    OKM(x.sunk_len == sent, "sink holds %d bytes after one frame of %d", x.sunk_len, sent);
    OKM(x.c.state != H1_C_DONE,
        "the stream closed -- the partial delivery above proves nothing if the response was over");
    OKM(x.c.resp.body_len == 0, "a streamed body must not also be buffered (%d bytes)",
        x.c.resp.body_len);

    /* Now let the rest go. */
    if (s && s->h2) s->h2s.hold_body = 0;
    x_spin(v, 1, 2000);
    OKM(x.c.state == H1_C_DONE, "the stream never completed (state %d err %d)", x.c.state, x.c.err);
    if (s && s->h2) {
        OKM(x.sunk_len == s->h2s.reply_len[0], "sink got %d bytes, the reply was %d",
            x.sunk_len, s->h2s.reply_len[0]);
        OKM(!memcmp(x.sunk, s->h2s.reply[0], (size_t)x.sunk_len), "the streamed bytes differ");
    }
    x_free(&x);
}

/* A response body that carries NUL, 0xFF and a lone 0xC3 must reach the reader
 * unmodified -- the same payload as the request side, in the other direction. */
static void t_binary_response(void)
{
    reset_world();
    printf("-- a binary response body over h2\n");

    struct xfer x; struct xfer *v[1] = { &x };
    OK(x_start(&x, "GET", "/bin", "h2.example", 443, 0, 0, 1) == 0);

    x_spin(v, 1, 3000);
    struct hsock *s = sk(x.fd);
    OK(s && s->h2);

    OKM(x.c.state == H1_C_DONE, "the response did not complete (state %d err %d)", x.c.state, x.c.err);
    if (s && s->h2) {
        OKM(x.sunk_len == 400, "reader got %d bytes, expected 400", x.sunk_len);
        int bad = -1;
        for (int i = 0; i < 400 && i < x.sunk_len; i++)
            if (x.sunk[i] != (uint8_t)s->h2s.reply[0][i]) { bad = i; break; }
        OKM(bad < 0, "response body differs at byte %d: got 0x%02X want 0x%02X", bad,
            bad >= 0 ? x.sunk[bad] : 0, bad >= 0 ? (uint8_t)s->h2s.reply[0][bad] : 0);
    }
    x_free(&x);
}

/* THE FALLBACK, which is the common case and the one that breaks silently. An
 * origin that answers `http/1.1` must behave exactly as it did before HTTP/2
 * existed: one exchange per connection, four requests, four connections, four
 * correct responses. A client that speculatively shared a socket and then found
 * out it was talking HTTP/1.1 must undo that, not interleave. */
static void t_h1_fallback(void)
{
    reset_world();
    printf("-- the HTTP/1.1 fallback: 4 concurrent requests to an http/1.1 origin\n");

    struct xfer x[4]; struct xfer *v[4];
    for (int i = 0; i < 4; i++) {
        char p[16]; snprintf(p, sizeof p, "/r%d", i);
        v[i] = &x[i];
        OK(x_start(&x[i], "GET", p, "h1.example", 443, 0, 0, 0) == 0);
    }
    x_spin(v, 4, 6000);

    for (int i = 0; i < 4; i++) {
        OKM(x[i].c.state == H1_C_DONE, "h1 request %d did not complete (state %d err %d)",
            i, x[i].c.state, x[i].c.err);
        OKM(x[i].c.resp.code == 200, "h1 request %d status %d", i, x[i].c.resp.code);
        OKM(x[i].c.resp.body_len > 0, "h1 request %d has an empty body", i);
    }
    /* Each exchange got its own socket: an HTTP/1.1 connection carries one at
     * a time, so sharing would have interleaved two responses into one parser. */
    for (int i = 0; i < 4; i++)
        for (int k = i + 1; k < 4; k++)
            OKM(x[i].fd != x[k].fd, "h1 requests %d and %d shared a connection", i, k);
    OKM(g_accepts >= 4, "four http/1.1 requests used %d connections", g_accepts);

    int h2conns = 0;
    bxfer_stats(0, &h2conns, 0, 0);
    OKM(h2conns == 0, "an http/1.1 origin was recorded as HTTP/2 (%d)", h2conns);
    printf("   http/1.1: %d requests over %d connections (one each, as before)\n", 4, g_accepts);

    for (int i = 0; i < 4; i++) x_free(&x[i]);
}

/* A plain http:// origin has no ALPN at all, so it is HTTP/1.1 by
 * construction. Worth its own case because the h2 path must not be reached by
 * a code path that never had a handshake to ask. */
static void t_plaintext_is_h1(void)
{
    reset_world();
    printf("-- http:// (no TLS, no ALPN) stays HTTP/1.1\n");
    struct xfer x; struct xfer *v[1] = { &x };
    memset(&x, 0, sizeof x);
    x.fd = bxfer_open("h2.example", 80, 0);        /* the h2 origin, over cleartext */
    OK(x.fd >= 0);
    int rawlen = 0;
    char *raw = build_req("GET", "/", "h2.example", 0, 0, &rawlen);
    OK(raw != 0);
    struct h1_transport t = { h2mux_tr_read, h2mux_tr_write, 0, (void *)(long)x.fd };
    OK(bxfer_start(&x.c, &t, raw, rawlen, &x.fd, "h2.example", 80, 0) == H1_OK);
    x.started = 1;
    x_spin(v, 1, 3000);
    OKM(x.c.state == H1_C_DONE, "cleartext request did not complete (state %d)", x.c.state);
    int h2conns = 0;
    bxfer_stats(0, &h2conns, 0, 0);
    OKM(h2conns == 0, "a cleartext connection was recorded as HTTP/2");
    x_free(&x);
}

int main(void)
{
    printf("=== h2mux_test: the browser's fetch transport over HTTP/2 ===\n");
#ifdef BXFER_H1_ONLY
    printf("*** built with -DBXFER_H1_ONLY: this is the NEGATIVE CONTROL and the\n"
           "*** multiplexing assertions are REQUIRED to fail.\n");
#endif
    t_multiplex();
    t_binary_body();
    t_streaming();
    t_binary_response();
    t_h1_fallback();
    t_plaintext_is_h1();

    printf("\n%d checks, %d failures\n", checks, fails);
    if (fails) { printf("h2mux_test: FAIL\n"); return 1; }
    printf("h2mux_test: ALL PASS\n");
    return 0;
}
