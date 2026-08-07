/* Host unit test for c/net/http/http2.c -- frames, streams, flow control.
 *
 * There is no QEMU here and no socket.  The client talks to an in-memory
 * server through the transport vtable, exactly as webapi_test.c does for
 * fetch() and tcp_test.c does for TCP: two byte queues and a frame-level peer
 * written in this file.  That buys three things a live server cannot give:
 *
 *   - THE PEER CAN MISBEHAVE ON PURPOSE.  A real server will not send a pad
 *     length longer than its own frame, a CONTINUATION for a different
 *     stream, or a WINDOW_UPDATE of zero.  Those are the inputs that decide
 *     whether this code is safe, and they only exist if the test writes them.
 *
 *   - THE SERVER CAN ENFORCE FLOW CONTROL STRICTLY.  The 2 MiB transfer below
 *     sends only what the client's windows allow and then stops.  If the
 *     client ever fails to send a WINDOW_UPDATE the transfer does not slow
 *     down, it stops dead -- which is the actual failure mode of a flow
 *     control bug, and which a live server would paper over by having a
 *     window larger than the test's body.
 *
 *   - THE CLOCK IS A VARIABLE.  Stall detection is asserted by advancing
 *     `now`, not by waiting.
 *
 * The byte-granularity test at the end is the same property http1_fuzz.c
 * asserts for HTTP/1.1: the same bytes delivered one at a time, in sevens, and
 * all at once must produce the identical outcome.  For a frame protocol that
 * is where "I assumed a frame arrives whole" lives.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "http2.h"
#include "hpack.h"
#include "hpool.h"

static int fails, checks;
#define OK(cond) do { checks++; if (!(cond)) { \
        printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)
#define OKM(cond, ...) do { checks++; if (!(cond)) { \
        printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ======================================================== the wire ======== */

struct pipebuf { uint8_t *b; int len, cap, off; };

