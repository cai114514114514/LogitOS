#ifndef LOGIT_HTTP2_H
#define LOGIT_HTTP2_H

#include <stdint.h>
#include <stddef.h>

#include "hpack.h"

/* http2 -- an HTTP/2 client (RFC 7540/9113) in ring 3, over the same shape of
 * transport vtable http1.c uses.
 *
 * WHAT THIS BUYS, HONESTLY.  Pooled HTTP/1.1 already removed most of the cost
 * this project used to pay: one wikipedia page went from 14 handshakes to 4
 * once hpool landed, and 40 resources over 3 origins now cost 3 dials.  So
 * HTTP/2 is not here to remove handshakes -- that is done.  It is here for the
 * two things a pool cannot do:
 *
 *   MULTIPLEXING.  On HTTP/1.1 a connection carries one request at a time, so
 *   concurrency means six sockets per origin, and this machine has 32 TCP
 *   slots in total.  On HTTP/2 one connection carries every request at once,
 *   interleaved at the frame level, and a slow response no longer blocks the
 *   ones behind it.  40 subresources become 1 connection and 1 round trip
 *   instead of 6 connections and 7 sequential trips.
 *
 *   HEADER COMPRESSION.  Every request on a page repeats the same user-agent,
 *   accept, cookie and referer.  HPACK sends them once and then indexes them,
 *   which on a page with dozens of subresources is most of the request bytes.
 *
 * WHAT IT COSTS.  One connection is now a single point of failure for a whole
 * origin, and its two stateful subsystems -- HPACK and flow control -- fail
 * silently rather than loudly.  Both are treated accordingly: a compression
 * error tears the connection down (see hpack.h for why resynchronising is
 * impossible), and every flow-control window is accounted in both directions
 * with an explicit stall timeout, because the natural failure of a flow
 * control bug is not an error, it is a hang.
 *
 * FLOW CONTROL, exactly.  There are two independent windows in each direction:
 * one for the connection and one per stream, and a DATA frame spends BOTH.
 *   - Receive: we announce SETTINGS_INITIAL_WINDOW_SIZE = H2_OUR_STREAM_WINDOW
 *     and raise the connection window to H2_OUR_CONN_WINDOW with an immediate
 *     WINDOW_UPDATE (SETTINGS cannot set the connection window -- that is a
 *     real asymmetry in the protocol and forgetting it stalls every transfer
 *     at exactly 65535 bytes).  Bytes are counted as consumed the moment they
 *     are delivered, and a WINDOW_UPDATE is emitted once the unacknowledged
 *     total passes half the window.  There is no path where we consume data
 *     and do not eventually replenish; that is the deadlock.
 *   - Send: the peer's windows start at 65535 and its
 *     SETTINGS_INITIAL_WINDOW_SIZE retroactively adjusts EVERY open stream by
 *     the delta, including streams already flowing.  A DATA frame is sized to
 *     min(remaining, connection window, stream window, peer max frame size).
 *     A window that would exceed 2^31-1 is a flow-control error, not a wrap.
 *   - Neither direction waits forever: if we have data to send, both windows
 *     are shut, and nothing has moved for h2_conn_stall_ms, the connection
 *     fails with H2_E_STALL instead of hanging.  A peer that never sends
 *     WINDOW_UPDATE is a bug we report, not a bug we inherit.
 *
 * SERVER PUSH IS REFUSED, NOT IMPLEMENTED.  We advertise
 * SETTINGS_ENABLE_PUSH = 0, so a conforming server never pushes.  If one
 * arrives anyway we still HPACK-decode the promised header block -- skipping
 * it would desynchronise the compression context and corrupt every later
 * header on the connection, which is a far worse outcome than the push -- and
 * then RST_STREAM(REFUSED_STREAM) the promised stream and drop it.  Push is
 * deprecated in practice (Chrome removed it in 2022) and implementing the
 * cache semantics correctly is more code than the feature is worth; refusing
 * it safely is the whole obligation.  A flood of promises past
 * H2_MAX_PUSH_REFUSALS is treated as a connection error, because refusing
 * costs us a frame each time.
 */

/* ---- transport ------------------------------------------------------- */

