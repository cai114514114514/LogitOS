/* http2.c -- the HTTP/2 client: frames, streams, flow control. See http2.h for
 * the flow-control accounting and the server-push decision.
 *
 * Three things about the shape of this file.
 *
 * IT NEVER BLOCKS.  h2_conn_pump() writes what it can, reads what is there,
 * parses whole frames and returns. Nothing waits, sleeps or polls, for the
 * same reason http1.c's parser does not: a browser loading a page has several
 * requests in flight, and on this machine they share one thread with the
 * window manager. It also makes "deliver this response one byte at a time" a
 * three-line test rather than a network setup.
 *
 * A HEADER BLOCK IS DECODED EVEN WHEN NOBODY WANTS IT.  HEADERS for a stream
 * we have already released, and PUSH_PROMISE for a push we are about to
 * refuse, still go through HPACK. Skipping them would leave our dynamic table
 * a different size from the peer's, and from that moment every indexed header
 * on the connection decodes to the wrong field -- silently. That is why the
 * decode is unconditional and why an HPACK failure kills the connection rather
 * than the stream.
 *
 * EVERY LENGTH FROM THE WIRE IS CHECKED AGAINST WHAT ARRIVED.  Frame length
 * against our SETTINGS_MAX_FRAME_SIZE before anything is buffered; pad length
 * against the payload that actually contains it (a pad length >= the payload
 * is the classic HTTP/2 out-of-bounds read); fixed-size frames against their
 * exact size; the accumulated header block against a cap. The peer chooses all
 * of these numbers.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "http2.h"

const char *h2_strerror(int err)
{
    switch (err) {
    case H2_OK:          return "ok";
    case H2_E_PROTO:     return "protocol error";
    case H2_E_FRAMESIZE: return "frame size error";
    case H2_E_COMPRESS:  return "hpack failure (connection unrecoverable)";
    case H2_E_FLOW:      return "flow control error";
    case H2_E_TRANSPORT: return "transport failure";
    case H2_E_NOMEM:     return "out of memory";
    case H2_E_ARG:       return "bad argument";
    case H2_E_GOAWAY:    return "peer sent GOAWAY";
    case H2_E_REFUSED:   return "stream refused (retryable)";
    case H2_E_RESET:     return "stream reset by peer";
    case H2_E_TOOLARGE:  return "exceeded a configured bound";
    case H2_E_STALL:     return "flow control window never opened";
    case H2_E_CLOSED:    return "connection closed";
    case H2_E_NOSLOT:    return "no stream slot available";
    default:             return "http2 error";
    }
}

const char *h2_errcode_name(uint32_t code)
{
    switch (code) {
    case H2_ERR_NO_ERROR:            return "NO_ERROR";
    case H2_ERR_PROTOCOL_ERROR:      return "PROTOCOL_ERROR";
    case H2_ERR_INTERNAL_ERROR:      return "INTERNAL_ERROR";
    case H2_ERR_FLOW_CONTROL_ERROR:  return "FLOW_CONTROL_ERROR";
    case H2_ERR_SETTINGS_TIMEOUT:    return "SETTINGS_TIMEOUT";
    case H2_ERR_STREAM_CLOSED:       return "STREAM_CLOSED";
    case H2_ERR_FRAME_SIZE_ERROR:    return "FRAME_SIZE_ERROR";
    case H2_ERR_REFUSED_STREAM:      return "REFUSED_STREAM";
    case H2_ERR_CANCEL:              return "CANCEL";
    case H2_ERR_COMPRESSION_ERROR:   return "COMPRESSION_ERROR";
    case H2_ERR_CONNECT_ERROR:       return "CONNECT_ERROR";
    case H2_ERR_ENHANCE_YOUR_CALM:   return "ENHANCE_YOUR_CALM";
    case H2_ERR_INADEQUATE_SECURITY: return "INADEQUATE_SECURITY";
    case H2_ERR_HTTP_1_1_REQUIRED:   return "HTTP_1_1_REQUIRED";
    default:                         return "UNKNOWN";
    }
}

const char *h2_state_name(int s)
{
    switch (s) {
    case H2_S_IDLE:               return "idle";
    case H2_S_RESERVED_REMOTE:    return "reserved(remote)";
    case H2_S_OPEN:               return "open";
    case H2_S_HALF_CLOSED_LOCAL:  return "half-closed(local)";
    case H2_S_HALF_CLOSED_REMOTE: return "half-closed(remote)";
    case H2_S_CLOSED:             return "closed";
    default:                      return "?";
    }
}

/* ------------------------------------------------------------- frame bytes */

int h2_frame_parse(const uint8_t *p, uint32_t *len, uint8_t *type,
                   uint8_t *flags, uint32_t *stream)
{
    if (!p || !len || !type || !flags || !stream) return H2_E_ARG;
    *len    = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    *type   = p[3];
    *flags  = p[4];
    /* Bit 31 is reserved and MUST be ignored on receipt -- not rejected. An
     * implementation that treats it as part of the id reads stream
     * 0x80000001 and calls a perfectly ordinary frame a protocol error. */
    *stream = (((uint32_t)p[5] << 24) | ((uint32_t)p[6] << 16) |
               ((uint32_t)p[7] << 8) | p[8]) & 0x7FFFFFFFu;
    return H2_OK;
}