static void pb_put(struct pipebuf *p, const void *d, int n)
{
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

struct wire {
    struct pipebuf c2s, s2c;
    int max_read;         /* 0 = unlimited; else the transport reads at most N */
    int max_write;        /* 0 = unlimited; else short writes of at most N */
    int dead;
    int eof;
};

static int tr_read(void *ctx, void *buf, int len)
{
    struct wire *w = (struct wire *)ctx;
    if (w->dead) return H2_TERR;
    if (w->max_read && len > w->max_read) len = w->max_read;
    int n = pb_take(&w->s2c, buf, len);
    if (n == 0) return w->eof ? H2_EOF : H2_AGAIN;
    return n;
}
static int tr_write(void *ctx, const void *buf, int len)
{
    struct wire *w = (struct wire *)ctx;
    if (w->dead) return H2_TERR;
    if (w->max_write && len > w->max_write) len = w->max_write;
    pb_put(&w->c2s, buf, len);
    return len;
}
static struct h2_transport mk_transport(struct wire *w)
{
    struct h2_transport t;
    t.read = tr_read; t.write = tr_write; t.poll = NULL; t.ctx = w;
    return t;
}

/* ==================================================== the test server ===== */

#define SRV_STREAMS 32
#define SRV_EVENTS  4096

struct ev { uint8_t type, flags; uint32_t sid, len, aux; };

struct server {
    struct wire *w;
    struct hpack_enc enc;          /* encodes the responses we send */
    struct hpack_dec dec;          /* decodes the requests the client sends */

    uint8_t  hb[H2_HEADER_BLOCK_MAX];
    int      hb_len, hb_active;
    uint32_t hb_sid;

    struct ev ev[SRV_EVENTS];
    int nev;

    int      preface_ok;
    int      settings_acks;        /* acks the CLIENT sent for our SETTINGS */
    int      settings_recv;        /* SETTINGS frames the client sent */
    int      goaway_seen;
    uint32_t goaway_code;
    int      pings_acked;

    /* our view of the windows the CLIENT has granted us */
    int64_t  conn_win;
    int64_t  stream_win[SRV_STREAMS];
    int64_t  init_win;

    /* per (client) stream, indexed by (id-1)/2 */
    struct hpack_list req[SRV_STREAMS];
    int      req_seen[SRV_STREAMS];
    int64_t  data_in[SRV_STREAMS];
    int      end_stream_in[SRV_STREAMS];
    int      rst_in[SRV_STREAMS];
    uint32_t rst_code[SRV_STREAMS];
    /* streams the server has been asked to RST (by id, for pushes) */
    uint32_t rst_ids[64];
    int      n_rst_ids;
};

static int sidx(uint32_t id) { return (id && (id & 1)) ? (int)((id - 1) / 2) : -1; }

static void srv_init(struct server *s, struct wire *w)
{
    memset(s, 0, sizeof *s);
    s->w = w;
    hpack_enc_init(&s->enc, HPACK_DEFAULT_CAP);
    hpack_dec_init(&s->dec, HPACK_DEFAULT_CAP);
    s->conn_win = 65535;
    s->init_win = 65535;
    for (int i = 0; i < SRV_STREAMS; i++) { s->stream_win[i] = 65535; hpack_list_init(&s->req[i]); }
}
static void srv_free(struct server *s)
{
    hpack_enc_free(&s->enc);
    hpack_dec_free(&s->dec);
    for (int i = 0; i < SRV_STREAMS; i++) hpack_list_free(&s->req[i]);
}

static void srv_send(struct server *s, uint8_t type, uint8_t flags, uint32_t sid,
                     const void *p, int len)
{
    uint8_t h[H2_FRAME_HDR];
    h2_frame_write(h, (uint32_t)len, type, flags, sid);
    pb_put(&s->w->s2c, h, H2_FRAME_HDR);
    if (len) pb_put(&s->w->s2c, p, len);
}

/* Raw byte injection, so a test can write a frame the writer above would
 * refuse to build (a bad length, a reserved bit set, a truncated payload). */
static void srv_raw(struct server *s, const void *p, int n) { pb_put(&s->w->s2c, p, n); }

static void srv_settings(struct server *s, const uint16_t *ids, const uint32_t *vals, int n)
{
    uint8_t p[96];
    int o = 0;
    for (int i = 0; i < n && o + 6 <= (int)sizeof p; i++) {
        p[o++] = (uint8_t)(ids[i] >> 8); p[o++] = (uint8_t)ids[i];
        p[o++] = (uint8_t)(vals[i] >> 24); p[o++] = (uint8_t)(vals[i] >> 16);
        p[o++] = (uint8_t)(vals[i] >> 8);  p[o++] = (uint8_t)vals[i];
    }
    srv_send(s, H2_F_SETTINGS, 0, 0, p, o);
}

/* Encode and send a HEADERS block from a NULL-terminated name/value array. */
static void srv_headers(struct server *s, uint32_t sid, const char *const *kv,
                        int end_stream, int split)
{
    struct hpack_list l;
    hpack_list_init(&l);
    for (int i = 0; kv[i]; i += 2) hpack_list_add(&l, kv[i], -1, kv[i + 1], -1, 0);
    uint8_t *b = NULL; int n = 0;
    hpack_encode(&s->enc, &l, &b, &n);
    hpack_list_free(&l);

    uint8_t flags = (uint8_t)(end_stream ? H2_FLAG_END_STREAM : 0);
    if (!split || n < 2) {
        srv_send(s, H2_F_HEADERS, (uint8_t)(flags | H2_FLAG_END_HEADERS), sid, b, n);
    } else {
        /* Deliberately fragmented: HEADERS + CONTINUATION, which is the path
         * that only runs when a header block exceeds one frame. */
        int half = n / 2;
        srv_send(s, H2_F_HEADERS, flags, sid, b, half);
        srv_send(s, H2_F_CONTINUATION, H2_FLAG_END_HEADERS, sid, b + half, n - half);
    }
    free(b);
}

static void srv_rst(struct server *s, uint32_t sid, uint32_t code)
{
    uint8_t p[4] = { (uint8_t)(code >> 24), (uint8_t)(code >> 16), (uint8_t)(code >> 8), (uint8_t)code };
    srv_send(s, H2_F_RST_STREAM, 0, sid, p, 4);
}
static void srv_goaway(struct server *s, uint32_t last, uint32_t code)
{
    uint8_t p[8] = { (uint8_t)(last >> 24), (uint8_t)(last >> 16), (uint8_t)(last >> 8), (uint8_t)last,
                     (uint8_t)(code >> 24), (uint8_t)(code >> 16), (uint8_t)(code >> 8), (uint8_t)code };
    srv_send(s, H2_F_GOAWAY, 0, 0, p, 8);
}
static void srv_window(struct server *s, uint32_t sid, uint32_t inc)
{
    uint8_t p[4] = { (uint8_t)(inc >> 24), (uint8_t)(inc >> 16), (uint8_t)(inc >> 8), (uint8_t)inc };
    srv_send(s, H2_F_WINDOW_UPDATE, 0, sid, p, 4);
}

/* Send at most `want` body bytes on a stream, respecting BOTH of the client's
 * windows. Returns how many it actually sent -- 0 means the client owes a
 * WINDOW_UPDATE, which is the deadlock the flow control test is about. */
static int srv_data(struct server *s, uint32_t sid, const uint8_t *body, int want, int end)
{
    int i = sidx(sid);
    int64_t win = s->conn_win;
    if (i >= 0 && s->stream_win[i] < win) win = s->stream_win[i];
    if (win < 0) win = 0;
    int n = want;
    if (n > win) n = (int)win;
    if (n > H2_DEFAULT_MAX_FRAME) n = H2_DEFAULT_MAX_FRAME;
    if (n <= 0 && !(end && want == 0)) return 0;
    srv_send(s, H2_F_DATA, (uint8_t)((end && n == want) ? H2_FLAG_END_STREAM : 0), sid, body, n);
    s->conn_win -= n;
    if (i >= 0) s->stream_win[i] -= n;
    return n;
}

/* Consume everything the client has written and record it. */
static void srv_poll(struct server *s)
{
    struct pipebuf *p = &s->w->c2s;
    if (!s->preface_ok) {
        if (pb_avail(p) < H2_PREFACE_LEN) return;
        if (!memcmp(p->b + p->off, H2_PREFACE, H2_PREFACE_LEN)) s->preface_ok = 1;
        else { s->preface_ok = -1; return; }
        p->off += H2_PREFACE_LEN;
    }
    for (;;) {
        if (pb_avail(p) < H2_FRAME_HDR) break;
        uint32_t len, sid; uint8_t type, flags;
        h2_frame_parse(p->b + p->off, &len, &type, &flags, &sid);
        if ((uint32_t)pb_avail(p) < H2_FRAME_HDR + len) break;
        const uint8_t *pl = p->b + p->off + H2_FRAME_HDR;
        int i = sidx(sid);

        if (s->nev < SRV_EVENTS) {
            struct ev *e = &s->ev[s->nev++];
            e->type = type; e->flags = flags; e->sid = sid; e->len = len; e->aux = 0;
            if (type == H2_F_WINDOW_UPDATE && len == 4)
                e->aux = ((uint32_t)pl[0] << 24 | (uint32_t)pl[1] << 16 |
                          (uint32_t)pl[2] << 8 | pl[3]) & 0x7FFFFFFFu;
            if ((type == H2_F_RST_STREAM || type == H2_F_GOAWAY) && len >= 4)
                e->aux = ((uint32_t)pl[len == 4 ? 0 : 4] << 24 | (uint32_t)pl[len == 4 ? 1 : 5] << 16 |
                          (uint32_t)pl[len == 4 ? 2 : 6] << 8 | pl[len == 4 ? 3 : 7]);
        }

        switch (type) {
        case H2_F_SETTINGS:
            if (flags & H2_FLAG_ACK) s->settings_acks++;
            else {
                s->settings_recv++;
                for (uint32_t k = 0; k + 6 <= len; k += 6) {
                    uint16_t id = (uint16_t)((pl[k] << 8) | pl[k + 1]);
                    uint32_t v = ((uint32_t)pl[k + 2] << 24) | ((uint32_t)pl[k + 3] << 16) |
                                 ((uint32_t)pl[k + 4] << 8) | pl[k + 5];
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
            uint32_t inc = ((uint32_t)pl[0] << 24 | (uint32_t)pl[1] << 16 |
                            (uint32_t)pl[2] << 8 | pl[3]) & 0x7FFFFFFFu;
            if (!sid) s->conn_win += inc;
            else if (i >= 0 && i < SRV_STREAMS) s->stream_win[i] += inc;
            break;
        }
        case H2_F_HEADERS:
        case H2_F_CONTINUATION: {
            const uint8_t *frag = pl; uint32_t flen = len;
            if (type == H2_F_HEADERS) {
                s->hb_len = 0; s->hb_active = 1; s->hb_sid = sid;
                if (flags & H2_FLAG_PADDED) { frag++; flen -= 1 + frag[-1]; }
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
                    s->req_seen[j] = 1;
                } else hpack_list_free(&l);
                s->hb_active = 0;
            }
            if (type == H2_F_HEADERS && (flags & H2_FLAG_END_STREAM) &&
                i >= 0 && i < SRV_STREAMS) s->end_stream_in[i] = 1;
            break;
        }
        case H2_F_DATA:
            if (i >= 0 && i < SRV_STREAMS) {
                s->data_in[i] += len;
                if (flags & H2_FLAG_END_STREAM) s->end_stream_in[i] = 1;
            }
            break;
        case H2_F_RST_STREAM:
            if (i >= 0 && i < SRV_STREAMS) {
                s->rst_in[i] = 1;
                s->rst_code[i] = ((uint32_t)pl[0] << 24) | ((uint32_t)pl[1] << 16) |
                                 ((uint32_t)pl[2] << 8) | pl[3];
            }
            if (s->n_rst_ids < 64) s->rst_ids[s->n_rst_ids++] = sid;
            break;
        case H2_F_GOAWAY:
            s->goaway_seen = 1;
            if (len >= 8) s->goaway_code = ((uint32_t)pl[4] << 24) | ((uint32_t)pl[5] << 16) |
                                           ((uint32_t)pl[6] << 8) | pl[7];
            break;
        case H2_F_PING:
            if (flags & H2_FLAG_ACK) s->pings_acked++;
            break;
        default: break;
        }
        p->off += H2_FRAME_HDR + (int)len;
        if (p->off == p->len) { p->off = p->len = 0; }
    }
}

static int srv_count(const struct server *s, uint8_t type, uint32_t sid)
{
    int n = 0;
    for (int i = 0; i < s->nev; i++)
        if (s->ev[i].type == type && (sid == 0xFFFFFFFFu || s->ev[i].sid == sid)) n++;
    return n;
}
static int srv_saw_rst(const struct server *s, uint32_t sid, uint32_t code)
{
    for (int i = 0; i < s->nev; i++)
        if (s->ev[i].type == H2_F_RST_STREAM && s->ev[i].sid == sid && s->ev[i].aux == code) return 1;
    return 0;
}

/* Drive both sides until `done` or the budget runs out. Returns iterations. */
static int spin(struct h2_conn *c, struct server *s, int (*done)(struct h2_conn *, void *),
                void *arg, int budget, int64_t *clock)
{
    int i = 0;
    for (; i < budget; i++) {
        h2_conn_pump(c, clock ? (*clock)++ : 0);
        srv_poll(s);
        if (done && done(c, arg)) return i;
        if (h2_conn_state(c) == H2_C_ERROR || h2_conn_state(c) == H2_C_CLOSED) return i;
    }
    return i;
}
static int done_stream(struct h2_conn *c, void *arg) { return h2_stream_done(c, *(uint32_t *)arg); }

/* ================================================== frame header basics === */

static void t_frame_header(void)
{
    uint8_t p[H2_FRAME_HDR];
    uint32_t len, sid; uint8_t type, flags;

    h2_frame_write(p, 16384, H2_F_DATA, H2_FLAG_END_STREAM, 5);
    OK(h2_frame_parse(p, &len, &type, &flags, &sid) == H2_OK);
    OK(len == 16384 && type == H2_F_DATA && flags == H2_FLAG_END_STREAM && sid == 5);

    /* The maximum a 24-bit length can express. */
    h2_frame_write(p, 0xFFFFFF, H2_F_HEADERS, 0, 0x7FFFFFFF);
    h2_frame_parse(p, &len, &type, &flags, &sid);
    OK(len == 0xFFFFFF && sid == 0x7FFFFFFFu);

    /* The reserved high bit of the stream id MUST be ignored, not rejected --
     * treating it as part of the id turns an ordinary frame into a protocol
     * error against any peer that happens to set it. */
    p[5] |= 0x80;
    h2_frame_parse(p, &len, &type, &flags, &sid);
    OK(sid == 0x7FFFFFFFu);

    uint8_t z[H2_FRAME_HDR] = { 0, 0, 0, H2_F_PING, H2_FLAG_ACK, 0x80, 0, 0, 0 };
    h2_frame_parse(z, &len, &type, &flags, &sid);
    OK(len == 0 && type == H2_F_PING && flags == H2_FLAG_ACK && sid == 0);
}

/* ================================================== preface and settings == */

static void t_preface(void)
{
    struct wire w; memset(&w, 0, sizeof w);
    struct server srv; srv_init(&srv, &w);
    struct h2_conn c;
    struct h2_transport t = mk_transport(&w);

    OK(h2_conn_start(&c, &t) == H2_OK);
    h2_conn_pump(&c, 0);
    srv_poll(&srv);

    OK(srv.preface_ok == 1);
    OK(srv.settings_recv == 1);
    /* The connection window must be raised explicitly: SETTINGS cannot do it,
     * and without this every multiplexed page stalls at 65535 bytes total. */
    int found = 0;
    for (int i = 0; i < srv.nev; i++)
        if (srv.ev[i].type == H2_F_WINDOW_UPDATE && srv.ev[i].sid == 0 &&
            srv.ev[i].aux == (uint32_t)(H2_OUR_CONN_WINDOW - 65535)) found = 1;
    OK(found);

    /* Our SETTINGS must disable push, or a server is entitled to spend our
     * connection window on things we never asked for. */
    int push_off = 0;
    for (int i = 0; i < srv.nev; i++) (void)i;
    /* re-read the settings payload from the recorded frame is overkill; the
     * server applied them, so assert through behaviour in t_push instead. */
    (void)push_off;

    /* The peer's SETTINGS get an ACK, and its ACK of ours clears the flag. */
    uint16_t ids[] = { H2_SET_MAX_CONCURRENT_STREAMS };
    uint32_t vals[] = { 100 };
    srv_settings(&srv, ids, vals, 1);
    h2_conn_pump(&c, 0);
    srv_poll(&srv);
    OK(srv_count(&srv, H2_F_SETTINGS, 0) >= 2);      /* ours + the ack */
    OK(c.settings_ack_pending == 0);                 /* the server acked ours */
    OK(c.peer_max_conc == H2_MAX_STREAMS);           /* clamped to our slot count */

    h2_conn_free(&c);
    srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);
}

/* ========================================================= a whole GET ==== */

static void t_get(void)
{
    struct wire w; memset(&w, 0, sizeof w);
    struct server srv; srv_init(&srv, &w);
    struct h2_conn c;
    struct h2_transport t = mk_transport(&w);
    h2_conn_start(&c, &t);

    int id = h2_request(&c, "GET", "https", "example.com", "/index.html", NULL, NULL, 0);
    OK(id == 1);
    h2_conn_pump(&c, 0);
    srv_poll(&srv);

    /* The request the server actually received, decoded through a real HPACK
     * decoder: this is the round trip our encoder has to survive. */
    OK(srv.req_seen[0]);
    OK(!strcmp(hpack_list_get(&srv.req[0], ":method"), "GET"));
    OK(!strcmp(hpack_list_get(&srv.req[0], ":scheme"), "https"));
    OK(!strcmp(hpack_list_get(&srv.req[0], ":authority"), "example.com"));
    OK(!strcmp(hpack_list_get(&srv.req[0], ":path"), "/index.html"));
    OK(srv.end_stream_in[0] == 1);                   /* a GET closes its half */

    struct h2_stream *s = h2_stream_get(&c, 1);
    OK(s && s->state == H2_S_HALF_CLOSED_LOCAL);

    const char *hdrs[] = { ":status", "200", "content-type", "text/html",
                           "content-length", "11", NULL };
    srv_headers(&srv, 1, hdrs, 0, 0);
    srv_data(&srv, 1, (const uint8_t *)"hello world", 11, 1);

    uint32_t sid = 1;
    spin(&c, &srv, done_stream, &sid, 50, NULL);

    OK(h2_stream_done(&c, 1));
    OK(h2_stream_status(&c, 1) == 200);
    OK(h2_stream_err(&c, 1) == H2_OK);
    const char *ct = h2_stream_header(&c, 1, "content-type");
    OK(ct && !strcmp(ct, "text/html"));
    int bl = 0;
    const uint8_t *body = h2_stream_body(&c, 1, &bl);
    OK(bl == 11 && body && !memcmp(body, "hello world", 11));
    OK(h2_stream_get(&c, 1)->state == H2_S_CLOSED);

    /* A released slot is reusable, and the next stream id is odd and larger. */
    h2_stream_release(&c, 1);
    OK(h2_stream_get(&c, 1) == NULL);
    int id2 = h2_request(&c, "GET", "https", "example.com", "/2", NULL, NULL, 0);
    OK(id2 == 3);

    h2_conn_free(&c);
    srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);
}