/* Deliberately the same contract as http1.h's H1_*: a caller that already has
 * a transport for HTTP/1.1 hands the identical vtable here, which is what
 * makes "whatever ALPN chose" a one-line decision at the call site. The types
 * are separate so this module has no build dependency on http1.c. */
#define H2_AGAIN   0        /* nothing available right now */
#define H2_EOF   (-1)       /* orderly close by the peer */
#define H2_TERR  (-2)       /* transport failure */

struct h2_transport {
    int (*read)(void *ctx, void *buf, int len);
    int (*write)(void *ctx, const void *buf, int len);
    int (*poll)(void *ctx, int want_write);
    void *ctx;
};

/* ---- local errors ---------------------------------------------------- */

enum {
    H2_OK           =  0,
    H2_E_PROTO      = -1,   /* the peer broke the protocol */
    H2_E_FRAMESIZE  = -2,
    H2_E_COMPRESS   = -3,   /* HPACK failed; the connection is unrecoverable */
    H2_E_FLOW       = -4,
    H2_E_TRANSPORT  = -5,
    H2_E_NOMEM      = -6,
    H2_E_ARG        = -7,
    H2_E_GOAWAY     = -8,   /* the peer is shutting down */
    H2_E_REFUSED    = -9,   /* stream refused; safe to retry elsewhere */
    H2_E_RESET      = -10,  /* peer RST_STREAM'd this stream */
    H2_E_TOOLARGE   = -11,
    H2_E_STALL      = -12,  /* flow control never opened; see h2_conn_stall_ms */
    H2_E_CLOSED     = -13,
    H2_E_NOSLOT     = -14
};

const char *h2_strerror(int err);
const char *h2_errcode_name(uint32_t code);

/* ---- wire error codes (RFC 7540 section 7) --------------------------- */

enum {
    H2_ERR_NO_ERROR            = 0x0,
    H2_ERR_PROTOCOL_ERROR      = 0x1,
    H2_ERR_INTERNAL_ERROR      = 0x2,
    H2_ERR_FLOW_CONTROL_ERROR  = 0x3,
    H2_ERR_SETTINGS_TIMEOUT    = 0x4,
    H2_ERR_STREAM_CLOSED       = 0x5,
    H2_ERR_FRAME_SIZE_ERROR    = 0x6,
    H2_ERR_REFUSED_STREAM      = 0x7,
    H2_ERR_CANCEL              = 0x8,
    H2_ERR_COMPRESSION_ERROR   = 0x9,
    H2_ERR_CONNECT_ERROR       = 0xa,
    H2_ERR_ENHANCE_YOUR_CALM   = 0xb,
    H2_ERR_INADEQUATE_SECURITY = 0xc,
    H2_ERR_HTTP_1_1_REQUIRED   = 0xd
};

/* ---- frames ---------------------------------------------------------- */

enum {
    H2_F_DATA          = 0x0,
    H2_F_HEADERS       = 0x1,
    H2_F_PRIORITY      = 0x2,
    H2_F_RST_STREAM    = 0x3,
    H2_F_SETTINGS      = 0x4,
    H2_F_PUSH_PROMISE  = 0x5,
    H2_F_PING          = 0x6,
    H2_F_GOAWAY        = 0x7,
    H2_F_WINDOW_UPDATE = 0x8,
    H2_F_CONTINUATION  = 0x9
};

enum {
    H2_FLAG_END_STREAM  = 0x01,
    H2_FLAG_ACK         = 0x01,   /* SETTINGS and PING reuse bit 0 */
    H2_FLAG_END_HEADERS = 0x04,
    H2_FLAG_PADDED      = 0x08,
    H2_FLAG_PRIORITY    = 0x20
};

enum {
    H2_SET_HEADER_TABLE_SIZE      = 0x1,
    H2_SET_ENABLE_PUSH            = 0x2,
    H2_SET_MAX_CONCURRENT_STREAMS = 0x3,
    H2_SET_INITIAL_WINDOW_SIZE    = 0x4,
    H2_SET_MAX_FRAME_SIZE         = 0x5,
    H2_SET_MAX_HEADER_LIST_SIZE   = 0x6
};

#define H2_PREFACE      "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define H2_PREFACE_LEN  24
#define H2_FRAME_HDR    9
#define H2_DEFAULT_MAX_FRAME    16384
#define H2_MAX_WINDOW           0x7FFFFFFF

