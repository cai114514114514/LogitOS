#ifndef LOGIT_HTTP1_H
#define LOGIT_HTTP1_H

#include <stdint.h>
#include <stddef.h>

/* http1 -- a real HTTP/1.1 client, in ring 3.
 *
 * WHY THIS EXISTS.  The kernel's c/net/http/http.c builds its request by
 * concatenating string literals, so it can carry no header list at all: no
 * Cookie, no POST body, no conditional GET.  It reads into one global 1 MiB
 * buffer behind an `http_busy` flag, so exactly one request can be in flight
 * on the whole machine.  It sends `Connection: close` every time, so a page
 * with 40 sub-resources pays 40 TLS handshakes.  It advertises
 * `Accept-Encoding: identity` because "we have no inflater on this path" --
 * an inflater exists (rust/src/inflate.rs), it was just on the other side of
 * the ring boundary.  And it has never had a unit test, despite being the
 * component that parses attacker-controlled bytes in ring 0.
 *
 * The design here follows M17: the kernel is a primitive provider, parsing
 * moves to ring 3, and the parser becomes host-testable.  Two decisions carry
 * most of the value:
 *
 *   1. TRANSPORT IS A VTABLE (struct h1_transport).  http1.c never calls
 *      tcp_recv or tls_recv.  It gets read/write/poll function pointers, so
 *      the same code runs over a real socket, over TLS, and over an
 *      in-memory byte stream in a host test.  It also means this module does
 *      not have to wait for the async-socket or TLS work to land.
 *
 *   2. THE RESPONSE PARSER NEVER BLOCKS.  h1_response_feed() takes whatever
 *      bytes have arrived -- one byte, half a header, three chunks and a
 *      trailer -- and advances a state machine.  Nothing in it waits, sleeps,
 *      or polls.  That is what makes several requests in flight possible, and
 *      it is also what makes "deliver this response one byte at a time" a
 *      three-line test instead of a network setup.
 *
 * Everything is bounded.  There is no path here where a hostile server
 * chooses an allocation size: Content-Length and chunk-size are parsed with
 * overflow detection and checked against a configured cap BEFORE any memory
 * is reserved, and the body buffer grows only as bytes actually arrive.
 */

/* ---- transport ------------------------------------------------------- */

/* read()/write() return a positive byte count, or one of these. Keeping EOF
 * distinct from an error is not pedantry: "connection closed" is how a
 * response with neither Content-Length nor chunked encoding legitimately
 * ends, and it is also how a truncated one illegitimately ends. The parser
 * must be able to tell those apart, so the transport must too. */
#define H1_AGAIN   0        /* nothing available right now; call again later */
#define H1_EOF   (-1)       /* orderly close by the peer */
#define H1_TERR  (-2)       /* transport failure */

struct h1_transport {
    int (*read)(void *ctx, void *buf, int len);
    int (*write)(void *ctx, const void *buf, int len);
    /* Optional readiness hint; NULL means "always try". want_write selects
     * the direction. Returns 1 ready, 0 not ready, -1 dead. */
    int (*poll)(void *ctx, int want_write);
    void *ctx;
};

/* ---- errors ---------------------------------------------------------- */

enum {
    H1_OK          =  0,
    H1_E_SYNTAX    = -1,    /* malformed status line, header, or chunk framing */
    H1_E_TOOLARGE  = -2,    /* exceeded a configured bound */
    H1_E_TRUNC     = -3,    /* peer closed before the message was complete */
    H1_E_NOMEM     = -4,
    H1_E_TRANSPORT = -5,
    H1_E_ARG       = -6,
    H1_E_ENCODING  = -7     /* Content-Encoding unsupported or corrupt */
};

const char *h1_strerror(int err);

/* ---- bounds ---------------------------------------------------------- */

#define H1_LINE_MAX     8192        /* one status/header/chunk-size line */
#define H1_HDR_MAX     65536        /* whole header section */
#define H1_MAX_HEADERS   256
#define H1_BODY_MAX  (8*1024*1024)  /* default body cap; see h1_response_limit */
#define H1_MAX_INTERIM     8        /* consecutive 1xx before we give up */
#define H1_METHOD_MAX     16
#define H1_REASON_MAX     80