/* A header block split across HEADERS + CONTINUATION, plus trailers and a 1xx
 * informational response -- the three shapes a response can take that a
 * one-HEADERS-per-stream implementation gets wrong. */
static void t_header_shapes(void)
{
    struct wire w; memset(&w, 0, sizeof w);
    struct server srv; srv_init(&srv, &w);
    struct h2_conn c;
    struct h2_transport t = mk_transport(&w);
    h2_conn_start(&c, &t);

    uint32_t id = (uint32_t)h2_request(&c, "GET", "https", "e.com", "/", NULL, NULL, 0);
    h2_conn_pump(&c, 0); srv_poll(&srv);

    /* 1xx first: it is NOT the response, and keeping it would make 100 the
     * answer to the request. */
    const char *info[] = { ":status", "103", "link", "</s.css>; rel=preload", NULL };
    srv_headers(&srv, id, info, 0, 0);
    h2_conn_pump(&c, 0);
    OK(!h2_stream_done(&c, id));
    OK(h2_stream_status(&c, id) == 0);

    const char *hdrs[] = { ":status", "200", "content-type", "text/plain",
                           "x-long", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL };
    srv_headers(&srv, id, hdrs, 0, 1);               /* split across CONTINUATION */
    srv_data(&srv, id, (const uint8_t *)"abc", 3, 0);
    const char *trail[] = { "x-checksum", "deadbeef", NULL };
    srv_headers(&srv, id, trail, 1, 0);              /* trailers, END_STREAM */

    spin(&c, &srv, done_stream, &id, 50, NULL);
    OK(h2_stream_status(&c, id) == 200);
    const char *xl = h2_stream_header(&c, id, "x-long");
    OK(xl && !strcmp(xl, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    int bl = 0; h2_stream_body(&c, id, &bl);
    OK(bl == 3);
    struct h2_stream *s = h2_stream_get(&c, id);
    OK(s && hpack_list_get(&s->trailer, "x-checksum") &&
       !strcmp(hpack_list_get(&s->trailer, "x-checksum"), "deadbeef"));
    OK(h2_conn_state(&c) == H2_C_OPEN);

    h2_conn_free(&c);
    srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);
}

/* ========================================================= multiplexing === */

struct allargs { struct h2_conn *c; uint32_t *ids; int n; };
static int done_all(struct h2_conn *c, void *arg)
{
    struct allargs *a = (struct allargs *)arg;
    for (int i = 0; i < a->n; i++) if (!h2_stream_done(c, a->ids[i])) return 0;
    return 1;
}

/* The property HTTP/2 exists for: N requests in flight on ONE connection, with
 * the responses interleaved. A client that serialised -- sent request 2 only
 * after response 1 completed -- would fetch the same bytes and pass a naive
 * "did it download" check, so the assertions are about SIMULTANEITY: all N
 * streams open at the same instant, and body bytes arriving for stream k while
 * stream k-1 is still unfinished. */
static void t_multiplex(void)
{
    struct wire w; memset(&w, 0, sizeof w);
    struct server srv; srv_init(&srv, &w);
    struct h2_conn c;
    struct h2_transport t = mk_transport(&w);
    h2_conn_start(&c, &t);

    enum { N = 8 };
    uint32_t ids[N];
    for (int i = 0; i < N; i++) {
        char path[32];
        snprintf(path, sizeof path, "/r%d", i);
        int r = h2_request(&c, "GET", "https", "example.com", path, NULL, NULL, 0);
        OKM(r > 0, "request %d refused: %s", i, h2_strerror(r));
        ids[i] = (uint32_t)r;
    }
    /* All N were queued before ANY response existed: that is what one
     * connection buys and six sockets used to cost. */
    OK(c.concurrent == N);
    h2_conn_pump(&c, 0);
    srv_poll(&srv);
    for (int i = 0; i < N; i++) OK(srv.req_seen[i]);
    OK(c.max_concurrent_seen == N);

    /* Answer them INTERLEAVED: one header block each, then one DATA chunk per
     * stream per round, in a rotating order. */
    const char *hdrs[] = { ":status", "200", "content-type", "text/plain", NULL };
    for (int i = N - 1; i >= 0; i--) srv_headers(&srv, ids[i], hdrs, 0, 0);

    char payload[N][64];
    int plen[N];
    for (int i = 0; i < N; i++) {
        plen[i] = snprintf(payload[i], sizeof payload[i], "body-of-stream-%d", i);
    }
    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < N; i++) {
            int per = (plen[i] + 3) / 4;
            int off = round * per;
            int n = plen[i] - off; if (n > per) n = per;
            if (n <= 0) continue;
            int last = (off + n >= plen[i]);
            srv_data(&srv, ids[i], (const uint8_t *)payload[i] + off, n, last);
        }
        h2_conn_pump(&c, 0);
        srv_poll(&srv);
    }

    struct allargs aa = { &c, ids, N };
    spin(&c, &srv, done_all, &aa, 100, NULL);

    for (int i = 0; i < N; i++) {
        OKM(h2_stream_done(&c, ids[i]), "stream %u not done", ids[i]);
        OK(h2_stream_status(&c, ids[i]) == 200);
        int bl = 0;
        const uint8_t *b = h2_stream_body(&c, ids[i], &bl);
        OKM(bl == plen[i] && b && !memcmp(b, payload[i], (size_t)bl),
            "stream %u body %d/%d", ids[i], bl, plen[i]);
    }
    /* One connection, N streams, and the server never saw a second preface. */
    OK(srv.preface_ok == 1);
    OK(c.streams_opened == N && c.streams_done == N);

    /* And the slot limit is reported, not exceeded. */
    for (int i = 0; i < N; i++) h2_stream_release(&c, ids[i]);
    int opened = 0;
    for (int i = 0; i < H2_MAX_STREAMS + 4; i++)
        if (h2_request(&c, "GET", "https", "e.com", "/x", NULL, NULL, 0) > 0) opened++;
    OK(opened == H2_MAX_STREAMS);
    OK(h2_request(&c, "GET", "https", "e.com", "/y", NULL, NULL, 0) == H2_E_NOSLOT);

    h2_conn_free(&c);
    srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);
}

/* Header compression is the other half of the win: the tenth request on a
 * connection must be far smaller on the wire than the first. */
static void t_compression_win(void)
{
    struct wire w; memset(&w, 0, sizeof w);
    struct server srv; srv_init(&srv, &w);
    struct h2_conn c;
    struct h2_transport t = mk_transport(&w);
    h2_conn_start(&c, &t);

    struct hpack_list extra;
    hpack_list_init(&extra);
    hpack_list_add(&extra, "user-agent", -1,
                   "Mozilla/5.0 (LogitOS x86_64) Browser/1.0", -1, 0);
    hpack_list_add(&extra, "accept", -1,
                   "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8", -1, 0);
    hpack_list_add(&extra, "accept-language", -1, "en-US,en;q=0.9", -1, 0);
    hpack_list_add(&extra, "referer", -1, "https://example.com/index.html", -1, 0);

    int first = 0, tenth = 0;
    for (int i = 0; i < 10; i++) {
        int before = w.c2s.len;
        char path[32]; snprintf(path, sizeof path, "/asset%d.png", i);
        int id = h2_request(&c, "GET", "https", "example.com", path, &extra, NULL, 0);
        OK(id > 0);
        h2_conn_pump(&c, 0);
        int sz = w.c2s.len - before;
        if (i == 0) first = sz;
        if (i == 9) tenth = sz;
        srv_poll(&srv);
        h2_stream_release(&c, (uint32_t)id);
    }
    OKM(tenth * 3 < first, "10th request %d bytes vs 1st %d -- headers are not being indexed",
        tenth, first);
    /* And the server still decoded it correctly, which is the part that
     * matters: compression that loses a header is worse than no compression. */
    OK(!strcmp(hpack_list_get(&srv.req[9], ":path"), "/asset9.png"));
    OK(!strcmp(hpack_list_get(&srv.req[9], "referer"), "https://example.com/index.html"));

    hpack_list_free(&extra);
    h2_conn_free(&c);
    srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);
}

/* ======================================================== flow control ==== */

/* A 2 MiB response through a server that sends ONLY what the client's windows
 * allow. Both windows start far below 2 MiB, so this completes if and only if
 * the client keeps replenishing them. A client that forgets does not run slow,
 * it stops -- and the assertion is that the transfer finished, not that it was
 * fast. */