/* ---- our limits ------------------------------------------------------ */

#define H2_MAX_STREAMS          16        /* concurrent streams we will open */
#define H2_OUR_STREAM_WINDOW    (256 * 1024)
#define H2_OUR_CONN_WINDOW      (1024 * 1024)
#define H2_OUR_MAX_FRAME        H2_DEFAULT_MAX_FRAME
#define H2_HEADER_BLOCK_MAX     (64 * 1024)   /* one HEADERS + its CONTINUATIONs */
#define H2_BODY_MAX             (16 * 1024 * 1024)
#define H2_MAX_PUSH_REFUSALS    64
#define H2_RX_CHUNK             16384     /* bytes read per pump, so one fast
                                           * origin cannot starve the others */

/* ---- stream states (RFC 7540 section 5.1) ---------------------------- */

enum {
    H2_S_IDLE = 0,
    H2_S_RESERVED_REMOTE,      /* only reachable via PUSH_PROMISE, which we refuse */
    H2_S_OPEN,
    H2_S_HALF_CLOSED_LOCAL,    /* we sent END_STREAM; still receiving */
    H2_S_HALF_CLOSED_REMOTE,
    H2_S_CLOSED
};

const char *h2_state_name(int state);

/* Body sink, same contract as http1's: bytes are handed up as they arrive
 * instead of being buffered to completion. Return H2_OK to accept. */
typedef int (*h2_body_sink)(void *ctx, const uint8_t *data, int len);

struct h2_stream {
    int      used;
    uint32_t id;
    int      state;

    /* flow control, both directions, both levels (the connection's pair lives
     * on struct h2_conn) */
    int64_t  send_win;         /* what WE may still send on this stream */
    int64_t  recv_win;         /* what the PEER may still send us */
    int64_t  recv_unacked;     /* consumed but not yet WINDOW_UPDATE'd */

    /* response */
    struct hpack_list hdr;
    struct hpack_list trailer;
    int      status;
    int      headers_done;
    int      hdr_blocks;       /* 1 = informational/final headers, 2 = trailers */
    uint8_t *body;
    int      body_len, body_cap, body_max;
    int64_t  body_seen;
    h2_body_sink sink;
    void    *sink_ctx;
    int      streaming;

    /* request body, borrowed not copied */
    const uint8_t *req_body;
    int      req_len, req_off;
    int      req_end_stream;   /* END_STREAM still owed once the body is out */
    int      req_headers_sent;

    int      done;             /* the response is complete or the stream failed */
    int      err;              /* H2_* local error, 0 while healthy */
    uint32_t rst_code;         /* wire code if the peer reset us */
    int      rst_sent;
};

struct h2_conn {
    struct h2_transport t;

    int      state;            /* H2_C_* */
    int      err;              /* first local error */
    uint32_t sent_goaway, recv_goaway;
    int      have_recv_goaway;
    uint32_t peer_last_stream; /* from GOAWAY: streams above this are refused */

    /* buffers */
    uint8_t *rx; int rx_len, rx_cap;
    uint8_t *tx; int tx_len, tx_off, tx_cap;

    /* HPACK, one context per direction for the life of the connection */
    struct hpack_dec dec;
    struct hpack_enc enc;

    /* header block assembly: a HEADERS/PUSH_PROMISE and its CONTINUATIONs are
     * a single unit and nothing may be interleaved between them */
    int      cont_active;
    uint32_t cont_stream;
    int      cont_kind;        /* H2_F_HEADERS or H2_F_PUSH_PROMISE */
    int      cont_end_stream;
    uint32_t cont_promised;
    uint8_t *hblock; int hblock_len, hblock_cap;

    /* peer settings */
    uint32_t peer_max_frame;
    uint32_t peer_max_conc;
    uint32_t peer_hdr_table;
    uint32_t peer_max_hdr_list;
    int64_t  peer_init_win;
    int      peer_settings_seen;
    int      settings_ack_pending;   /* our SETTINGS not yet acked */

    /* connection-level flow control */
    int64_t  send_win, recv_win, recv_unacked;

    struct h2_stream st[H2_MAX_STREAMS];
    uint32_t next_id;