void h2_frame_write(uint8_t *p, uint32_t len, uint8_t type, uint8_t flags, uint32_t stream)
{
    p[0] = (uint8_t)((len >> 16) & 0xFF);
    p[1] = (uint8_t)((len >> 8) & 0xFF);
    p[2] = (uint8_t)(len & 0xFF);
    p[3] = type;
    p[4] = flags;
    stream &= 0x7FFFFFFFu;
    p[5] = (uint8_t)((stream >> 24) & 0xFF);
    p[6] = (uint8_t)((stream >> 16) & 0xFF);
    p[7] = (uint8_t)((stream >> 8) & 0xFF);
    p[8] = (uint8_t)(stream & 0xFF);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

/* ------------------------------------------------------------- tx queueing */

static int buf_reserve(uint8_t **b, int *cap, int need)
{
    if (need <= *cap) return H2_OK;
    int ncap = *cap ? *cap : 1024;
    while (ncap < need) {
        if (ncap > (1 << 28)) return H2_E_NOMEM;
        ncap *= 2;
    }
    uint8_t *nb = (uint8_t *)realloc(*b, (size_t)ncap);
    if (!nb) return H2_E_NOMEM;
    *b = nb; *cap = ncap;
    return H2_OK;
}

static int tx_push(struct h2_conn *c, const void *data, int len)
{
    if (len <= 0) return H2_OK;
    /* Compact first: the queue is drained from the front, so without this a
     * long-lived connection grows its send buffer forever. */
    if (c->tx_off > 0 && c->tx_off == c->tx_len) { c->tx_off = c->tx_len = 0; }
    else if (c->tx_off > 4096) {
        memmove(c->tx, c->tx + c->tx_off, (size_t)(c->tx_len - c->tx_off));
        c->tx_len -= c->tx_off; c->tx_off = 0;
    }
    if (buf_reserve(&c->tx, &c->tx_cap, c->tx_len + len) != H2_OK) return H2_E_NOMEM;
    memcpy(c->tx + c->tx_len, data, (size_t)len);
    c->tx_len += len;
    return H2_OK;
}

static int tx_frame(struct h2_conn *c, uint8_t type, uint8_t flags, uint32_t sid,
                    const void *payload, int len)
{
    uint8_t h[H2_FRAME_HDR];
    if (len < 0) return H2_E_ARG;
    h2_frame_write(h, (uint32_t)len, type, flags, sid);
    if (tx_push(c, h, H2_FRAME_HDR) != H2_OK) return H2_E_NOMEM;
    if (len && tx_push(c, payload, len) != H2_OK) return H2_E_NOMEM;
    c->frames_out++;
    return H2_OK;
}

/* --------------------------------------------------------------- streams */

static struct h2_stream *find_stream(struct h2_conn *c, uint32_t id)
{
    if (!id) return NULL;
    for (int i = 0; i < H2_MAX_STREAMS; i++)
        if (c->st[i].used && c->st[i].id == id) return &c->st[i];
    return NULL;
}

struct h2_stream *h2_stream_get(struct h2_conn *c, uint32_t id)
{
    return c ? find_stream(c, id) : NULL;
}

static void stream_clear(struct h2_stream *s)
{
    hpack_list_free(&s->hdr);
    hpack_list_free(&s->trailer);
    free(s->body);
    memset(s, 0, sizeof *s);
}

static void stream_finish(struct h2_conn *c, struct h2_stream *s, int err)
{
    if (s->done) return;
    s->done = 1;
    if (err && !s->err) s->err = err;
    if (s->state != H2_S_CLOSED) { s->state = H2_S_CLOSED; if (c->concurrent > 0) c->concurrent--; }
    c->streams_done++;
}

/* ------------------------------------------------------- connection errors */

/* A connection error is terminal: queue GOAWAY with the wire code, remember
 * the local reason, and fail every live stream. Callers return immediately
 * after -- there is no "carry on and see". */
static int conn_fail(struct h2_conn *c, uint32_t wire, int err)
{
    if (c->state == H2_C_ERROR) return c->err;
    uint8_t p[8];
    uint32_t last = 0;
    for (int i = 0; i < H2_MAX_STREAMS; i++)
        if (c->st[i].used && c->st[i].id > last) last = c->st[i].id;
    p[0] = (uint8_t)(last >> 24); p[1] = (uint8_t)(last >> 16);
    p[2] = (uint8_t)(last >> 8);  p[3] = (uint8_t)last;
    p[4] = (uint8_t)(wire >> 24); p[5] = (uint8_t)(wire >> 16);
    p[6] = (uint8_t)(wire >> 8);  p[7] = (uint8_t)wire;
    tx_frame(c, H2_F_GOAWAY, 0, 0, p, 8);
    c->sent_goaway = wire;
    c->state = H2_C_ERROR;
    c->err = err;
    for (int i = 0; i < H2_MAX_STREAMS; i++)
        if (c->st[i].used && !c->st[i].done) stream_finish(c, &c->st[i], err);
    return err;
}

/* A stream error kills one stream and leaves the connection running. The
 * distinction is the whole point of HTTP/2's error model and getting it wrong
 * in the "connection" direction turns one bad response into a dead page. */
static void stream_fail(struct h2_conn *c, struct h2_stream *s, uint32_t wire, int err)
{
    if (!s) return;
    if (!s->rst_sent && s->id) {
        uint8_t p[4] = { (uint8_t)(wire >> 24), (uint8_t)(wire >> 16),
                         (uint8_t)(wire >> 8), (uint8_t)wire };
        tx_frame(c, H2_F_RST_STREAM, 0, s->id, p, 4);
        s->rst_sent = 1;
        c->rst_out++;
    }
    stream_finish(c, s, err);
}

/* Refuse a stream id we are not tracking, without allocating a slot for it. */
static void rst_id(struct h2_conn *c, uint32_t id, uint32_t wire)
{
    uint8_t p[4] = { (uint8_t)(wire >> 24), (uint8_t)(wire >> 16),
                     (uint8_t)(wire >> 8), (uint8_t)wire };
    tx_frame(c, H2_F_RST_STREAM, 0, id, p, 4);
    c->rst_out++;
}

/* ------------------------------------------------------- flow control (rx) */

/* Replenish what we have consumed. The threshold matters: acknowledging every
 * frame wastes a frame per DATA, and acknowledging never is the deadlock. Half
 * the window keeps at least half of it available at all times, so the peer is
 * never idle waiting for us. */
static void ack_window(struct h2_conn *c, struct h2_stream *s)
{
    uint8_t p[4];
    if (c->recv_unacked >= H2_OUR_CONN_WINDOW / 2) {
        uint32_t n = (uint32_t)c->recv_unacked;
        p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16);
        p[2] = (uint8_t)(n >> 8);  p[3] = (uint8_t)n;
        tx_frame(c, H2_F_WINDOW_UPDATE, 0, 0, p, 4);
        c->recv_win += c->recv_unacked;
        c->recv_unacked = 0;
        c->window_updates_out++;
    }
    if (s && !s->done && s->recv_unacked >= H2_OUR_STREAM_WINDOW / 2) {
        uint32_t n = (uint32_t)s->recv_unacked;
        p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16);
        p[2] = (uint8_t)(n >> 8);  p[3] = (uint8_t)n;
        tx_frame(c, H2_F_WINDOW_UPDATE, 0, s->id, p, 4);
        s->recv_win += s->recv_unacked;
        s->recv_unacked = 0;
        c->window_updates_out++;
    }
}

/* Called when a stream ends: whatever it still owes on the CONNECTION window
 * must be returned, or a page of finished streams leaks the whole window and
 * the next transfer stalls at 65535 bytes with no error anywhere. */