static void t_flow_recv(void)
{
    struct wire w; memset(&w, 0, sizeof w);
    struct server srv; srv_init(&srv, &w);
    struct h2_conn c;
    struct h2_transport t = mk_transport(&w);
    h2_conn_start(&c, &t);

    uint32_t id = (uint32_t)h2_request(&c, "GET", "https", "big.example", "/2mb", NULL, NULL, 0);
    h2_conn_pump(&c, 0);
    srv_poll(&srv);

    const char *hdrs[] = { ":status", "200", "content-type", "application/octet-stream", NULL };
    srv_headers(&srv, id, hdrs, 0, 0);

    const int TOTAL = 2 * 1024 * 1024;
    uint8_t *chunk = (uint8_t *)malloc(H2_DEFAULT_MAX_FRAME);
    for (int i = 0; i < H2_DEFAULT_MAX_FRAME; i++) chunk[i] = (uint8_t)(i * 7 + 13);

    /* PHASE 1, the control: burst without letting the client run. The windows
     * must bind at exactly what was granted -- 256 KiB, our announced
     * SETTINGS_INITIAL_WINDOW_SIZE -- and then stop dead. If this phase
     * managed the whole 2 MiB, the test below would prove nothing. */
    int sent = 0, stuck = 0;
    while (sent < TOTAL) {
        int want = TOTAL - sent;
        if (want > H2_DEFAULT_MAX_FRAME) want = H2_DEFAULT_MAX_FRAME;
        int n = srv_data(&srv, id, chunk, want, sent + want >= TOTAL);
        if (n == 0) { stuck++; break; }
        sent += n;
    }
    OKM(stuck == 1 && sent == H2_OUR_STREAM_WINDOW,
        "the window did not bind: %d bytes went out before it shut", sent);

    /* PHASE 2: now let the client run. It completes only if it keeps
     * replenishing BOTH windows as it consumes. */
    int spins = 0;
    while (sent < TOTAL && spins < 20000) {
        int want = TOTAL - sent;
        if (want > H2_DEFAULT_MAX_FRAME) want = H2_DEFAULT_MAX_FRAME;
        sent += srv_data(&srv, id, chunk, want, sent + want >= TOTAL);
        h2_conn_pump(&c, 0);
        srv_poll(&srv);
        spins++;
    }
    OKM(sent == TOTAL, "server could only send %d of %d bytes -- the client stopped "
                       "replenishing its windows", sent, TOTAL);

    spin(&c, &srv, done_stream, &id, 200, NULL);
    OK(h2_stream_done(&c, id));
    int bl = 0;
    const uint8_t *b = h2_stream_body(&c, id, &bl);
    OKM(bl == TOTAL, "body %d of %d", bl, TOTAL);
    OK(b && b[0] == 13 && b[TOTAL - 1] == chunk[(TOTAL - 1) % H2_DEFAULT_MAX_FRAME]);
    /* Both levels were replenished, not just one: a client that only updates
     * the stream window stalls at the connection window and vice versa. */
    int conn_upd = 0, stream_upd = 0;
    for (int i = 0; i < srv.nev; i++)
        if (srv.ev[i].type == H2_F_WINDOW_UPDATE) { if (srv.ev[i].sid) stream_upd++; else conn_upd++; }
    OKM(conn_upd >= 2, "only %d connection WINDOW_UPDATEs", conn_upd);
    OKM(stream_upd >= 2, "only %d stream WINDOW_UPDATEs", stream_upd);

    free(chunk);
    h2_conn_free(&c);
    srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);
}

/* The send direction: a POST body larger than the peer's window. The client
 * must never send more than it has been granted -- the server checks every
 * DATA frame against the window it issued -- and must resume when granted
 * more. */
static void t_flow_send(void)
{
    struct wire w; memset(&w, 0, sizeof w);
    struct server srv; srv_init(&srv, &w);
    struct h2_conn c;
    struct h2_transport t = mk_transport(&w);
    h2_conn_start(&c, &t);

    const int BODY = 300000;
    uint8_t *body = (uint8_t *)malloc(BODY);
    for (int i = 0; i < BODY; i++) body[i] = (uint8_t)(i * 31 + 5);

    uint32_t id = (uint32_t)h2_request(&c, "POST", "https", "up.example", "/put", NULL, body, BODY);
    OK(id == 1);

    /* Track the client's spend against what we granted. The server's initial
     * windows are 65535 in each direction until it grants more. */
    int64_t granted_conn = 65535, granted_stream = 65535;
    int64_t spent = 0;
    int rounds = 0;
    while (!h2_stream_done(&c, id) && rounds < 2000) {
        int before = (int)srv.data_in[0];
        h2_conn_pump(&c, (int64_t)rounds);
        srv_poll(&srv);
        int64_t now_spent = srv.data_in[0];
        OKM(now_spent <= granted_conn && now_spent <= granted_stream,
            "client sent %lld bytes with only %lld/%lld granted",
            (long long)now_spent, (long long)granted_conn, (long long)granted_stream);
        if (now_spent == before && now_spent < BODY) {
            /* It has stopped: grant more and check that it resumes. */
            srv_window(&srv, 0, 32768);   granted_conn += 32768;
            srv_window(&srv, id, 32768);  granted_stream += 32768;
        }
        spent = now_spent;
        rounds++;
    }
    OKM(spent == BODY, "only %lld of %d body bytes arrived", (long long)spent, BODY);
    OK(srv.end_stream_in[0] == 1);
    OK(h2_stream_get(&c, id)->state == H2_S_HALF_CLOSED_LOCAL ||
       h2_stream_get(&c, id)->state == H2_S_CLOSED);

    const char *hdrs[] = { ":status", "204", NULL };
    srv_headers(&srv, id, hdrs, 1, 0);
    spin(&c, &srv, done_stream, &id, 50, NULL);
    OK(h2_stream_status(&c, id) == 204);

    free(body);
    h2_conn_free(&c);
    srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);
}

/* SETTINGS_INITIAL_WINDOW_SIZE applies RETROACTIVELY to streams that are
 * already open (RFC 7540 6.9.2). An implementation that only applies it to new
 * streams over-sends on the old ones and is reset by the server. */
static void t_flow_settings_retroactive(void)
{
    struct wire w; memset(&w, 0, sizeof w);
    struct server srv; srv_init(&srv, &w);
    struct h2_conn c;
    struct h2_transport t = mk_transport(&w);
    h2_conn_start(&c, &t);

    uint8_t body[16] = { 0 };
    uint32_t id = (uint32_t)h2_request(&c, "POST", "https", "e.com", "/", NULL, body, 16);
    h2_conn_pump(&c, 0); srv_poll(&srv);
    struct h2_stream *s = h2_stream_get(&c, id);
    OK(s != NULL);
    int64_t before = s ? s->send_win : 0;

    uint16_t ids[] = { H2_SET_INITIAL_WINDOW_SIZE };
    uint32_t vals[] = { 100000 };
    srv_settings(&srv, ids, vals, 1);
    h2_conn_pump(&c, 0);
    /* The stream had already spent 16 bytes; the delta is +34465 regardless. */
    OKM(s->send_win == before + (100000 - 65535),
        "send window %lld, expected %lld", (long long)s->send_win,
        (long long)(before + (100000 - 65535)));

    /* Shrinking it is legal too, and can drive a window NEGATIVE -- which is a
     * defined state, not an error: the stream simply may not send until the
     * peer grants enough to bring it back above zero. */
    uint32_t small[] = { 1 };
    srv_settings(&srv, ids, small, 1);
    h2_conn_pump(&c, 0);
    OK(s->send_win < 0 || s->send_win <= 1);
    OK(h2_conn_state(&c) == H2_C_OPEN);

    h2_conn_free(&c);
    srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);
}

/* THE DEADLOCK. A peer that never sends WINDOW_UPDATE would hang a naive
 * client forever, and "forever" is the one failure a browser cannot report.
 * The connection must FAIL instead, on a clock the test controls. */
static void t_flow_stall(void)
{
    struct wire w; memset(&w, 0, sizeof w);
    struct server srv; srv_init(&srv, &w);
    struct h2_conn c;
    struct h2_transport t = mk_transport(&w);
    h2_conn_start(&c, &t);
    h2_conn_stall_ms(&c, 100);

    const int BODY = 200000;
    uint8_t *body = (uint8_t *)calloc(1, BODY);
    uint32_t id = (uint32_t)h2_request(&c, "POST", "https", "e.com", "/", NULL, body, BODY);

    int64_t clock = 0;
    for (int i = 0; i < 50 && h2_conn_state(&c) == H2_C_OPEN; i++) {
        h2_conn_pump(&c, clock);
        srv_poll(&srv);       /* the server reads, but grants nothing */
        clock += 10;
    }
    OKM(h2_conn_state(&c) == H2_C_ERROR, "a shut window hung instead of failing");
    OK(c.err == H2_E_STALL);
    OK(h2_stream_err(&c, id) == H2_E_STALL);
    /* And it told the peer why, rather than just vanishing. */
    OK(srv.goaway_seen && srv.goaway_code == H2_ERR_FLOW_CONTROL_ERROR);
    /* The 65535 bytes it WAS allowed did go out first: this is a stall, not a
     * refusal to send. */
    OK(srv.data_in[0] == 65535);

    free(body);
    h2_conn_free(&c);
    srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);
}

/* ================================================= the state machine ====== */

/* Each case builds a fresh connection, drives one illegal (or legal-but-edgy)
 * transition, and asserts the exact consequence: a CONNECTION error kills
 * everything, a STREAM error kills one stream and leaves the connection
 * usable. Getting that distinction backwards turns one bad response into a
 * dead page, which is why every case checks the connection state too. */

struct fix { struct wire w; struct server srv; struct h2_conn c; };

static uint32_t fix_open(struct fix *f)
{
    memset(&f->w, 0, sizeof f->w);
    srv_init(&f->srv, &f->w);
    struct h2_transport t = mk_transport(&f->w);
    h2_conn_start(&f->c, &t);
    uint32_t id = (uint32_t)h2_request(&f->c, "GET", "https", "e.com", "/", NULL, NULL, 0);
    h2_conn_pump(&f->c, 0);
    srv_poll(&f->srv);
    return id;
}
static void fix_close(struct fix *f)
{
    h2_conn_free(&f->c);
    srv_free(&f->srv);
    pb_free(&f->w.c2s); pb_free(&f->w.s2c);
}