    /* progress / stall detection */
    int64_t  last_progress;
    int64_t  stall_ms;

    /* observability: a multiplexed connection that silently serialises looks
     * exactly like one that works, so the counters are part of the design. */
    int      frames_in, frames_out;
    int      streams_opened, streams_done;
    int      max_concurrent_seen, concurrent;
    int      push_refused;
    int      window_updates_out, window_updates_in;
    int64_t  bytes_in, bytes_out;
    int      pings_in, rst_in, rst_out;
};

enum { H2_C_INIT = 0, H2_C_OPEN, H2_C_GOAWAY, H2_C_CLOSED, H2_C_ERROR };

/* ---- connection ------------------------------------------------------ */

/* Queues the connection preface, our SETTINGS and the connection-level
 * WINDOW_UPDATE. Sends nothing: the first h2_conn_pump does the writing, so
 * this never blocks and can be called before the socket is writable. */
int  h2_conn_start(struct h2_conn *c, const struct h2_transport *t);
void h2_conn_free(struct h2_conn *c);
/* How long a completely shut send window may last before the connection fails
 * with H2_E_STALL. 0 disables the check. Default 30000 ms. */
void h2_conn_stall_ms(struct h2_conn *c, int64_t ms);

/* One non-blocking step: flush what is queued, read what is there, parse whole
 * frames, deliver. `now` is a millisecond clock used only for stall detection;
 * host tests supply their own. Returns the connection state. */
int  h2_conn_pump(struct h2_conn *c, int64_t now);

/* Start a request. Returns the stream id (odd, increasing) or a negative
 * H2_E_*: H2_E_NOSLOT when H2_MAX_STREAMS or the peer's
 * SETTINGS_MAX_CONCURRENT_STREAMS is reached (retry after a stream finishes),
 * H2_E_GOAWAY once the peer is shutting down. `body` is BORROWED and must stay
 * valid until the stream completes. `extra` may be NULL. */
int  h2_request(struct h2_conn *c, const char *method, const char *scheme,
                const char *authority, const char *path,
                const struct hpack_list *extra, const void *body, int blen);

/* Send GOAWAY and stop opening streams. Existing streams keep running. */
void h2_conn_goaway(struct h2_conn *c, uint32_t code);
int  h2_conn_state(const struct h2_conn *c);
/* 1 while the connection can still carry a new request. */
int  h2_conn_usable(const struct h2_conn *c);
int  h2_conn_active_streams(const struct h2_conn *c);

/* ---- streams --------------------------------------------------------- */

struct h2_stream *h2_stream_get(struct h2_conn *c, uint32_t id);
int   h2_stream_done(const struct h2_conn *c, uint32_t id);
int   h2_stream_status(const struct h2_conn *c, uint32_t id);
int   h2_stream_err(const struct h2_conn *c, uint32_t id);
const char *h2_stream_header(const struct h2_conn *c, uint32_t id, const char *name);
const uint8_t *h2_stream_body(const struct h2_conn *c, uint32_t id, int *len);
/* Take the body buffer away from the stream; the caller frees it. */
uint8_t *h2_stream_take_body(struct h2_conn *c, uint32_t id, int *len);
/* Register a sink before the response arrives; body bytes then bypass the
 * buffer entirely. */
void  h2_stream_sink(struct h2_conn *c, uint32_t id, h2_body_sink cb, void *ctx);
void  h2_stream_limit(struct h2_conn *c, uint32_t id, int body_max);
/* RST_STREAM(CANCEL) and close it locally. */
void  h2_stream_cancel(struct h2_conn *c, uint32_t id);
/* Release the slot so another request can use it. Frees the response. */
void  h2_stream_release(struct h2_conn *c, uint32_t id);

/* ---- exposed for the tests and the fuzzer ---------------------------- */

/* Parse a frame header out of 9 bytes. Returns H2_OK or H2_E_ARG. */
int  h2_frame_parse(const uint8_t *p, uint32_t *len, uint8_t *type,
                    uint8_t *flags, uint32_t *stream);
void h2_frame_write(uint8_t *p, uint32_t len, uint8_t type, uint8_t flags, uint32_t stream);

#endif /* LOGIT_HTTP2_H */