/* ---- header list ----------------------------------------------------- */

struct h1_hdr { char *name; char *value; };

struct h1_headers {
    struct h1_hdr *v;
    int n, cap;
};

void        h1_headers_init(struct h1_headers *h);
void        h1_headers_free(struct h1_headers *h);
/* Append. nlen/vlen may be -1 for NUL-terminated. Rejects a non-token name or
 * a value containing CR, LF or NUL (H1_E_ARG) -- see the header-injection
 * note in http1.c. */
int         h1_headers_add(struct h1_headers *h, const char *name, int nlen,
                           const char *value, int vlen);
/* Replace every occurrence of `name`, or append if absent. */
int         h1_headers_set(struct h1_headers *h, const char *name, const char *value);
void        h1_headers_remove(struct h1_headers *h, const char *name);
/* Case-insensitive lookup. Returns the FIRST match, or NULL. */
const char *h1_headers_get(const struct h1_headers *h, const char *name);
int         h1_headers_count(const struct h1_headers *h, const char *name);
const char *h1_headers_nth(const struct h1_headers *h, const char *name, int idx);

/* ---- request --------------------------------------------------------- */

struct h1_request {
    char  method[H1_METHOD_MAX];
    char *target;                   /* request-target, owned */
    struct h1_headers hdr;
    const uint8_t *body;            /* borrowed, not copied */
    int   body_len;
};

int  h1_request_init(struct h1_request *q, const char *method, const char *target);
void h1_request_free(struct h1_request *q);
int  h1_request_set_target(struct h1_request *q, const char *target);
int  h1_request_set_header(struct h1_request *q, const char *name, const char *value);
int  h1_request_add_header(struct h1_request *q, const char *name, const char *value);
void h1_request_set_body(struct h1_request *q, const void *body, int len);
/* Serialize into a malloc'd buffer the caller frees. Revalidates every field:
 * build() is a security boundary, not a formatter. */
int  h1_request_build(const struct h1_request *q, char **out, int *outlen);

/* ---- response -------------------------------------------------------- */

/* A body sink: bytes are handed up AS THEY ARRIVE instead of being buffered
 * until the message is complete.  Return H1_OK to accept, or a negative H1_E_*
 * to fail the response.
 *
 * WHY THIS EXISTS.  Server-Sent Events is an HTTP response that never ends:
 * a chat model emits a token, flushes, and emits the next one seconds later.
 * Buffer-then-deliver turns that into "hang until the server gives up", so a
 * streaming endpoint is unusable even though every byte arrives.  With a sink
 * registered, h1_response_feed calls it from inside the same parse that
 * consumed the bytes -- there is no queue and no extra copy, and chunked
 * framing is undone on the way through, so the sink sees the entity body and
 * never a chunk header.
 *
 * A sink only takes effect when the body needs no whole-message transform.
 * Content-Encoding does (our inflater is one-shot), so a gzip response falls
 * back to buffering and is delivered complete: correct, just not incremental.
 * h1_response_streaming() reports which happened, and it is only meaningful
 * after h1_response_headers_done(). */
typedef int (*h1_body_sink)(void *ctx, const uint8_t *data, int len);

enum {
    H1_ST_STATUS = 0,   /* awaiting the status line */
    H1_ST_HEADER,       /* awaiting a header line (or the blank line) */
    H1_ST_BODY_LEN,     /* Content-Length body, `remain` bytes to go */
    H1_ST_BODY_EOF,     /* body runs to connection close */
    H1_ST_CHUNK_SIZE,
    H1_ST_CHUNK_DATA,
    H1_ST_CHUNK_CRLF,   /* the CRLF that follows chunk data */
    H1_ST_TRAILER,
    H1_ST_DONE,
    H1_ST_ERROR
};

struct h1_response {
    int  state;
    int  err;
    int  code;                      /* 200, 404, ... */
    int  minor;                     /* HTTP/1.<minor> */
    char reason[H1_REASON_MAX];
    struct h1_headers hdr;
    struct h1_headers trailer;      /* chunked trailer section, kept separate */

    uint8_t *body;                  /* always NUL-terminated at [body_len] */
    int      body_len, body_cap;