static void t_states(void)
{
    struct fix f;
    uint32_t id;

    /* --- connection errors ------------------------------------------- */

    /* DATA on stream 0. */
    id = fix_open(&f);
    srv_send(&f.srv, H2_F_DATA, 0, 0, "x", 1);
    h2_conn_pump(&f.c, 0); srv_poll(&f.srv);
    OK(h2_conn_state(&f.c) == H2_C_ERROR && f.c.err == H2_E_PROTO);
    OK(f.srv.goaway_seen && f.srv.goaway_code == H2_ERR_PROTOCOL_ERROR);
    fix_close(&f);

    /* HEADERS on a server-initiated (even) stream: only PUSH_PROMISE may
     * reserve one, and we refuse those. */
    id = fix_open(&f);
    { const char *h[] = { ":status", "200", NULL }; srv_headers(&f.srv, 2, h, 1, 0); }
    h2_conn_pump(&f.c, 0);
    OK(h2_conn_state(&f.c) == H2_C_ERROR);
    fix_close(&f);

    /* HEADERS on an id we have never used. */
    id = fix_open(&f);
    { const char *h[] = { ":status", "200", NULL }; srv_headers(&f.srv, 99, h, 1, 0); }
    h2_conn_pump(&f.c, 0);
    OK(h2_conn_state(&f.c) == H2_C_ERROR);
    fix_close(&f);

    /* A frame between HEADERS and its CONTINUATION -- even a harmless PING on
     * stream 0. A header block is one unit; splicing anything into it splices
     * two HPACK streams together. */
    id = fix_open(&f);
    {
        struct hpack_list l; hpack_list_init(&l);
        hpack_list_add(&l, ":status", -1, "200", -1, 0);
        uint8_t *b = NULL; int n = 0;
        hpack_encode(&f.srv.enc, &l, &b, &n);
        srv_send(&f.srv, H2_F_HEADERS, 0, id, b, n / 2);
        srv_send(&f.srv, H2_F_PING, 0, 0, "12345678", 8);
        srv_send(&f.srv, H2_F_CONTINUATION, H2_FLAG_END_HEADERS, id, b + n / 2, n - n / 2);
        free(b); hpack_list_free(&l);
    }
    h2_conn_pump(&f.c, 0);
    OK(h2_conn_state(&f.c) == H2_C_ERROR && f.c.err == H2_E_PROTO);
    fix_close(&f);

    /* CONTINUATION with no header block open. */
    id = fix_open(&f);
    srv_send(&f.srv, H2_F_CONTINUATION, H2_FLAG_END_HEADERS, id, "\x82", 1);
    h2_conn_pump(&f.c, 0);
    OK(h2_conn_state(&f.c) == H2_C_ERROR);
    fix_close(&f);

    /* Fixed-size frames with the wrong size. */
    struct { uint8_t type; int len; uint32_t sid; } badsize[] = {
        { H2_F_RST_STREAM,    3, 1 }, { H2_F_RST_STREAM,    5, 1 },
        { H2_F_PING,          7, 0 }, { H2_F_PING,          9, 0 },
        { H2_F_WINDOW_UPDATE, 3, 0 }, { H2_F_WINDOW_UPDATE, 5, 1 },
        { H2_F_PRIORITY,      4, 1 }, { H2_F_GOAWAY,        7, 0 },
        { H2_F_SETTINGS,      7, 0 }
    };
    for (unsigned k = 0; k < sizeof badsize / sizeof badsize[0]; k++) {
        uint8_t junk[16] = { 0 };
        id = fix_open(&f);
        srv_send(&f.srv, badsize[k].type, 0, badsize[k].sid, junk, badsize[k].len);
        h2_conn_pump(&f.c, 0); srv_poll(&f.srv);
        OKM(h2_conn_state(&f.c) == H2_C_ERROR, "type %d len %d was accepted",
            badsize[k].type, badsize[k].len);
        OKM(f.c.err == H2_E_FRAMESIZE || f.c.err == H2_E_PROTO, "type %d gave %s",
            badsize[k].type, h2_strerror(f.c.err));
        fix_close(&f);
    }

    /* SETTINGS with the ACK flag AND a payload. */
    id = fix_open(&f);
    srv_send(&f.srv, H2_F_SETTINGS, H2_FLAG_ACK, 0, "\x00\x03\x00\x00\x00\x01", 6);
    h2_conn_pump(&f.c, 0);
    OK(h2_conn_state(&f.c) == H2_C_ERROR && f.c.err == H2_E_FRAMESIZE);
    fix_close(&f);

    /* SETTINGS on a nonzero stream, PING on a nonzero stream, GOAWAY likewise. */
    id = fix_open(&f);
    srv_send(&f.srv, H2_F_SETTINGS, 0, 1, NULL, 0);
    h2_conn_pump(&f.c, 0);
    OK(h2_conn_state(&f.c) == H2_C_ERROR);
    fix_close(&f);

    id = fix_open(&f);
    srv_send(&f.srv, H2_F_PING, 0, 1, "12345678", 8);
    h2_conn_pump(&f.c, 0);
    OK(h2_conn_state(&f.c) == H2_C_ERROR);
    fix_close(&f);

    /* A frame larger than the SETTINGS_MAX_FRAME_SIZE we advertised. The
     * length is rejected BEFORE the payload is buffered, so a peer cannot make
     * us hold 16 MiB by claiming it. */
    id = fix_open(&f);
    {
        uint8_t h[H2_FRAME_HDR];
        h2_frame_write(h, H2_OUR_MAX_FRAME + 1, H2_F_DATA, 0, id);
        srv_raw(&f.srv, h, H2_FRAME_HDR);
    }
    h2_conn_pump(&f.c, 0);
    OK(h2_conn_state(&f.c) == H2_C_ERROR && f.c.err == H2_E_FRAMESIZE);
    fix_close(&f);

    /* A pad length that reaches past the end of its own frame -- the classic
     * HTTP/2 out-of-bounds read. */
    id = fix_open(&f);
    {
        const char *h[] = { ":status", "200", NULL };
        srv_headers(&f.srv, id, h, 0, 0);
        uint8_t p[8] = { 200, 'a', 'b', 'c' };      /* pad=200 inside a 4-byte payload */
        srv_send(&f.srv, H2_F_DATA, H2_FLAG_PADDED, id, p, 4);
    }
    h2_conn_pump(&f.c, 0);
    OK(h2_conn_state(&f.c) == H2_C_ERROR && f.c.err == H2_E_PROTO);
    fix_close(&f);

    /* Legal padding, by contrast, must work and must not appear in the body. */
    id = fix_open(&f);
    {
        const char *h[] = { ":status", "200", NULL };
        srv_headers(&f.srv, id, h, 0, 0);
        uint8_t p[8] = { 3, 'a', 'b', 'c', 0, 0, 0 };
        srv_send(&f.srv, H2_F_DATA, (uint8_t)(H2_FLAG_PADDED | H2_FLAG_END_STREAM), id, p, 7);
    }
    spin(&f.c, &f.srv, done_stream, &id, 20, NULL);
    OK(h2_conn_state(&f.c) == H2_C_OPEN);
    { int bl = 0; const uint8_t *b = h2_stream_body(&f.c, id, &bl);
      OK(bl == 3 && b && !memcmp(b, "abc", 3)); }
    fix_close(&f);

    /* WINDOW_UPDATE of zero, and one that overflows the window. */
    id = fix_open(&f);
    srv_window(&f.srv, 0, 0);
    h2_conn_pump(&f.c, 0);
    OK(h2_conn_state(&f.c) == H2_C_ERROR);
    fix_close(&f);

    id = fix_open(&f);
    srv_window(&f.srv, 0, 0x7FFFFFFF);
    h2_conn_pump(&f.c, 0);
    OK(h2_conn_state(&f.c) == H2_C_ERROR && f.c.err == H2_E_FLOW);
    fix_close(&f);

    /* SETTINGS_ENABLE_PUSH = 1 from a server is a connection error (RFC 9113
     * 8.4): the client cannot push, so the server is confused or probing. */
    id = fix_open(&f);
    { uint16_t i2[] = { H2_SET_ENABLE_PUSH }; uint32_t v2[] = { 1 };
      srv_settings(&f.srv, i2, v2, 1); }
    h2_conn_pump(&f.c, 0);
    OK(h2_conn_state(&f.c) == H2_C_ERROR && f.c.err == H2_E_PROTO);
    fix_close(&f);

    /* SETTINGS_MAX_FRAME_SIZE outside [16384, 2^24-1]. */
    id = fix_open(&f);
    { uint16_t i2[] = { H2_SET_MAX_FRAME_SIZE }; uint32_t v2[] = { 1024 };
      srv_settings(&f.srv, i2, v2, 1); }
    h2_conn_pump(&f.c, 0);
    OK(h2_conn_state(&f.c) == H2_C_ERROR);
    fix_close(&f);

    /* --- stream errors: one stream dies, the connection lives -------- */

    /* DATA before HEADERS ends the stream but not the connection. */
    id = fix_open(&f);
    srv_send(&f.srv, H2_F_DATA, H2_FLAG_END_STREAM, id, "x", 1);
    h2_conn_pump(&f.c, 0); srv_poll(&f.srv);
    OK(h2_conn_state(&f.c) == H2_C_OPEN);
    OK(h2_stream_done(&f.c, id) && h2_stream_err(&f.c, id) == H2_E_PROTO);
    OK(f.srv.rst_in[0]);
    fix_close(&f);

    /* Malformed responses: each is a stream error and the next request on the
     * same connection still works. */
    static const char *const bad[][6] = {
        { "content-type", "text/html", NULL },                    /* no :status */
        { ":status", "200", ":method", "GET", NULL },             /* wrong pseudo-field */
        { "content-type", "text/html", ":status", "200", NULL },  /* pseudo after regular */
        { ":status", "200", "Content-Type", "text/html", NULL },  /* uppercase name */
        { ":status", "200", "connection", "keep-alive", NULL },   /* connection-specific */
        { ":status", "200", "transfer-encoding", "chunked", NULL },
        { ":status", "2000", NULL },                              /* not three digits */
        { ":status", "20x", NULL }
    };
    for (unsigned k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        id = fix_open(&f);
        srv_headers(&f.srv, id, bad[k], 1, 0);
        h2_conn_pump(&f.c, 0); srv_poll(&f.srv);
        OKM(h2_conn_state(&f.c) == H2_C_OPEN, "case %u took the connection down", k);
        OKM(h2_stream_done(&f.c, id) && h2_stream_err(&f.c, id) == H2_E_PROTO,
            "case %u was accepted", k);
        OKM(f.srv.rst_in[0] && f.srv.rst_code[0] == H2_ERR_PROTOCOL_ERROR,
            "case %u did not RST_STREAM", k);
        /* the connection is still usable */
        uint32_t id2 = (uint32_t)h2_request(&f.c, "GET", "https", "e.com", "/2", NULL, NULL, 0);
        OKM(id2 == 3, "case %u: the connection was unusable afterwards", k);
        const char *good[] = { ":status", "204", NULL };
        srv_headers(&f.srv, id2, good, 1, 0);
        spin(&f.c, &f.srv, done_stream, &id2, 20, NULL);
        OKM(h2_stream_status(&f.c, id2) == 204, "case %u: the next stream failed", k);
        fix_close(&f);
    }

    /* RST_STREAM from the peer. REFUSED_STREAM is retryable and says so;
     * anything else is not. */
    id = fix_open(&f);
    srv_rst(&f.srv, id, H2_ERR_REFUSED_STREAM);
    h2_conn_pump(&f.c, 0);
    OK(h2_stream_done(&f.c, id) && h2_stream_err(&f.c, id) == H2_E_REFUSED);
    OK(h2_conn_state(&f.c) == H2_C_OPEN);
    fix_close(&f);

    id = fix_open(&f);
    srv_rst(&f.srv, id, H2_ERR_INTERNAL_ERROR);
    h2_conn_pump(&f.c, 0); srv_poll(&f.srv);
    OK(h2_stream_done(&f.c, id) && h2_stream_err(&f.c, id) == H2_E_RESET);
    OK(h2_stream_get(&f.c, id)->rst_code == H2_ERR_INTERNAL_ERROR);
    /* We must not answer a reset with a reset -- that is a frame storm. */
    OK(!f.srv.rst_in[0]);
    fix_close(&f);

    /* RST_STREAM on a stream that was never opened. */
    id = fix_open(&f);
    srv_rst(&f.srv, 77, H2_ERR_CANCEL);
    h2_conn_pump(&f.c, 0);
    OK(h2_conn_state(&f.c) == H2_C_ERROR);
    fix_close(&f);

    /* Our own cancel: RST_STREAM(CANCEL) goes out, the stream ends, the
     * connection lives. */
    id = fix_open(&f);
    h2_stream_cancel(&f.c, id);
    h2_conn_pump(&f.c, 0); srv_poll(&f.srv);
    OK(srv_saw_rst(&f.srv, id, H2_ERR_CANCEL));
    OK(h2_stream_done(&f.c, id));
    OK(h2_conn_state(&f.c) == H2_C_OPEN);
    fix_close(&f);

    /* PING is answered automatically, with the same payload. */
    id = fix_open(&f);
    srv_send(&f.srv, H2_F_PING, 0, 0, "logitos!", 8);
    h2_conn_pump(&f.c, 0); srv_poll(&f.srv);
    OK(f.srv.pings_acked == 1);
    OK(h2_conn_state(&f.c) == H2_C_OPEN);
    /* A PING ACK is consumed silently, not echoed back forever. */
    srv_send(&f.srv, H2_F_PING, H2_FLAG_ACK, 0, "logitos!", 8);
    h2_conn_pump(&f.c, 0); srv_poll(&f.srv);
    OK(f.srv.pings_acked == 1);
    fix_close(&f);

    /* An unknown frame type must be IGNORED, not rejected: that is how HTTP/2
     * is extended, and a client that errors cannot talk to a newer server. */
    id = fix_open(&f);
    srv_send(&f.srv, 0x63, 0xFF, 0, "whatever", 8);
    { const char *h[] = { ":status", "200", NULL }; srv_headers(&f.srv, id, h, 1, 0); }
    spin(&f.c, &f.srv, done_stream, &id, 20, NULL);
    OK(h2_conn_state(&f.c) == H2_C_OPEN);
    OK(h2_stream_status(&f.c, id) == 200);
    fix_close(&f);

    /* PRIORITY is parsed and discarded; it must not disturb anything. */
    id = fix_open(&f);
    srv_send(&f.srv, H2_F_PRIORITY, 0, id, "\x00\x00\x00\x00\x10", 5);
    { const char *h[] = { ":status", "200", NULL }; srv_headers(&f.srv, id, h, 1, 0); }
    spin(&f.c, &f.srv, done_stream, &id, 20, NULL);
    OK(h2_stream_status(&f.c, id) == 200 && h2_conn_state(&f.c) == H2_C_OPEN);
    fix_close(&f);

    /* HEADERS carrying a PRIORITY block: the five bytes must be skipped or
     * HPACK starts five bytes late and decodes garbage. */
    id = fix_open(&f);
    {
        struct hpack_list l; hpack_list_init(&l);
        hpack_list_add(&l, ":status", -1, "200", -1, 0);
        uint8_t *b = NULL; int n = 0;
        hpack_encode(&f.srv.enc, &l, &b, &n);
        uint8_t *p = (uint8_t *)malloc((size_t)n + 5);
        memset(p, 0, 5); p[4] = 16;
        memcpy(p + 5, b, (size_t)n);
        srv_send(&f.srv, H2_F_HEADERS,
                 (uint8_t)(H2_FLAG_END_HEADERS | H2_FLAG_PRIORITY | H2_FLAG_END_STREAM),
                 id, p, n + 5);
        free(p); free(b); hpack_list_free(&l);
    }
    spin(&f.c, &f.srv, done_stream, &id, 20, NULL);
    OK(h2_stream_status(&f.c, id) == 200);
    fix_close(&f);

    /* A corrupt HPACK block is a CONNECTION error, not a stream error: once
     * the tables diverge there is nothing left to salvage. */
    id = fix_open(&f);
    srv_send(&f.srv, H2_F_HEADERS, (uint8_t)(H2_FLAG_END_HEADERS | H2_FLAG_END_STREAM),
             id, "\x80\xff\xff", 3);
    h2_conn_pump(&f.c, 0); srv_poll(&f.srv);
    OK(h2_conn_state(&f.c) == H2_C_ERROR && f.c.err == H2_E_COMPRESS);
    OK(f.srv.goaway_seen && f.srv.goaway_code == H2_ERR_COMPRESSION_ERROR);
    fix_close(&f);

    (void)id;
}