static void flush_conn_window(struct h2_conn *c)
{
    if (c->recv_unacked <= 0) return;
    uint32_t n = (uint32_t)c->recv_unacked;
    uint8_t p[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    tx_frame(c, H2_F_WINDOW_UPDATE, 0, 0, p, 4);
    c->recv_win += c->recv_unacked;
    c->recv_unacked = 0;
    c->window_updates_out++;
}

/* --------------------------------------------------------------- lifecycle */

int h2_conn_start(struct h2_conn *c, const struct h2_transport *t)
{
    if (!c || !t || !t->read || !t->write) return H2_E_ARG;
    memset(c, 0, sizeof *c);
    c->t = *t;
    hpack_dec_init(&c->dec, HPACK_DEFAULT_CAP);
    hpack_enc_init(&c->enc, HPACK_DEFAULT_CAP);

    c->peer_max_frame     = H2_DEFAULT_MAX_FRAME;
    c->peer_max_conc      = H2_MAX_STREAMS;      /* until the peer says otherwise */
    c->peer_hdr_table     = HPACK_DEFAULT_CAP;
    c->peer_max_hdr_list  = 0xFFFFFFFFu;
    c->peer_init_win      = 65535;
    c->send_win           = 65535;
    c->recv_win           = 65535;
    c->next_id            = 1;                   /* client streams are odd */
    c->stall_ms           = 30000;
    c->state              = H2_C_OPEN;

    if (tx_push(c, H2_PREFACE, H2_PREFACE_LEN) != H2_OK) return H2_E_NOMEM;

    /* Our SETTINGS. ENABLE_PUSH=0 first, because it is the one that changes
     * what the peer is allowed to do to us. */
    uint8_t s[6 * 6];
    int n = 0;
    struct { uint16_t id; uint32_t v; } sv[] = {
        { H2_SET_ENABLE_PUSH,            0 },
        { H2_SET_MAX_CONCURRENT_STREAMS, H2_MAX_STREAMS },
        { H2_SET_INITIAL_WINDOW_SIZE,    H2_OUR_STREAM_WINDOW },
        { H2_SET_MAX_FRAME_SIZE,         H2_OUR_MAX_FRAME },
        { H2_SET_MAX_HEADER_LIST_SIZE,   H2_HEADER_BLOCK_MAX },
        { H2_SET_HEADER_TABLE_SIZE,      HPACK_DEFAULT_CAP }
    };
    for (unsigned i = 0; i < sizeof sv / sizeof sv[0]; i++) {
        s[n++] = (uint8_t)(sv[i].id >> 8); s[n++] = (uint8_t)sv[i].id;
        s[n++] = (uint8_t)(sv[i].v >> 24); s[n++] = (uint8_t)(sv[i].v >> 16);
        s[n++] = (uint8_t)(sv[i].v >> 8);  s[n++] = (uint8_t)sv[i].v;
    }
    if (tx_frame(c, H2_F_SETTINGS, 0, 0, s, n) != H2_OK) return H2_E_NOMEM;
    c->settings_ack_pending = 1;

    /* SETTINGS_INITIAL_WINDOW_SIZE does NOT apply to the connection window --
     * that is a real asymmetry in RFC 7540 section 6.9.2, and the only way to
     * raise the connection window above 65535 is an explicit WINDOW_UPDATE on
     * stream 0. Forget this and every multi-stream page stalls at 64 KiB in
     * aggregate no matter how big the per-stream windows are. */
    uint32_t bump = H2_OUR_CONN_WINDOW - 65535;
    uint8_t w[4] = { (uint8_t)(bump >> 24), (uint8_t)(bump >> 16),
                     (uint8_t)(bump >> 8), (uint8_t)bump };
    if (tx_frame(c, H2_F_WINDOW_UPDATE, 0, 0, w, 4) != H2_OK) return H2_E_NOMEM;
    c->recv_win = H2_OUR_CONN_WINDOW;
    c->window_updates_out++;
    return H2_OK;
}

void h2_conn_free(struct h2_conn *c)
{
    if (!c) return;
    for (int i = 0; i < H2_MAX_STREAMS; i++) if (c->st[i].used) stream_clear(&c->st[i]);
    hpack_dec_free(&c->dec);
    hpack_enc_free(&c->enc);
    free(c->rx); free(c->tx); free(c->hblock);
    c->rx = c->tx = c->hblock = NULL;
    c->rx_cap = c->tx_cap = c->hblock_cap = 0;
    c->rx_len = c->tx_len = c->tx_off = c->hblock_len = 0;
    c->state = H2_C_CLOSED;
}

void h2_conn_stall_ms(struct h2_conn *c, int64_t ms) { if (c) c->stall_ms = ms; }
int  h2_conn_state(const struct h2_conn *c) { return c ? c->state : H2_C_CLOSED; }

int h2_conn_usable(const struct h2_conn *c)
{
    if (!c) return 0;
    return c->state == H2_C_OPEN && !c->have_recv_goaway;
}

int h2_conn_active_streams(const struct h2_conn *c)
{
    int n = 0;
    if (!c) return 0;
    for (int i = 0; i < H2_MAX_STREAMS; i++) if (c->st[i].used && !c->st[i].done) n++;
    return n;
}

void h2_conn_goaway(struct h2_conn *c, uint32_t code)
{
    if (!c || c->state == H2_C_ERROR || c->state == H2_C_CLOSED) return;
    uint32_t last = 0;
    for (int i = 0; i < H2_MAX_STREAMS; i++)
        if (c->st[i].used && c->st[i].id > last) last = c->st[i].id;
    uint8_t p[8] = { (uint8_t)(last >> 24), (uint8_t)(last >> 16), (uint8_t)(last >> 8), (uint8_t)last,
                     (uint8_t)(code >> 24), (uint8_t)(code >> 16), (uint8_t)(code >> 8), (uint8_t)code };
    tx_frame(c, H2_F_GOAWAY, 0, 0, p, 8);
    c->sent_goaway = code;
    if (c->state == H2_C_OPEN) c->state = H2_C_GOAWAY;
}

/* ---------------------------------------------------------------- requests */

int h2_request(struct h2_conn *c, const char *method, const char *scheme,
               const char *authority, const char *path,
               const struct hpack_list *extra, const void *body, int blen)
{
    if (!c || !method || !scheme || !authority || !path) return H2_E_ARG;
    if (blen < 0 || (blen && !body)) return H2_E_ARG;
    if (c->state == H2_C_ERROR || c->state == H2_C_CLOSED) return c->err ? c->err : H2_E_CLOSED;
    if (c->state == H2_C_GOAWAY || c->have_recv_goaway) return H2_E_GOAWAY;

    int live = h2_conn_active_streams(c);
    if ((uint32_t)live >= c->peer_max_conc) return H2_E_NOSLOT;

    struct h2_stream *s = NULL;
    for (int i = 0; i < H2_MAX_STREAMS; i++) if (!c->st[i].used) { s = &c->st[i]; break; }
    if (!s) return H2_E_NOSLOT;

    /* Pseudo-header fields come first and in this order. It is not cosmetic:
     * a peer must treat a pseudo-header after a regular one as malformed, and
     * ours is the side that has to get it right. */
    struct hpack_list hl;
    hpack_list_init(&hl);
    int rc = H2_OK;
    if (hpack_list_add(&hl, ":method", -1, method, -1, 0) != HPACK_OK ||
        hpack_list_add(&hl, ":scheme", -1, scheme, -1, 0) != HPACK_OK ||
        hpack_list_add(&hl, ":authority", -1, authority, -1, 0) != HPACK_OK ||
        hpack_list_add(&hl, ":path", -1, path, -1, 0) != HPACK_OK)
        rc = H2_E_NOMEM;
    for (int i = 0; rc == H2_OK && extra && i < extra->n; i++) {
        const struct hpack_hdr *h = &extra->v[i];
        /* Connection-specific fields have no meaning in HTTP/2 and a server
         * must reject them; better to refuse here than to send a request that
         * cannot succeed. */
        if (h->nlen && h->name[0] == ':') { rc = H2_E_ARG; break; }
        if (!strcmp(h->name, "connection") || !strcmp(h->name, "keep-alive") ||
            !strcmp(h->name, "proxy-connection") || !strcmp(h->name, "transfer-encoding") ||
            !strcmp(h->name, "upgrade") || !strcmp(h->name, "host")) { rc = H2_E_ARG; break; }
        for (int k = 0; k < h->nlen; k++)
            if (h->name[k] >= 'A' && h->name[k] <= 'Z') { rc = H2_E_ARG; break; }
        if (rc != H2_OK) break;
        if (hpack_list_add(&hl, h->name, h->nlen, h->value, h->vlen, h->sensitive) != HPACK_OK)
            rc = H2_E_NOMEM;
    }
    if (rc != H2_OK) { hpack_list_free(&hl); return rc; }

    uint8_t *block = NULL; int blocklen = 0;
    if (hpack_encode(&c->enc, &hl, &block, &blocklen) != HPACK_OK) {
        hpack_list_free(&hl);
        return H2_E_COMPRESS;
    }
    hpack_list_free(&hl);

    uint32_t id = c->next_id;
    c->next_id += 2;

    memset(s, 0, sizeof *s);
    s->used = 1;
    s->id = id;
    s->state = H2_S_OPEN;
    s->send_win = c->peer_init_win;
    s->recv_win = H2_OUR_STREAM_WINDOW;
    s->body_max = H2_BODY_MAX;
    hpack_list_init(&s->hdr);
    hpack_list_init(&s->trailer);
    s->req_body = (const uint8_t *)body;
    s->req_len = blen;
    s->req_end_stream = 1;

    /* HEADERS, then CONTINUATION for whatever did not fit. The peer's
     * SETTINGS_MAX_FRAME_SIZE is a hard limit on each fragment; a header block
     * bigger than one frame is normal on a request with cookies. */
    int off = 0;
    int maxf = (int)(c->peer_max_frame ? c->peer_max_frame : H2_DEFAULT_MAX_FRAME);
    int first = 1;
    while (off < blocklen || first) {
        int chunk = blocklen - off;
        if (chunk > maxf) chunk = maxf;
        int last = (off + chunk >= blocklen);
        uint8_t flags = 0;
        if (last) flags |= H2_FLAG_END_HEADERS;
        if (first && last && blen == 0) flags |= H2_FLAG_END_STREAM;
        tx_frame(c, first ? H2_F_HEADERS : H2_F_CONTINUATION, flags, id, block + off, chunk);
        off += chunk;
        first = 0;
        if (last) break;
    }
    free(block);

    if (blen == 0) {
        s->state = H2_S_HALF_CLOSED_LOCAL;
        s->req_end_stream = 0;
    }
    s->req_headers_sent = 1;
    c->streams_opened++;
    c->concurrent++;
    if (c->concurrent > c->max_concurrent_seen) c->max_concurrent_seen = c->concurrent;
    return (int)id;
}

/* Push request-body DATA out as far as both windows allow. Returns 1 if it
 * moved anything. */
static int pump_send_bodies(struct h2_conn *c)
{
    int moved = 0;
    for (int i = 0; i < H2_MAX_STREAMS; i++) {
        struct h2_stream *s = &c->st[i];
        if (!s->used || s->done || !s->req_end_stream) continue;
        if (s->state != H2_S_OPEN) continue;
        for (;;) {
            int remain = s->req_len - s->req_off;
            int64_t win = s->send_win < c->send_win ? s->send_win : c->send_win;
            if (win < 0) win = 0;
            int n = remain;
            if (n > win) n = (int)win;
            if (n > (int)c->peer_max_frame) n = (int)c->peer_max_frame;
            if (n <= 0) break;
            uint8_t flags = (s->req_off + n == s->req_len) ? H2_FLAG_END_STREAM : 0;
            if (tx_frame(c, H2_F_DATA, flags, s->id, s->req_body + s->req_off, n) != H2_OK)
                return moved;
            s->req_off += n;
            s->send_win -= n;
            c->send_win -= n;
            moved = 1;
            if (flags & H2_FLAG_END_STREAM) {
                s->req_end_stream = 0;
                s->state = H2_S_HALF_CLOSED_LOCAL;
                break;
            }
        }
        /* A zero-length body that still owes END_STREAM. */
        if (s->req_end_stream && s->req_len == 0) {
            tx_frame(c, H2_F_DATA, H2_FLAG_END_STREAM, s->id, NULL, 0);
            s->req_end_stream = 0;
            s->state = H2_S_HALF_CLOSED_LOCAL;
            moved = 1;
        }
    }
    return moved;
}

/* Is anything waiting on a shut send window? That is the state the stall
 * timeout exists to bound. */
static int send_blocked(const struct h2_conn *c)
{
    for (int i = 0; i < H2_MAX_STREAMS; i++) {
        const struct h2_stream *s = &c->st[i];
        if (!s->used || s->done || !s->req_end_stream) continue;
        if (s->req_off < s->req_len && (s->send_win <= 0 || c->send_win <= 0)) return 1;
    }
    return 0;
}

/* --------------------------------------------------- response header checks */

/* RFC 9113 section 8.3: a response carries exactly one pseudo-header,
 * :status, before every regular field; field names are lowercase; and the
 * HTTP/1.1 connection-management fields are forbidden outright. A message
 * that breaks any of these is malformed and is a STREAM error -- one bad
 * response must not take the connection down with it. */
static int validate_response(const struct hpack_list *l, int *status, int trailers)
{
    int seen_regular = 0, nstatus = 0;
    *status = 0;
    for (int i = 0; i < l->n; i++) {
        const struct hpack_hdr *h = &l->v[i];
        if (h->nlen == 0) return H2_E_PROTO;
        for (int k = 0; k < h->nlen; k++) {
            unsigned char ch = (unsigned char)h->name[k];
            if (ch >= 'A' && ch <= 'Z') return H2_E_PROTO;
            if (ch <= 0x20 || ch == 0x7F) return H2_E_PROTO;
            if (ch == ':' && k != 0) return H2_E_PROTO;
        }
        for (int k = 0; k < h->vlen; k++) {
            unsigned char ch = (unsigned char)h->value[k];
            if (ch == 0 || ch == '\r' || ch == '\n') return H2_E_PROTO;
        }
        if (h->name[0] == ':') {
            if (trailers) return H2_E_PROTO;          /* no pseudo-fields in trailers */
            if (seen_regular) return H2_E_PROTO;      /* pseudo-fields come first */
            if (strcmp(h->name, ":status")) return H2_E_PROTO;
            if (++nstatus > 1) return H2_E_PROTO;
            if (h->vlen != 3) return H2_E_PROTO;
            int v = 0;
            for (int k = 0; k < 3; k++) {
                if (h->value[k] < '0' || h->value[k] > '9') return H2_E_PROTO;
                v = v * 10 + (h->value[k] - '0');
            }
            *status = v;
            continue;
        }
        seen_regular = 1;
        if (!strcmp(h->name, "connection") || !strcmp(h->name, "keep-alive") ||
            !strcmp(h->name, "proxy-connection") || !strcmp(h->name, "upgrade") ||
            !strcmp(h->name, "transfer-encoding"))
            return H2_E_PROTO;
        /* TE is only legal with the single value "trailers". */
        if (!strcmp(h->name, "te") && strcmp(h->value, "trailers")) return H2_E_PROTO;
    }
    if (!trailers && nstatus != 1) return H2_E_PROTO;
    return H2_OK;
}

/* -------------------------------------------------------- header block done */

static int deliver_headers(struct h2_conn *c, uint32_t sid, int end_stream,
                           struct hpack_list *l)
{
    struct h2_stream *s = find_stream(c, sid);
    if (!s || s->done) { hpack_list_free(l); return H2_OK; }   /* decoded, then dropped */

    int trailers = (s->headers_done != 0);
    int status = 0;
    int rc = validate_response(l, &status, trailers);
    if (rc != H2_OK) {
        hpack_list_free(l);
        stream_fail(c, s, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
        return H2_OK;
    }
    if (trailers) {
        if (!end_stream) {                       /* trailers must end the stream */
            hpack_list_free(l);
            stream_fail(c, s, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
            return H2_OK;
        }
        hpack_list_free(&s->trailer);
        s->trailer = *l;
        hpack_list_init(l);
    } else if (status >= 100 && status < 200) {
        /* An informational response is not the response: 1xx may repeat and
         * the real HEADERS follows. Keeping it would make status 100 the
         * answer to every request that used Expect. */
        if (end_stream) {                        /* ...but 1xx cannot end one */
            hpack_list_free(l);
            stream_fail(c, s, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
            return H2_OK;
        }
        hpack_list_free(l);
        return H2_OK;
    } else {
        hpack_list_free(&s->hdr);
        s->hdr = *l;
        hpack_list_init(l);
        s->status = status;
        s->headers_done = 1;
    }
    s->hdr_blocks++;

    if (end_stream) {
        if (s->state == H2_S_OPEN) s->state = H2_S_HALF_CLOSED_REMOTE;
        stream_finish(c, s, 0);
        flush_conn_window(c);
    }
    return H2_OK;
}

static int finish_header_block(struct h2_conn *c)
{
    struct hpack_list l;
    hpack_list_init(&l);
    /* ALWAYS decode, even for a push we are about to refuse or a stream we no
     * longer have -- the compression context is shared and skipping a block
     * desynchronises it permanently. */
    int rc = hpack_decode(&c->dec, c->hblock, c->hblock_len, &l);
    int kind = c->cont_kind;
    uint32_t sid = c->cont_stream, promised = c->cont_promised;
    int end_stream = c->cont_end_stream;
    c->cont_active = 0; c->hblock_len = 0;
    c->cont_kind = 0; c->cont_stream = 0; c->cont_promised = 0; c->cont_end_stream = 0;

    if (rc != HPACK_OK) {
        hpack_list_free(&l);
        return conn_fail(c, H2_ERR_COMPRESSION_ERROR, H2_E_COMPRESS);
    }
    if (kind == H2_F_PUSH_PROMISE) {
        hpack_list_free(&l);
        /* Refuse, having kept the compression context in step. See http2.h. */
        rst_id(c, promised, H2_ERR_REFUSED_STREAM);
        c->push_refused++;
        if (c->push_refused > H2_MAX_PUSH_REFUSALS)
            return conn_fail(c, H2_ERR_ENHANCE_YOUR_CALM, H2_E_PROTO);
        return H2_OK;
    }
    return deliver_headers(c, sid, end_stream, &l);
}

static int start_header_block(struct h2_conn *c, int kind, uint32_t sid,
                              uint32_t promised, int end_stream,
                              const uint8_t *frag, int fraglen, int end_headers)
{
    c->cont_active = 1;
    c->cont_kind = kind;
    c->cont_stream = sid;
    c->cont_promised = promised;
    c->cont_end_stream = end_stream;
    c->hblock_len = 0;
    if (fraglen > H2_HEADER_BLOCK_MAX) return conn_fail(c, H2_ERR_ENHANCE_YOUR_CALM, H2_E_TOOLARGE);
    if (buf_reserve(&c->hblock, &c->hblock_cap, fraglen ? fraglen : 1) != H2_OK)
        return conn_fail(c, H2_ERR_INTERNAL_ERROR, H2_E_NOMEM);
    if (fraglen) memcpy(c->hblock, frag, (size_t)fraglen);
    c->hblock_len = fraglen;
    if (end_headers) return finish_header_block(c);
    return H2_OK;
}

/* ------------------------------------------------------------ frame handlers */

static int on_data(struct h2_conn *c, uint8_t flags, uint32_t sid,
                   const uint8_t *p, int len)
{
    if (!sid) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);

    /* The WHOLE payload counts against flow control, padding included (RFC
     * 7540 section 6.9.1), and it counts even when the stream is gone --
     * otherwise a reset mid-transfer silently leaks the connection window. */
    if (len > c->recv_win) return conn_fail(c, H2_ERR_FLOW_CONTROL_ERROR, H2_E_FLOW);
    c->recv_win -= len;
    c->recv_unacked += len;
    c->bytes_in += len;

    const uint8_t *data = p;
    int dlen = len;
    if (flags & H2_FLAG_PADDED) {
        if (len < 1) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
        int pad = p[0];
        /* pad + the pad-length octet must fit INSIDE the payload. ">=" not
         * ">": a pad that consumes the entire remainder is legal, one that
         * consumes more is the out-of-bounds read. */
        if (pad > len - 1) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
        data = p + 1;
        dlen = len - 1 - pad;
    }

    struct h2_stream *s = find_stream(c, sid);
    if (!s || s->done) {
        /* Data for a stream we closed. Legal in flight after RST_STREAM; the
         * window has already been credited above. */
        ack_window(c, NULL);
        if (!s && sid >= c->next_id) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
        return H2_OK;
    }
    if (s->state != H2_S_OPEN && s->state != H2_S_HALF_CLOSED_LOCAL) {
        stream_fail(c, s, H2_ERR_STREAM_CLOSED, H2_E_PROTO);
        return H2_OK;
    }
    if (len > s->recv_win) {
        stream_fail(c, s, H2_ERR_FLOW_CONTROL_ERROR, H2_E_FLOW);
        return H2_OK;
    }
    s->recv_win -= len;
    s->recv_unacked += len;
    s->body_seen += dlen;

    if (dlen > 0) {
        if (s->sink) {
            s->streaming = 1;
            int rc = s->sink(s->sink_ctx, data, dlen);
            if (rc != H2_OK) { stream_fail(c, s, H2_ERR_CANCEL, rc); return H2_OK; }
        } else {
            if (s->body_len + dlen > s->body_max) {
                stream_fail(c, s, H2_ERR_CANCEL, H2_E_TOOLARGE);
                return H2_OK;
            }
            if (buf_reserve(&s->body, &s->body_cap, s->body_len + dlen + 1) != H2_OK) {
                stream_fail(c, s, H2_ERR_INTERNAL_ERROR, H2_E_NOMEM);
                return H2_OK;
            }
            memcpy(s->body + s->body_len, data, (size_t)dlen);
            s->body_len += dlen;
            s->body[s->body_len] = 0;
        }
    }
    ack_window(c, s);

    if (flags & H2_FLAG_END_STREAM) {
        if (s->state == H2_S_OPEN) s->state = H2_S_HALF_CLOSED_REMOTE;
        if (!s->headers_done) { stream_fail(c, s, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO); return H2_OK; }
        stream_finish(c, s, 0);
        flush_conn_window(c);
    }
    return H2_OK;
}

static int on_headers(struct h2_conn *c, uint8_t flags, uint32_t sid,
                      const uint8_t *p, int len)
{
    if (!sid) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
    /* A server may not open a stream with HEADERS; only PUSH_PROMISE reserves
     * one, and even-numbered ids are the server's. Anything at or above our
     * next id has never been used by us. */
    if ((sid & 1) == 0 || sid >= c->next_id) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);

    int off = 0, plen = len;
    if (flags & H2_FLAG_PADDED) {
        if (plen < 1) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
        int pad = p[0];
        if (pad > plen - 1) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
        off = 1;
        plen = plen - 1 - pad;
    }
    if (flags & H2_FLAG_PRIORITY) {
        /* Five bytes of dependency and weight. We do not implement stream
         * prioritisation -- RFC 9113 deprecated the scheme and no server
         * depends on a client honouring it -- but the field still has to be
         * skipped or the header block starts five bytes late and HPACK
         * decodes garbage. */
        if (plen < 5) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
        off += 5;
        plen -= 5;
    }
    if (plen < 0) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
    return start_header_block(c, H2_F_HEADERS, sid, 0, (flags & H2_FLAG_END_STREAM) != 0,
                              p + off, plen, (flags & H2_FLAG_END_HEADERS) != 0);
}

static int on_push_promise(struct h2_conn *c, uint8_t flags, uint32_t sid,
                           const uint8_t *p, int len)
{
    if (!sid) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
    int off = 0, plen = len;
    if (flags & H2_FLAG_PADDED) {
        if (plen < 1) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
        int pad = p[0];
        if (pad > plen - 1) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
        off = 1; plen = plen - 1 - pad;
    }
    if (plen < 4) return conn_fail(c, H2_ERR_FRAME_SIZE_ERROR, H2_E_FRAMESIZE);
    uint32_t promised = rd32(p + off) & 0x7FFFFFFFu;
    off += 4; plen -= 4;
    /* A promised id must be server-initiated (even) and nonzero. */
    if (!promised || (promised & 1)) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
    return start_header_block(c, H2_F_PUSH_PROMISE, sid, promised, 0,
                              p + off, plen, (flags & H2_FLAG_END_HEADERS) != 0);
}

static int on_settings(struct h2_conn *c, uint8_t flags, uint32_t sid,
                       const uint8_t *p, int len)
{
    if (sid) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
    if (flags & H2_FLAG_ACK) {
        if (len) return conn_fail(c, H2_ERR_FRAME_SIZE_ERROR, H2_E_FRAMESIZE);
        c->settings_ack_pending = 0;
        return H2_OK;
    }
    if (len % 6) return conn_fail(c, H2_ERR_FRAME_SIZE_ERROR, H2_E_FRAMESIZE);

    for (int i = 0; i < len; i += 6) {
        uint16_t id = rd16(p + i);
        uint32_t v  = rd32(p + i + 2);
        switch (id) {
        case H2_SET_HEADER_TABLE_SIZE:
            c->peer_hdr_table = v;
            /* Our ENCODER's table now has a new ceiling; the update itself is
             * emitted at the head of the next header block, which is the only
             * position RFC 7541 allows one. */
            hpack_enc_set_capacity(&c->enc, (int)(v > HPACK_CAP_MAX ? HPACK_CAP_MAX : v));
            break;
        case H2_SET_ENABLE_PUSH:
            /* RFC 9113 section 8.4: a server must not set this to 1, and a
             * client must treat anything but 0 as a connection error. */
            if (v != 0) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
            break;
        case H2_SET_MAX_CONCURRENT_STREAMS:
            c->peer_max_conc = v > H2_MAX_STREAMS ? H2_MAX_STREAMS : v;
            break;
        case H2_SET_INITIAL_WINDOW_SIZE: {
            if (v > (uint32_t)H2_MAX_WINDOW)
                return conn_fail(c, H2_ERR_FLOW_CONTROL_ERROR, H2_E_FLOW);
            /* THE RETROACTIVE ONE. A new initial window size adjusts every
             * OPEN stream's send window by the delta, not just future streams
             * (RFC 7540 section 6.9.2). An implementation that only applies it
             * to new streams over-sends on the old ones and gets reset. */
            int64_t delta = (int64_t)v - c->peer_init_win;
            c->peer_init_win = v;
            for (int k = 0; k < H2_MAX_STREAMS; k++) {
                struct h2_stream *s = &c->st[k];
                if (!s->used || s->done) continue;
                s->send_win += delta;
                if (s->send_win > H2_MAX_WINDOW)
                    return conn_fail(c, H2_ERR_FLOW_CONTROL_ERROR, H2_E_FLOW);
            }
            break;
        }
        case H2_SET_MAX_FRAME_SIZE:
            if (v < H2_DEFAULT_MAX_FRAME || v > 16777215u)
                return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
            c->peer_max_frame = v;
            break;
        case H2_SET_MAX_HEADER_LIST_SIZE:
            c->peer_max_hdr_list = v;
            break;
        default:
            break;              /* unknown settings are ignored, per the RFC */
        }
    }
    c->peer_settings_seen = 1;
    tx_frame(c, H2_F_SETTINGS, H2_FLAG_ACK, 0, NULL, 0);
    return H2_OK;
}

static int on_window_update(struct h2_conn *c, uint32_t sid, const uint8_t *p, int len)
{
    if (len != 4) return conn_fail(c, H2_ERR_FRAME_SIZE_ERROR, H2_E_FRAMESIZE);
    uint32_t inc = rd32(p) & 0x7FFFFFFFu;
    c->window_updates_in++;
    if (!sid) {
        if (!inc) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
        c->send_win += inc;
        if (c->send_win > H2_MAX_WINDOW)
            return conn_fail(c, H2_ERR_FLOW_CONTROL_ERROR, H2_E_FLOW);
        return H2_OK;
    }
    struct h2_stream *s = find_stream(c, sid);
    if (!s) {
        if (sid >= c->next_id) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
        return H2_OK;                        /* for a stream we already released */
    }
    if (!inc) { stream_fail(c, s, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO); return H2_OK; }
    s->send_win += inc;
    if (s->send_win > H2_MAX_WINDOW) {
        stream_fail(c, s, H2_ERR_FLOW_CONTROL_ERROR, H2_E_FLOW);
        return H2_OK;
    }
    return H2_OK;
}

static int on_rst_stream(struct h2_conn *c, uint32_t sid, const uint8_t *p, int len)
{
    if (!sid) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
    if (len != 4) return conn_fail(c, H2_ERR_FRAME_SIZE_ERROR, H2_E_FRAMESIZE);
    uint32_t code = rd32(p);
    c->rst_in++;
    struct h2_stream *s = find_stream(c, sid);
    if (!s) {
        /* RST_STREAM for a stream that was never opened is a protocol error;
         * for one we have already finished with, it is normal. */
        if (sid >= c->next_id) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
        return H2_OK;
    }
    s->rst_code = code;
    s->rst_sent = 1;                 /* do not answer a reset with a reset */
    /* REFUSED_STREAM means the server never processed it, so the request is
     * safe to retry on another connection; anything else is not. */
    stream_finish(c, s, code == H2_ERR_REFUSED_STREAM ? H2_E_REFUSED : H2_E_RESET);
    flush_conn_window(c);
    return H2_OK;
}

static int on_goaway(struct h2_conn *c, uint32_t sid, const uint8_t *p, int len)
{
    if (sid) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
    if (len < 8) return conn_fail(c, H2_ERR_FRAME_SIZE_ERROR, H2_E_FRAMESIZE);
    c->peer_last_stream = rd32(p) & 0x7FFFFFFFu;
    c->recv_goaway = rd32(p + 4);
    c->have_recv_goaway = 1;
    if (c->state == H2_C_OPEN) c->state = H2_C_GOAWAY;
    /* Streams above last-stream-id were never processed: they are RETRYABLE,
     * and saying so is the difference between a page that reloads cleanly and
     * one that shows an error. */
    for (int i = 0; i < H2_MAX_STREAMS; i++) {
        struct h2_stream *s = &c->st[i];
        if (!s->used || s->done) continue;
        if (s->id > c->peer_last_stream) stream_finish(c, s, H2_E_REFUSED);
    }
    return H2_OK;
}

static int on_ping(struct h2_conn *c, uint8_t flags, uint32_t sid, const uint8_t *p, int len)
{
    if (sid) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
    if (len != 8) return conn_fail(c, H2_ERR_FRAME_SIZE_ERROR, H2_E_FRAMESIZE);
    c->pings_in++;
    if (!(flags & H2_FLAG_ACK)) tx_frame(c, H2_F_PING, H2_FLAG_ACK, 0, p, 8);
    return H2_OK;
}

/* --------------------------------------------------------------- frame loop */

static int handle_frame(struct h2_conn *c, uint32_t len, uint8_t type, uint8_t flags,
                        uint32_t sid, const uint8_t *p)
{
    c->frames_in++;

    /* A header block is one unit: between a HEADERS/PUSH_PROMISE without
     * END_HEADERS and its last CONTINUATION, NOTHING else may appear -- not
     * even on another stream, and not even a frame type we would otherwise
     * ignore. Allowing it is how an implementation ends up splicing two
     * streams' headers into one HPACK stream. */
    if (c->cont_active) {
        if (type != H2_F_CONTINUATION || sid != c->cont_stream)
            return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
        if (c->hblock_len + (int)len > H2_HEADER_BLOCK_MAX)
            return conn_fail(c, H2_ERR_ENHANCE_YOUR_CALM, H2_E_TOOLARGE);
        if (buf_reserve(&c->hblock, &c->hblock_cap, c->hblock_len + (int)len + 1) != H2_OK)
            return conn_fail(c, H2_ERR_INTERNAL_ERROR, H2_E_NOMEM);
        if (len) memcpy(c->hblock + c->hblock_len, p, (size_t)len);
        c->hblock_len += (int)len;
        if (flags & H2_FLAG_END_HEADERS) return finish_header_block(c);
        return H2_OK;
    }

    switch (type) {
    case H2_F_DATA:          return on_data(c, flags, sid, p, (int)len);
    case H2_F_HEADERS:       return on_headers(c, flags, sid, p, (int)len);
    case H2_F_PRIORITY:
        if (!sid) return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
        if (len != 5) return conn_fail(c, H2_ERR_FRAME_SIZE_ERROR, H2_E_FRAMESIZE);
        return H2_OK;        /* parsed and discarded: see the note in on_headers */
    case H2_F_RST_STREAM:    return on_rst_stream(c, sid, p, (int)len);
    case H2_F_SETTINGS:      return on_settings(c, flags, sid, p, (int)len);
    case H2_F_PUSH_PROMISE:  return on_push_promise(c, flags, sid, p, (int)len);
    case H2_F_PING:          return on_ping(c, flags, sid, p, (int)len);
    case H2_F_GOAWAY:        return on_goaway(c, sid, p, (int)len);
    case H2_F_WINDOW_UPDATE: return on_window_update(c, sid, p, (int)len);
    case H2_F_CONTINUATION:
        /* A CONTINUATION with no header block open. */
        return conn_fail(c, H2_ERR_PROTOCOL_ERROR, H2_E_PROTO);
    default:
        return H2_OK;        /* unknown frame types MUST be ignored (RFC 4.1) */
    }
}

/* Consume as many whole frames as the receive buffer holds. */
static int parse_frames(struct h2_conn *c)
{
    int off = 0;
    while (c->rx_len - off >= H2_FRAME_HDR) {
        uint32_t len, sid; uint8_t type, flags;
        h2_frame_parse(c->rx + off, &len, &type, &flags, &sid);
        /* Checked against OUR advertised maximum, before a byte is buffered:
         * this is the bound on how much a peer can make us hold per frame. */
        if (len > (uint32_t)H2_OUR_MAX_FRAME) {
            conn_fail(c, H2_ERR_FRAME_SIZE_ERROR, H2_E_FRAMESIZE);
            c->rx_len = 0;
            return c->err;
        }
        if ((uint32_t)(c->rx_len - off - H2_FRAME_HDR) < len) break;   /* incomplete */
        int rc = handle_frame(c, len, type, flags, sid, c->rx + off + H2_FRAME_HDR);
        off += H2_FRAME_HDR + (int)len;
        if (rc != H2_OK || c->state == H2_C_ERROR) {
            c->rx_len = 0;
            return c->state == H2_C_ERROR ? c->err : rc;
        }
    }
    if (off) {
        memmove(c->rx, c->rx + off, (size_t)(c->rx_len - off));
        c->rx_len -= off;
    }
    return H2_OK;
}

/* ------------------------------------------------------------------- pump */

/* Write out as much of the queue as the transport will take. A short write is
 * normal and not an error; the remainder stays queued. Returns the bytes
 * written, or -1 on a transport failure. */
static int tx_flush(struct h2_conn *c)
{
    int wrote = 0;
    while (c->tx_off < c->tx_len) {
        if (c->t.poll && c->t.poll(c->t.ctx, 1) == 0) break;
        int n = c->t.write(c->t.ctx, c->tx + c->tx_off, c->tx_len - c->tx_off);
        if (n == H2_AGAIN) break;
        if (n < 0) return -1;
        c->tx_off += n;
        c->bytes_out += n;
        wrote += n;
    }
    if (c->tx_off == c->tx_len && c->tx_len) { c->tx_off = c->tx_len = 0; }
    return wrote;
}

int h2_conn_pump(struct h2_conn *c, int64_t now)
{
    if (!c) return H2_C_CLOSED;
    if (c->state == H2_C_CLOSED) return c->state;

    int progress = 0, wrote;

    /* 1. Queue whatever request-body DATA the windows allow. */
    if (c->state != H2_C_ERROR && pump_send_bodies(c)) progress = 1;

    /* 2. Flush. */
    wrote = tx_flush(c);
    if (wrote < 0) { c->state = H2_C_ERROR; if (!c->err) c->err = H2_E_TRANSPORT; return c->state; }
    if (wrote > 0) progress = 1;

    /* Once the error GOAWAY is out there is nothing further to do. */
    if (c->state == H2_C_ERROR) return c->state;

    /* 3. Read a bounded chunk and parse. The bound is what stops one fast
     *    origin from starving every other connection in the loop. */
    int budget = H2_RX_CHUNK;
    while (budget > 0) {
        if (c->t.poll && c->t.poll(c->t.ctx, 0) == 0) break;
        if (buf_reserve(&c->rx, &c->rx_cap, c->rx_len + 4096) != H2_OK) {
            conn_fail(c, H2_ERR_INTERNAL_ERROR, H2_E_NOMEM);
            goto out;
        }
        int want = c->rx_cap - c->rx_len;
        if (want > budget) want = budget;
        int n = c->t.read(c->t.ctx, c->rx + c->rx_len, want);
        if (n == H2_AGAIN) break;
        if (n == H2_EOF) {
            /* The peer closed. Streams still waiting were truncated. */
            for (int i = 0; i < H2_MAX_STREAMS; i++)
                if (c->st[i].used && !c->st[i].done) stream_finish(c, &c->st[i], H2_E_CLOSED);
            c->state = H2_C_CLOSED;
            if (!c->err) c->err = H2_E_CLOSED;
            return c->state;
        }
        if (n < 0) {
            for (int i = 0; i < H2_MAX_STREAMS; i++)
                if (c->st[i].used && !c->st[i].done) stream_finish(c, &c->st[i], H2_E_TRANSPORT);
            c->state = H2_C_ERROR;
            if (!c->err) c->err = H2_E_TRANSPORT;
            return c->state;
        }
        c->rx_len += n;
        budget -= n;
        progress = 1;
        parse_frames(c);
        if (c->state == H2_C_ERROR) goto out;
    }

    /* 4. Flush again: the frames just parsed will have queued SETTINGS acks,
     *    PING acks and WINDOW_UPDATEs, and holding those until the next pump
     *    is exactly the delay that looks like a stall. */
    wrote = tx_flush(c);
    if (wrote < 0) { c->state = H2_C_ERROR; if (!c->err) c->err = H2_E_TRANSPORT; return c->state; }
    if (wrote > 0) progress = 1;

    /* 5. Stall detection. A flow-control bug does not raise an error, it
     *    simply stops -- on either side. If we have bytes to send and both
     *    windows have been shut for stall_ms with nothing else moving, fail
     *    loudly instead of waiting forever. */
    if (progress) c->last_progress = now;
    else if (c->stall_ms > 0 && send_blocked(c)) {
        if (!c->last_progress) c->last_progress = now;
        else if (now - c->last_progress > c->stall_ms)
            conn_fail(c, H2_ERR_FLOW_CONTROL_ERROR, H2_E_STALL);
    }

out:
    /* A connection error queues a GOAWAY naming the reason. Push it out in the
     * SAME pump: a peer that is told why is a peer that can log it, and a
     * caller that sees H2_C_ERROR has no reason to pump again. Best effort --
     * the transport may already be the thing that died. */
    if (c->state == H2_C_ERROR) tx_flush(c);
    return c->state;
}

/* ------------------------------------------------------------- stream API */

int h2_stream_done(const struct h2_conn *c, uint32_t id)
{
    const struct h2_stream *s = h2_stream_get((struct h2_conn *)c, id);
    return s ? s->done : 1;
}

int h2_stream_status(const struct h2_conn *c, uint32_t id)
{
    const struct h2_stream *s = h2_stream_get((struct h2_conn *)c, id);
    return s ? s->status : 0;
}

int h2_stream_err(const struct h2_conn *c, uint32_t id)
{
    const struct h2_stream *s = h2_stream_get((struct h2_conn *)c, id);
    return s ? s->err : H2_E_ARG;
}

const char *h2_stream_header(const struct h2_conn *c, uint32_t id, const char *name)
{
    const struct h2_stream *s = h2_stream_get((struct h2_conn *)c, id);
    return s ? hpack_list_get(&s->hdr, name) : NULL;
}

const uint8_t *h2_stream_body(const struct h2_conn *c, uint32_t id, int *len)
{
    const struct h2_stream *s = h2_stream_get((struct h2_conn *)c, id);
    if (len) *len = s ? s->body_len : 0;
    return s ? s->body : NULL;
}

uint8_t *h2_stream_take_body(struct h2_conn *c, uint32_t id, int *len)
{
    struct h2_stream *s = h2_stream_get(c, id);
    if (!s) { if (len) *len = 0; return NULL; }
    uint8_t *b = s->body;
    if (len) *len = s->body_len;
    s->body = NULL; s->body_len = s->body_cap = 0;
    return b;
}

void h2_stream_sink(struct h2_conn *c, uint32_t id, h2_body_sink cb, void *ctx)
{
    struct h2_stream *s = h2_stream_get(c, id);
    if (!s) return;
    s->sink = cb; s->sink_ctx = ctx;
}

void h2_stream_limit(struct h2_conn *c, uint32_t id, int body_max)
{
    struct h2_stream *s = h2_stream_get(c, id);
    if (s && body_max > 0) s->body_max = body_max;
}

void h2_stream_cancel(struct h2_conn *c, uint32_t id)
{
    struct h2_stream *s = h2_stream_get(c, id);
    if (!s || s->done) return;
    stream_fail(c, s, H2_ERR_CANCEL, H2_E_RESET);
    flush_conn_window(c);
}

void h2_stream_release(struct h2_conn *c, uint32_t id)
{
    struct h2_stream *s = h2_stream_get(c, id);
    if (!s) return;
    if (!s->done) h2_stream_cancel(c, id);
    stream_clear(s);
}