    /* framing state */
    int      chunked;
    int64_t  clen;                  /* declared Content-Length, or -1 */
    int64_t  remain;                /* bytes left in this body or chunk */
    int      head_request;          /* set before feeding: HEAD has no body */
    int      no_body;               /* 1xx / 204 / 304 / HEAD */
    int      must_close;            /* framing forces the connection shut */
    int      keep_alive;            /* connection may be pooled afterwards */
    int      interim;               /* 1xx responses consumed before this one */
    int      decoded;               /* h1_decode_body() already ran */

    /* limits (h1_response_limit) */
    int      body_max;

    /* incremental delivery (h1_response_sink) */
    h1_body_sink sink;
    void        *sink_ctx;
    int          streaming;         /* decided at headers-complete */
    int64_t      body_seen;         /* entity bytes seen, retained or not */

    /* line accumulator */
    char    *line;
    int      line_len, line_cap, have_line;
    int      hdr_bytes;
};

void h1_response_init(struct h1_response *r);
void h1_response_free(struct h1_response *r);
/* Recycle for the next message on a pooled connection; keeps the limits. */
void h1_response_reset(struct h1_response *r);
void h1_response_limit(struct h1_response *r, int body_max);
/* Tell the parser the request method was HEAD, before feeding any bytes. */
void h1_response_head(struct h1_response *r, int is_head);
/* Register a body sink, before feeding any bytes. cb == NULL restores
 * buffering. Survives h1_response_reset (a pooled connection keeps its sink). */
void h1_response_sink(struct h1_response *r, h1_body_sink cb, void *ctx);
/* 1 once the status line and the header section have been parsed -- i.e. code,
 * reason and hdr are final and the body (if any) is what remains. This is the
 * point at which fetch() is specified to resolve. */
int  h1_response_headers_done(const struct h1_response *r);
/* 1 if body bytes are going to the sink instead of r->body. Meaningful only
 * after h1_response_headers_done(). */
int  h1_response_streaming(const struct h1_response *r);

/* Feed network bytes. Returns how many were CONSUMED (which is < len exactly
 * when the message ended inside the buffer -- the remainder belongs to the
 * next response on a keep-alive connection), or a negative H1_E_*. */
int  h1_response_feed(struct h1_response *r, const void *data, int len);
/* The peer closed. Completes a close-delimited body, else H1_E_TRUNC. */
int  h1_response_eof(struct h1_response *r);
int  h1_response_done(const struct h1_response *r);

/* ---- content coding -------------------------------------------------- */

/* Value for the Accept-Encoding request header (what h1_decode_body can undo). */
const char *h1_accept_encoding(void);
/* Decode the body per Content-Encoding, in place. Idempotent. gzip and
 * deflate only; anything else (br, zstd) is H1_E_ENCODING. */
int  h1_decode_body(struct h1_response *r);

/* ---- redirects ------------------------------------------------------- */

int  h1_is_redirect(int code);
/* Per RFC 9110: 303 (and, in practice, 301/302) rewrite a non-HEAD request to
 * GET and drop the body; 307/308 preserve both. Writes the method to follow
 * into `out` and sets *drop_body. */
int  h1_redirect_method(int code, const char *method, char *out, int outmax, int *drop_body);

/* ---- one exchange over a transport ----------------------------------- */

enum { H1_C_SEND = 0, H1_C_RECV, H1_C_DONE, H1_C_ERROR };

struct h1_conn {
    struct h1_transport t;
    char    *out;                   /* serialized request, owned */
    int      out_len, out_off;
    struct h1_response resp;
    int      state;
    int      err;
    /* Bytes read past the end of the response: the head of the next one. */
    uint8_t  spill[4096];
    int      spill_len;
};

/* Takes ownership of the serialized request buffer (frees it in h1_conn_free). */
int  h1_conn_start(struct h1_conn *c, const struct h1_transport *t, char *req, int reqlen);
void h1_conn_free(struct h1_conn *c);
/* One non-blocking step: writes what it can, reads what is there, feeds the
 * parser. Returns the new H1_C_* state. Never blocks, never sleeps. */
int  h1_conn_pump(struct h1_conn *c);

#endif /* LOGIT_HTTP1_H */