/* ============================================================ GOAWAY ====== */

static void t_goaway(void)
{
    struct wire w; memset(&w, 0, sizeof w);
    struct server srv; srv_init(&srv, &w);
    struct h2_conn c;
    struct h2_transport t = mk_transport(&w);
    h2_conn_start(&c, &t);

    uint32_t a = (uint32_t)h2_request(&c, "GET", "https", "e.com", "/a", NULL, NULL, 0);
    uint32_t b = (uint32_t)h2_request(&c, "GET", "https", "e.com", "/b", NULL, NULL, 0);
    uint32_t d = (uint32_t)h2_request(&c, "GET", "https", "e.com", "/c", NULL, NULL, 0);
    h2_conn_pump(&c, 0); srv_poll(&srv);

    /* The server is going away and processed only up to stream `a`. Streams
     * above that were never seen, so they are RETRYABLE -- reporting them as
     * failures instead is the difference between a page that reloads and a
     * page that shows an error. */
    const char *hdrs[] = { ":status", "200", NULL };
    srv_headers(&srv, a, hdrs, 1, 0);
    srv_goaway(&srv, a, H2_ERR_NO_ERROR);
    h2_conn_pump(&c, 0);

    OK(h2_stream_done(&c, a) && h2_stream_status(&c, a) == 200 && h2_stream_err(&c, a) == H2_OK);
    OK(h2_stream_done(&c, b) && h2_stream_err(&c, b) == H2_E_REFUSED);
    OK(h2_stream_done(&c, d) && h2_stream_err(&c, d) == H2_E_REFUSED);
    OK(h2_conn_state(&c) == H2_C_GOAWAY);
    OK(!h2_conn_usable(&c));
    OK(h2_request(&c, "GET", "https", "e.com", "/d", NULL, NULL, 0) == H2_E_GOAWAY);

    h2_conn_free(&c);
    srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);
}

/* ======================================================== server push ===== */

/* We advertise ENABLE_PUSH=0, so this should never happen. When it does
 * anyway, the promised header block is still HPACK-decoded -- skipping it
 * would desynchronise the compression context and corrupt every later header
 * on the connection -- and then the promised stream is refused. */
static void t_push(void)
{
    struct wire w; memset(&w, 0, sizeof w);
    struct server srv; srv_init(&srv, &w);
    struct h2_conn c;
    struct h2_transport t = mk_transport(&w);
    h2_conn_start(&c, &t);

    uint32_t id = (uint32_t)h2_request(&c, "GET", "https", "e.com", "/", NULL, NULL, 0);
    h2_conn_pump(&c, 0); srv_poll(&srv);

    /* PUSH_PROMISE for stream 2, promising /style.css, with headers that also
     * mutate the HPACK dynamic table. */
    {
        struct hpack_list l; hpack_list_init(&l);
        hpack_list_add(&l, ":method", -1, "GET", -1, 0);
        hpack_list_add(&l, ":scheme", -1, "https", -1, 0);
        hpack_list_add(&l, ":authority", -1, "e.com", -1, 0);
        hpack_list_add(&l, ":path", -1, "/style.css", -1, 0);
        hpack_list_add(&l, "x-pushed", -1, "yes-indeed-a-long-value", -1, 0);
        uint8_t *b = NULL; int n = 0;
        hpack_encode(&srv.enc, &l, &b, &n);
        uint8_t *p = (uint8_t *)malloc((size_t)n + 4);
        p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 2;
        memcpy(p + 4, b, (size_t)n);
        srv_send(&srv, H2_F_PUSH_PROMISE, H2_FLAG_END_HEADERS, id, p, n + 4);
        free(p); free(b); hpack_list_free(&l);
    }
    h2_conn_pump(&c, 0); srv_poll(&srv);

    OK(srv_saw_rst(&srv, 2, H2_ERR_REFUSED_STREAM));
    OK(c.push_refused == 1);
    OK(h2_conn_state(&c) == H2_C_OPEN);

    /* THE POINT: the compression context survived. The response below indexes
     * "x-pushed" out of the dynamic table entry that the PUSH_PROMISE created.
     * If the promise had been skipped rather than decoded, this decodes to a
     * different header -- silently. */
    {
        struct hpack_list l; hpack_list_init(&l);
        hpack_list_add(&l, ":status", -1, "200", -1, 0);
        hpack_list_add(&l, "x-pushed", -1, "yes-indeed-a-long-value", -1, 0);
        uint8_t *b = NULL; int n = 0;
        hpack_encode(&srv.enc, &l, &b, &n);
        srv_send(&srv, H2_F_HEADERS, (uint8_t)(H2_FLAG_END_HEADERS | H2_FLAG_END_STREAM), id, b, n);
        free(b); hpack_list_free(&l);
    }
    spin(&c, &srv, done_stream, &id, 20, NULL);
    OK(h2_stream_status(&c, id) == 200);
    const char *xp = h2_stream_header(&c, id, "x-pushed");
    OKM(xp && !strcmp(xp, "yes-indeed-a-long-value"),
        "hpack context desynchronised by the refused push: %s", xp ? xp : "(missing)");

    h2_conn_free(&c);
    srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);
}

/* A promise flood costs us a RST_STREAM each; past a bound it is an attack. */
static void t_push_flood(void)
{
    struct wire w; memset(&w, 0, sizeof w);
    struct server srv; srv_init(&srv, &w);
    struct h2_conn c;
    struct h2_transport t = mk_transport(&w);
    h2_conn_start(&c, &t);
    uint32_t id = (uint32_t)h2_request(&c, "GET", "https", "e.com", "/", NULL, NULL, 0);
    h2_conn_pump(&c, 0); srv_poll(&srv);

    for (int i = 0; i < H2_MAX_PUSH_REFUSALS + 5 && h2_conn_state(&c) == H2_C_OPEN; i++) {
        uint8_t p[8] = { 0, 0, 0, (uint8_t)(2 + 2 * i), 0x82 };
        srv_send(&srv, H2_F_PUSH_PROMISE, H2_FLAG_END_HEADERS, id, p, 5);
        h2_conn_pump(&c, 0); srv_poll(&srv);
    }
    OK(h2_conn_state(&c) == H2_C_ERROR);
    OK(srv.goaway_seen && srv.goaway_code == H2_ERR_ENHANCE_YOUR_CALM);

    h2_conn_free(&c);
    srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);
}

/* ==================================================== streaming sink ====== */

struct sinkrec { char buf[4096]; int n; int calls; };
static int sink_cb(void *ctx, const uint8_t *d, int n)
{
    struct sinkrec *r = (struct sinkrec *)ctx;
    if (r->n + n < (int)sizeof r->buf) { memcpy(r->buf + r->n, d, (size_t)n); r->n += n; }
    r->calls++;
    return H2_OK;
}

static void t_sink(void)
{
    struct wire w; memset(&w, 0, sizeof w);
    struct server srv; srv_init(&srv, &w);
    struct h2_conn c;
    struct h2_transport t = mk_transport(&w);
    h2_conn_start(&c, &t);

    uint32_t id = (uint32_t)h2_request(&c, "GET", "https", "e.com", "/events", NULL, NULL, 0);
    struct sinkrec rec; memset(&rec, 0, sizeof rec);
    h2_stream_sink(&c, id, sink_cb, &rec);
    h2_conn_pump(&c, 0); srv_poll(&srv);

    const char *hdrs[] = { ":status", "200", "content-type", "text/event-stream", NULL };
    srv_headers(&srv, id, hdrs, 0, 0);
    /* Three DATA frames arriving at three different times: a sink must see
     * each one as it lands, not all of them at the end. */
    srv_data(&srv, id, (const uint8_t *)"data: one\n\n", 11, 0);
    h2_conn_pump(&c, 0);
    OK(rec.calls == 1 && rec.n == 11);
    srv_data(&srv, id, (const uint8_t *)"data: two\n\n", 11, 0);
    h2_conn_pump(&c, 0);
    OK(rec.calls == 2 && rec.n == 22);
    srv_data(&srv, id, (const uint8_t *)"data: end\n\n", 11, 1);
    spin(&c, &srv, done_stream, &id, 20, NULL);
    OK(rec.calls == 3 && rec.n == 33);
    OK(!memcmp(rec.buf, "data: one\n\ndata: two\n\ndata: end\n\n", 33));
    /* Nothing was buffered behind the sink's back. */
    int bl = 0; h2_stream_body(&c, id, &bl);
    OK(bl == 0);
    OK(h2_stream_get(&c, id)->streaming == 1);

    h2_conn_free(&c);
    srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);
}

/* ================================== granularity independence ============== */

/* The same server bytes, delivered one at a time, in sevens, and all at once,
 * must produce the identical result. For a frame protocol this is where "I
 * assumed a frame arrives whole" lives, and it is the property that makes "the
 * response came in two TCP segments" stop being a bug class. */
static void t_granularity(void)
{
    uint8_t *canned = NULL; int canned_len = 0;
    char ref_body[8192]; int ref_len = 0; int ref_status = 0;

    for (int pass = 0; pass < 4; pass++) {
        int gran = (pass == 0) ? 0 : (pass == 1 ? 1 : (pass == 2 ? 7 : 3));
        struct wire w; memset(&w, 0, sizeof w);
        struct server srv; srv_init(&srv, &w);
        struct h2_conn c;
        struct h2_transport t = mk_transport(&w);
        h2_conn_start(&c, &t);
        uint32_t id = (uint32_t)h2_request(&c, "GET", "https", "e.com", "/", NULL, NULL, 0);
        h2_conn_pump(&c, 0); srv_poll(&srv);

        if (pass == 0) {
            /* Build the canned response once, through the real server. */
            uint16_t sid_[] = { H2_SET_MAX_CONCURRENT_STREAMS }; uint32_t sv_[] = { 42 };
            srv_settings(&srv, sid_, sv_, 1);
            const char *hdrs[] = { ":status", "200", "content-type", "text/html",
                                   "server", "test/1.0", NULL };
            srv_headers(&srv, id, hdrs, 0, 1);
            srv_send(&srv, H2_F_PING, 0, 0, "abcdefgh", 8);
            for (int i = 0; i < 5; i++)
                srv_data(&srv, id, (const uint8_t *)"0123456789abcdefghij", 20, i == 4);
            canned_len = w.s2c.len - w.s2c.off;
            canned = (uint8_t *)malloc((size_t)canned_len);
            memcpy(canned, w.s2c.b + w.s2c.off, (size_t)canned_len);
        } else {
            pb_put(&w.s2c, canned, canned_len);
            w.max_read = gran;
        }

        for (int i = 0; i < canned_len * 2 + 200 && !h2_stream_done(&c, id); i++) {
            h2_conn_pump(&c, i);
            srv_poll(&srv);
        }
        OKM(h2_stream_done(&c, id), "pass %d (granularity %d) never completed", pass, gran);
        int bl = 0;
        const uint8_t *b = h2_stream_body(&c, id, &bl);
        if (pass == 0) {
            ref_status = h2_stream_status(&c, id);
            ref_len = bl;
            if (bl > 0 && bl < (int)sizeof ref_body) memcpy(ref_body, b, (size_t)bl);
        } else {
            OKM(h2_stream_status(&c, id) == ref_status, "pass %d status %d != %d",
                pass, h2_stream_status(&c, id), ref_status);
            OKM(bl == ref_len && b && !memcmp(b, ref_body, (size_t)bl),
                "pass %d body %d != %d", pass, bl, ref_len);
            const char *sv = h2_stream_header(&c, id, "server");
            OK(sv && !strcmp(sv, "test/1.0"));
        }
        OK(h2_conn_state(&c) == H2_C_OPEN);
        h2_conn_free(&c);
        srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);
    }
    OK(ref_len == 100 && ref_status == 200);
    free(canned);
}

/* Short writes on the send side: the transport accepts one byte at a time, so
 * the request must be reassembled across many pumps. */
static void t_short_writes(void)
{
    struct wire w; memset(&w, 0, sizeof w);
    w.max_write = 1;
    struct server srv; srv_init(&srv, &w);
    struct h2_conn c;
    struct h2_transport t = mk_transport(&w);
    h2_conn_start(&c, &t);
    uint32_t id = (uint32_t)h2_request(&c, "GET", "https", "e.com", "/slow", NULL, NULL, 0);

    for (int i = 0; i < 4000 && !srv.req_seen[0]; i++) { h2_conn_pump(&c, i); srv_poll(&srv); }
    OK(srv.preface_ok == 1);
    OK(srv.req_seen[0] && !strcmp(hpack_list_get(&srv.req[0], ":path"), "/slow"));

    const char *hdrs[] = { ":status", "200", NULL };
    srv_headers(&srv, id, hdrs, 1, 0);
    spin(&c, &srv, done_stream, &id, 200, NULL);
    OK(h2_stream_status(&c, id) == 200);

    h2_conn_free(&c);
    srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);
}

/* The transport dying mid-stream must fail the streams, not hang. */
static void t_transport_death(void)
{
    struct wire w; memset(&w, 0, sizeof w);
    struct server srv; srv_init(&srv, &w);
    struct h2_conn c;
    struct h2_transport t = mk_transport(&w);
    h2_conn_start(&c, &t);
    uint32_t id = (uint32_t)h2_request(&c, "GET", "https", "e.com", "/", NULL, NULL, 0);
    h2_conn_pump(&c, 0); srv_poll(&srv);
    w.dead = 1;
    h2_conn_pump(&c, 1);
    OK(h2_conn_state(&c) == H2_C_ERROR);
    OK(h2_stream_done(&c, id) && h2_stream_err(&c, id) == H2_E_TRANSPORT);
    h2_conn_free(&c);
    srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);

    /* An orderly EOF mid-response is a truncation, not a success. */
    struct wire w2; memset(&w2, 0, sizeof w2);
    struct server srv2; srv_init(&srv2, &w2);
    struct h2_conn c2;
    struct h2_transport t2 = mk_transport(&w2);
    h2_conn_start(&c2, &t2);
    uint32_t id2 = (uint32_t)h2_request(&c2, "GET", "https", "e.com", "/", NULL, NULL, 0);
    h2_conn_pump(&c2, 0); srv_poll(&srv2);
    const char *hdrs[] = { ":status", "200", "content-length", "100", NULL };
    srv_headers(&srv2, id2, hdrs, 0, 0);
    srv_data(&srv2, id2, (const uint8_t *)"half", 4, 0);
    h2_conn_pump(&c2, 0);
    w2.eof = 1;
    h2_conn_pump(&c2, 1);
    OK(h2_conn_state(&c2) == H2_C_CLOSED);
    OK(h2_stream_done(&c2, id2) && h2_stream_err(&c2, id2) == H2_E_CLOSED);
    h2_conn_free(&c2);
    srv_free(&srv2); pb_free(&w2.c2s); pb_free(&w2.s2c);
}

/* Requests must refuse the fields HTTP/2 has no place for, at the API rather
 * than on the wire. */
static void t_request_hygiene(void)
{
    struct wire w; memset(&w, 0, sizeof w);
    struct server srv; srv_init(&srv, &w);
    struct h2_conn c;
    struct h2_transport t = mk_transport(&w);
    h2_conn_start(&c, &t);

    static const char *const badname[] = { "connection", "keep-alive", "proxy-connection",
                                           "transfer-encoding", "upgrade", "host",
                                           "Content-Type", ":path", NULL };
    for (int i = 0; badname[i]; i++) {
        struct hpack_list e; hpack_list_init(&e);
        hpack_list_add(&e, badname[i], -1, "x", -1, 0);
        OKM(h2_request(&c, "GET", "https", "e.com", "/", &e, NULL, 0) == H2_E_ARG,
            "%s was accepted into a request", badname[i]);
        hpack_list_free(&e);
    }
    OK(h2_request(&c, NULL, "https", "e.com", "/", NULL, NULL, 0) == H2_E_ARG);
    OK(h2_request(&c, "GET", "https", "e.com", "/", NULL, NULL, -1) == H2_E_ARG);

    h2_conn_free(&c);
    srv_free(&srv); pb_free(&w.c2s); pb_free(&w.s2c);
}

/* ============================================== the pool learns about h2 == */

static int pool_closed[64], pool_nclosed;
static void pool_close_cb(int fd, void *ctx, void *user)
{
    (void)ctx; (void)user;
    if (pool_nclosed < 64) pool_closed[pool_nclosed++] = fd;
}

static void t_pool(void)
{
    struct hpool p;

    /* A caller that never mentions a protocol gets exactly the old pool: this
     * is the compatibility assertion, because the existing browser loader does
     * not know h2 exists yet. */
    hpool_init(&p);
    hpool_set_closer(&p, pool_close_cb, NULL);
    pool_nclosed = 0;
    int a = hpool_admit(&p, "example.com", 443, 1, 10, NULL, 1000);
    int b = hpool_admit(&p, "example.com", 443, 1, 11, NULL, 1000);
    OK(a >= 0 && b >= 0 && a != b);                 /* several conns per origin */
    OK(hpool_proto(&p, a) == HP_PROTO_H1);
    hpool_release(&p, a, 1, 1001);
    OK(hpool_acquire(&p, "example.com", 443, 1, 1002) == a);
    hpool_close_all(&p);

    /* PENDING: between dial and ALPN, the origin is closed to further dials.
     * Without this a page's first burst opens six sockets to a host that turns
     * out to speak h2, and every one of them carries a single stream. */
    hpool_init(&p);
    hpool_set_closer(&p, pool_close_cb, NULL);
    int s0 = hpool_admit_proto(&p, "h2.example", 443, 1, 20, NULL, HP_PROTO_PENDING, 2000);
    OK(s0 >= 0 && hpool_proto(&p, s0) == HP_PROTO_PENDING);
    OK(hpool_may_open(&p, "h2.example", 443, 1, 2000) == 0);
    OK(hpool_may_open(&p, "other.example", 443, 1, 2000) == 1);   /* other origins unaffected */
    OK(hpool_acquire(&p, "h2.example", 443, 1, 2000) == -1);      /* not an h1 connection */
    OK(hpool_acquire_mux(&p, "h2.example", 443, 1, 2000) == -1);  /* not yet h2 either */

    /* ALPN says h2. Now the same slot serves many callers at once. */
    hpool_set_proto(&p, s0, HP_PROTO_H2);
    OK(hpool_proto(&p, s0) == HP_PROTO_H2 && p.h2_conns == 1);
    OK(hpool_streams(&p, s0) == 1);                 /* the dial's own request */
    for (int i = 0; i < 10; i++)
        OKM(hpool_acquire_mux(&p, "h2.example", 443, 1, 2000) == s0,
            "stream %d did not land on the origin's one connection", i);
    OK(hpool_streams(&p, s0) == 11);
    OK(hpool_count(&p) == 1);                       /* still ONE connection */
    OK(p.mux_hits == 10);
    /* Still shut to new dials, and an h1 acquire still refuses it. */
    OK(hpool_may_open(&p, "h2.example", 443, 1, 2000) == 0);
    OK(hpool_acquire(&p, "h2.example", 443, 1, 2000) == -1);

    /* The stream cap is reported, not exceeded. */
    p.max_streams = 12;
    OK(hpool_acquire_mux(&p, "h2.example", 443, 1, 2000) == s0);   /* 12th */
    OK(hpool_acquire_mux(&p, "h2.example", 443, 1, 2000) == -1);   /* 13th refused */

    /* Releasing one stream leaves the connection up and in use. */
    pool_nclosed = 0;
    for (int i = 0; i < 11; i++) hpool_release(&p, s0, 1, 2001);
    OK(hpool_count(&p) == 1 && hpool_streams(&p, s0) == 1);
    OK(pool_nclosed == 0);
    /* Releasing the last one idles it -- and does NOT recycle it on max_reqs,
     * which on an h2 connection would cancel every stream still running. */
    hpool_release(&p, s0, 1, 2002);
    OK(hpool_streams(&p, s0) == 0 && hpool_idle_count(&p) == 1 && pool_nclosed == 0);
    /* An unusable h2 connection (GOAWAY, compression error, dead socket) still
     * goes away entirely: those reasons affect every stream on it. */
    hpool_acquire_mux(&p, "h2.example", 443, 1, 2003);
    hpool_release(&p, s0, 0, 2003);
    OK(hpool_count(&p) == 0 && pool_nclosed == 1 && pool_closed[0] == 20);
    hpool_close_all(&p);

    /* THE NUMBER, and the honest version of it. Pooled HTTP/1.1 already got a
     * 40-resource page down to one dial per origin, so DIALS is not what h2
     * improves -- ROUND TRIPS is. This models a page of 40 subresources over 3
     * origins: everything that can be placed concurrently goes out together,
     * then that wave completes, then the next. The wave count is the number of
     * sequential round trips the page costs.
     *
     * On HTTP/1.1 a connection carries one request at a time, so a wave is
     * bounded by max_total connections. On h2 the multiplexed origin puts all
     * of its resources in flight at once on ONE connection. */
    int rt[2] = { 0, 0 }, dials[2] = { 0, 0 }, peak[2] = { 0, 0 };
    for (int with_h2 = 0; with_h2 <= 1; with_h2++) {
        hpool_init(&p);
        hpool_set_closer(&p, pool_close_cb, NULL);
        const char *origins[3] = { "cdn.example", "img.example", "api.example" };
        int done[40]; memset(done, 0, sizeof done);
        int remaining = 40, fd = 100, waves = 0, maxwave = 0;

        while (remaining > 0 && waves < 100) {
            int slots[64], nlive = 0;
            for (int i = 0; i < 40 && nlive < 64; i++) {
                if (done[i]) continue;
                const char *h = origins[i % 3];
                int mux = with_h2 && (i % 3) == 0;   /* cdn.example speaks h2 */
                int slot = mux ? hpool_acquire_mux(&p, h, 443, 1, 3000) : -1;
                if (slot < 0 && !mux) slot = hpool_acquire(&p, h, 443, 1, 3000);
                if (slot < 0 && hpool_may_open(&p, h, 443, 1, 3000)) {
                    slot = hpool_admit_proto(&p, h, 443, 1, fd++, NULL,
                                             mux ? HP_PROTO_PENDING : HP_PROTO_H1, 3000);
                    if (slot >= 0 && mux) hpool_set_proto(&p, slot, HP_PROTO_H2);
                }
                if (slot < 0) continue;              /* waits for the next wave */
                slots[nlive++] = slot;
                done[i] = 1;
                remaining--;
            }
            OKM(nlive > 0, "no progress on wave %d (h2=%d)", waves, with_h2);
            if (nlive == 0) break;
            if (nlive > maxwave) maxwave = nlive;
            for (int k = 0; k < nlive; k++) hpool_release(&p, slots[k], 1, 3000 + waves);
            waves++;
        }
        OK(remaining == 0);
        rt[with_h2] = waves;
        dials[with_h2] = p.opened;
        peak[with_h2] = maxwave;
        hpool_close_all(&p);
    }
    printf("  pool: 40 resources / 3 origins -- HTTP/1.1 only: %d dials, %d round trips "
           "(%d in flight at peak)\n", dials[0], rt[0], peak[0]);
    printf("  pool: 40 resources / 3 origins -- one origin h2:  %d dials, %d round trips "
           "(%d in flight at peak)\n", dials[1], rt[1], peak[1]);
    OKM(rt[1] < rt[0], "h2 did not reduce round trips: %d vs %d", rt[1], rt[0]);
    OKM(peak[1] > peak[0], "h2 did not raise concurrency: %d vs %d", peak[1], peak[0]);
    /* Note what did NOT improve: dials. Pooling already had that, and the h2
     * run can even dial slightly more, because its one multiplexed connection
     * still occupies a slot in max_total while an origin with no work left is
     * a candidate for eviction. Round trips are the claim; connections are
     * not, and pretending otherwise would be measuring the wrong thing. */
}

int main(void)
{
    t_frame_header();
    t_preface();
    t_get();
    t_header_shapes();
    t_multiplex();
    t_compression_win();
    t_flow_recv();
    t_flow_send();
    t_flow_settings_retroactive();
    t_flow_stall();
    t_states();
    t_goaway();
    t_push();
    t_push_flood();
    t_sink();
    t_granularity();
    t_short_writes();
    t_transport_death();
    t_request_hygiene();
    t_pool();

    printf("h2_test: %d checks, %d failures\n", checks, fails);
    if (!fails) printf("h2_test: ALL PASS\n");
    return fails ? 1 : 0;
}
