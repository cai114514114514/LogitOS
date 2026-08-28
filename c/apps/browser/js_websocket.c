/* `WebSocket` (RFC 6455) as a real EventTarget in ring 3. See js_websocket.h
 * for the contract.
 *
 * THE SOCKET IS DEDICATED, NOT bxfer_open.  js_webapi.c's default net dials
 * through bxfer_open, which pools a connection per ORIGIN and may hand back a
 * socket that ALPN has already put into HTTP/2 -- fine for fetch, where a
 * request is a stream on that connection, and wrong here twice over: an
 * Upgrade cannot ride h2 (RFC 6455 assumes HTTP/1.1 framing end to end), and
 * even over HTTP/1.1 a WebSocket owns the byte stream for the rest of its
 * life -- sharing it with another fetch would interleave two peers' bytes on
 * one socket. So the default net below opens a plain socket with
 * SOCK_F_ALPN_HTTP11 only (no h2 offered at all) and nothing else touches it.
 *
 * THE HANDSHAKE IS AN h1_conn.  A WebSocket Upgrade is a GET request and a
 * 101 response -- ordinary HTTP/1.1 until the status line says otherwise --
 * so c/net/http/http1.c's parser does it: h1_conn_start with a GET whose
 * headers carry Sec-WebSocket-Key/Version[/Protocol], h1_conn_pump until
 * H1_C_DONE (http1.c already treats 101 as a no-body response --
 * http1_test.c pins `code==101 && keep_alive==0`), then this file validates
 * Sec-WebSocket-Accept and the Upgrade/Connection tokens BEFORE trusting a
 * single frame. Bytes h1_conn read past the header section (c->hconn.spill)
 * are the head of the first WebSocket frame and are fed to the frame parser
 * before the socket is read again -- see ws_enter_open.
 *
 * EVERY ASYNCHRONOUS PATH TERMINATES.  This is the bar the whole feature is
 * held to (see the phase-1 scope), above any WPT number: a WebSocket that
 * connects and then never fires open/error/close is worse than one that was
 * never shipped, because feature detection reports true and a page's queued
 * work fills forever with nothing to show for it -- the identical shape to
 * the indexedDB trap documented in js_platform.h. So every state below has an
 * exit that is NOT "wait forever": CONNECTING is bounded by WS_CONNECT_MS,
 * re-armed only by bytes actually arriving; a script's close() while
 * CONNECTING fails the connection on the very next pump rather than waiting
 * for that deadline; CLOSING is bounded by WS_CLOSING_MS; OPEN has
 * deliberately NO idle timeout (an idle WebSocket sitting open is the point
 * of the protocol -- see js_webapi.c's fetch for the same argument about SSE)
 * but every read is checked for a transport error or EOF on every pump, so a
 * dead peer is never silently waited on. `send()` and `close()` are wholly
 * synchronous (validate-and-either-throw-or-act), so neither can leave
 * anything pending by itself.
 *
 * WHAT IS DELIBERATELY NOT HERE, each refused by name rather than half-built
 * -- see the phase-1 scope for the full argument on each:
 *   - WebSocketStream: not defined at all. typeof WebSocketStream ===
 *     'undefined' is the correct answer for a class this file does not ship.
 *   - permessage-deflate: never offered in the handshake, and an
 *     Sec-WebSocket-Extensions we did not offer in the response FAILS the
 *     connection (RFC 6455 4.1) rather than being silently accepted.
 *   - RFC 8441 (WebSocket over HTTP/2): never attempted -- the dedicated
 *     socket above offers http/1.1 ALPN only.
 *   - Redirects on the handshake: a 3xx is a failed connection, not a hop.
 *   - Outgoing keepalive pings: this file answers a peer's ping with a pong
 *     (RFC 6455 5.5.3 "MUST") and never originates one itself.
 */

#include "quickjs.h"
#include "js_websocket.h"
#include "http1.h"
#include "ws.h"
#include "logit_abi.h"     /* SOCK_F_* / SOCK_P_* */
#include <string.h>
#include <stdlib.h>

int printf(const char *, ...);

#ifndef WEBAPI_HOST
#include "logit.h"          /* sock_* + monotonic_ms + getrandom_bytes; ring 3 only */
#endif

/* Masking-key / nonce source. On the device, real entropy (getrandom_bytes,
 * which itself refuses rather than degrade -- see c/apps/logit.h). On the
 * host, stdlib rand() -- host tests are not a security context, and the
 * in-memory peer in tests/unit/ws_test.c unmasks using the key IN the frame,
 * exactly as a real server does, so no determinism is required of this. */
#ifndef WEBAPI_HOST
static int ws_rand(uint8_t *out, int n) { return getrandom_bytes(out, n); }
#else
static int ws_rand(uint8_t *out, int n) { for (int i = 0; i < n; i++) out[i] = (uint8_t)(rand() & 0xFF); return 0; }
#endif

/* ---- the transport, injected -------------------------------------------- */

#ifndef WEBAPI_HOST
static int  d_open(const char *h, int p, int tls)
{ return sock_open(h, p, tls ? (SOCK_F_TLS | SOCK_F_ALPN_HTTP11) : 0); }
static int  d_poll(int fd) { return sock_poll(fd); }
static int  d_send(int fd, const void *b, int n) { return sock_send(fd, b, n); }
static int  d_recv(int fd, void *b, int n) { return sock_recv(fd, b, n); }
static void d_close(int fd) { sock_close(fd); }
static unsigned long long d_now(void) { return monotonic_ms(); }
static const struct wsnet g_default_net = { d_open, d_poll, d_send, d_recv, d_close, d_now };
#else
/* Absent by default on the host: every field NULL.  ws_dial() below treats a
 * NULL open() the same as open() returning < 0 -- a dial that fails
 * immediately and terminates through the ordinary error path on the very
 * next pump, never a hang.  tests/unit/ws_test.c installs an in-memory net
 * before running any page. */
static const struct wsnet g_default_net = { 0, 0, 0, 0, 0, 0 };
#endif
static const struct wsnet *g_wsnet = &g_default_net;
void js_websocket_set_net(const struct wsnet *n) { g_wsnet = n ? n : &g_default_net; }
static unsigned long long now_ms(void) { return g_wsnet && g_wsnet->now_ms ? g_wsnet->now_ms() : 0; }

/* ---- small local utilities (deliberately not shared with js_webapi.c's
 * statics of the same shape -- see that file's own "one jar, two doors" scar
 * tissue; these are a handful of trivial lines, not a list that can drift) */
static void scopy(char *d, const char *s, int max)
{
    int i = 0;
    if (max <= 0) return;
    if (s) while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static int lc_c(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
/* 1 if `value` contains `token` as a comma/whitespace-separated element,
 * case-insensitively -- RFC 7230 6.7's Connection/Upgrade tokens may share a
 * header with others ("Connection: keep-alive, Upgrade"). */
static int header_has_token(const char *value, const char *token)
{
    if (!value) return 0;
    const char *p = value;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        int len = (int)(end - start);
        int tl = (int)strlen(token);
        if (len == tl) {
            int eq = 1;
            for (int i = 0; i < len; i++) if (lc_c((unsigned char)start[i]) != lc_c((unsigned char)token[i])) { eq = 0; break; }
            if (eq) return 1;
        }
    }
    return 0;
}

/* ---- connection table ---------------------------------------------------- */

#define WS_MAX          8            /* concurrent connections; generous but bounded */
#define WS_CONNECT_MS   30000ull     /* idle deadline through DNS/TCP/TLS/handshake */
#define WS_CLOSING_MS    5000ull     /* how long we wait for the peer's Close echo */
#define WS_MSG_MAX      (16*1024*1024)  /* one assembled message, any number of frames */
#define WS_OUT_CAP      (1*1024*1024)   /* outgoing byte buffer, framed bytes included */
#define WS_OUTQ_MAX     64           /* queued un-flushed messages (bufferedAmount entries) */

enum { WSC_FREE = 0, WSC_DIAL, WSC_HANDSHAKE, WSC_OPEN, WSC_CLOSING, WSC_DONE };

struct ws_outqent { size_t end_off; size_t paylen; };

struct wsconn {
    int state;
    int gen;                        /* bumped on release; guards a stale JS handle */
    int fd;
    JSValue self;                   /* the WebSocket instance -- events fire on this */

    /* handshake */
    struct h1_conn hconn;
    char   key_b64[25];
    char   protocols_offered[512];  /* csv, lower-cased copy for the subset check */
    char   protocol[128];           /* negotiated Sec-WebSocket-Protocol, or "" */
    unsigned long long deadline;
    int    abort_requested;         /* close() called while CONNECTING */

    /* open/closing */
    struct ws_parser rx;
    int    close_sent, close_received;
    int    final_wasclean;
    int    final_code;
    char   final_reason[128];

    /* message assembly (incoming) */
    int      rxmsg_active, rxmsg_opcode;
    uint8_t *rxmsg_buf; size_t rxmsg_len, rxmsg_cap;

    /* outgoing byte buffer -- framed bytes, control and data frames alike */
    uint8_t *outbuf; size_t outlen, outoff, outcap;
    struct ws_outqent outq[WS_OUTQ_MAX];
    int    outq_n;
};
static struct wsconn g_ws[WS_MAX];

static int ws_mkid(int slot, int gen) { return (slot << 16) | (gen & 0xFFFF); }
static struct wsconn *ws_lookup(int id)
{
    int slot = (id >> 16) & 0xFFFF, gen = id & 0xFFFF;
    if (slot < 0 || slot >= WS_MAX) return 0;
    if (g_ws[slot].state == WSC_FREE) return 0;
    if (g_ws[slot].gen != gen) return 0;      /* stale handle from a released slot */
    return &g_ws[slot];
}

/* ---- handshake transport (h1_conn over a raw fd) -------------------------- */
static int hs_read(void *c, void *buf, int len)
{
    int fd = (int)(long)c;
    int n = g_wsnet->recv ? g_wsnet->recv(fd, buf, len) : -1;
    if (n > 0) return n;
    if (n == 0) return H1_AGAIN;
    return H1_EOF;
}
static int hs_write(void *c, const void *buf, int len)
{
    int fd = (int)(long)c;
    int n = g_wsnet->send ? g_wsnet->send(fd, buf, len) : -1;
    if (n >= 0) return n;
    return H1_TERR;
}

/* ---- the one shared JS-side fire function, captured at install time ------ */
static JSValue g_ws_fire = { 0 };
#define WS_FIRE_VALID(v) (!JS_IsUndefined(v) && !JS_IsNull(v))

/* ---- release ---------------------------------------------------------- */
static void ws_release(JSContext *ctx, struct wsconn *c)
{
    if (c->fd >= 0 && g_wsnet->close) g_wsnet->close(c->fd);
    c->fd = -1;
    h1_conn_free(&c->hconn);
    memset(&c->hconn, 0, sizeof c->hconn);
    ws_parser_free(&c->rx);
    memset(&c->rx, 0, sizeof c->rx);
    free(c->rxmsg_buf); c->rxmsg_buf = 0; c->rxmsg_cap = 0; c->rxmsg_len = 0; c->rxmsg_active = 0;
    free(c->outbuf); c->outbuf = 0; c->outcap = c->outlen = c->outoff = 0; c->outq_n = 0;
    /* The caller (ws_finish_close/ws_fail_connect) took its OWN dup of self
     * before calling here, to fire the close event after this returns --
     * this is the one that releases the reference js_ws_open took. */
    if (ctx) JS_FreeValue(ctx, c->self);
    c->self = JS_UNDEFINED;
    c->state = WSC_FREE;
    c->gen = (c->gen + 1) & 0xFFFF;
}

/* ---- firing events -------------------------------------------------------
 * `type` selects which event js_websocket_prelude.inc's __wsFire builds:
 * "open"/"error" take no extra args; "message" takes (isBinary, dataPtr,
 * dataLen); "close" takes (wasClean, code, reasonPtr, reasonLen). Passing raw
 * bytes rather than a pre-built JSValue keeps this file the only place that
 * decides text-vs-binary delivery and blob-vs-arraybuffer binaryType. */
static void ws_fire_simple(JSContext *ctx, struct wsconn *c, const char *type)
{
    if (!WS_FIRE_VALID(g_ws_fire)) return;
    JSValue args[2];
    args[0] = JS_DupValue(ctx, c->self);
    args[1] = JS_NewString(ctx, type);
    JSValue r = JS_Call(ctx, g_ws_fire, JS_UNDEFINED, 2, (JSValueConst *)args);
    if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
}
static void ws_fire_message_text(JSContext *ctx, struct wsconn *c, const uint8_t *data, size_t len)
{
    if (!WS_FIRE_VALID(g_ws_fire)) return;
    JSValue args[4];
    args[0] = JS_DupValue(ctx, c->self);
    args[1] = JS_NewString(ctx, "message");
    args[2] = JS_NewStringLen(ctx, (const char *)data, len);
    args[3] = JS_FALSE; /* isBinary */
    JSValue r = JS_Call(ctx, g_ws_fire, JS_UNDEFINED, 4, (JSValueConst *)args);
    if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
    for (int i = 0; i < 4; i++) JS_FreeValue(ctx, args[i]);
}
static void ws_fire_message_binary(JSContext *ctx, struct wsconn *c, const uint8_t *data, size_t len)
{
    if (!WS_FIRE_VALID(g_ws_fire)) return;
    JSValue ab = JS_NewArrayBufferCopy(ctx, data, len);
    JSValue args[4];
    args[0] = JS_DupValue(ctx, c->self);
    args[1] = JS_NewString(ctx, "message");
    args[2] = ab;
    args[3] = JS_TRUE; /* isBinary */
    JSValue r = JS_Call(ctx, g_ws_fire, JS_UNDEFINED, 4, (JSValueConst *)args);
    if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
    for (int i = 0; i < 4; i++) JS_FreeValue(ctx, args[i]);
}
static void ws_fire_close(JSContext *ctx, struct wsconn *c, int wasclean, int code, const char *reason)
{
    if (!WS_FIRE_VALID(g_ws_fire)) return;
    JSValue args[5];
    args[0] = JS_DupValue(ctx, c->self);
    args[1] = JS_NewString(ctx, "close");
    args[2] = wasclean ? JS_TRUE : JS_FALSE;
    args[3] = JS_NewInt32(ctx, code);
    args[4] = JS_NewString(ctx, reason ? reason : "");
    JSValue r = JS_Call(ctx, g_ws_fire, JS_UNDEFINED, 5, (JSValueConst *)args);
    if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
    for (int i = 0; i < 5; i++) JS_FreeValue(ctx, args[i]);
}
static void ws_set_ready(JSContext *ctx, struct wsconn *c, int state)
{ JS_SetPropertyStr(ctx, c->self, "readyState", JS_NewInt32(ctx, state)); }

/* ---- terminal transitions -------------------------------------------------
 * Both of these are the ONLY two places a connection ever stops being
 * pumped: every path above either reaches one of these or is still making
 * progress toward one, which is the whole termination argument. */

/* CONNECTING never became OPEN -- refuse without ever having sent a Close
 * frame (none is defined before the handshake completes). Always wasClean:0,
 * code 1006 -- RFC 6455 7.1.5/7.1.6: no Close frame was ever exchanged. */
static void ws_fail_connect(JSContext *ctx, struct wsconn *c)
{
#ifdef WS_NO_SETTLE
    /* THE NEGATIVE CONTROL for "every asynchronous path terminates" -- see
     * tests/unit/ws_test.c's -DWS_NO_SETTLE build and js_platform.h's
     * indexedDB paragraph this whole feature is held to the same bar as: a
     * connection that reaches a terminal condition and neither fires an
     * event nor releases anything is exactly the "request object instead of
     * an error event" trap. Never defined in a real build -- the test's job
     * is to prove ITS OWN bounded-pump-count loop notices this rather than
     * hanging forever waiting for an event that will never come. */
    (void)ctx; (void)c; return;
#endif
    JSValue self = JS_DupValue(ctx, c->self);
    ws_set_ready(ctx, c, 3 /* CLOSED */);
    ws_fire_simple(ctx, c, "error");
    ws_fire_close(ctx, c, 0, WS_CLOSE_ABNORMAL, "");
    ws_release(ctx, c);
    JS_FreeValue(ctx, self);
}
static void ws_finish_close(JSContext *ctx, struct wsconn *c, int wasclean, int code, const char *reason)
{
#ifdef WS_NO_SETTLE
    (void)ctx; (void)c; (void)wasclean; (void)code; (void)reason; return;
#endif
    JSValue self = JS_DupValue(ctx, c->self);
    ws_set_ready(ctx, c, 3 /* CLOSED */);
    ws_fire_close(ctx, c, wasclean, code, reason);
    ws_release(ctx, c);
    JS_FreeValue(ctx, self);
}

/* ---- outgoing frame queueing ----------------------------------------------
 * `mask` is ALWAYS applied here -- RFC 6455 5.1, "a client MUST mask all
 * frames it sends to the server" -- and the key comes from getrandom_bytes
 * fresh per frame, never reused. If entropy fails, this is a LOCAL fault and
 * the deliberate choice (see js_websocket.h's header, "depends_on" in the
 * phase-1 scope) is to refuse rather than send an unmasked/zero-masked
 * frame: a zero mask is the "flock returns 0" mistake with a wire format. */
static int ws_raw_enqueue(struct wsconn *c, int fin, int opcode, const uint8_t *payload, size_t len)
{
    uint8_t mask[4];
    if (ws_rand(mask, 4) != 0) return -1;
    size_t need = len + WS_HDR_MAX;
    if (c->outlen + need > c->outcap) {
        size_t ncap = c->outcap ? c->outcap * 2 : 4096;
        while (ncap < c->outlen + need) ncap *= 2;
        if (ncap > WS_OUT_CAP) return -1;              /* refused, see file header */
        uint8_t *nb = realloc(c->outbuf, ncap);
        if (!nb) return -1;
        c->outbuf = nb; c->outcap = ncap;
    }
#ifdef WS_NO_MASK
    /* THE NEGATIVE CONTROL for RFC 6455 5.1's client-masking requirement --
     * see tests/unit/ws_test.c's -DWS_NO_MASK build. Sends an UNMASKED
     * frame, which a spec-following server (this file's own in-memory one,
     * built with ws_parser's require_masked=1) MUST refuse. Never defined in
     * a real build; grep the Makefile -- it is not there. */
    int n = ws_frame_write(c->outbuf + c->outlen, c->outcap - c->outlen, fin, opcode, payload, len, 0 /* unmasked */);
    (void)mask;
#else
    int n = ws_frame_write(c->outbuf + c->outlen, c->outcap - c->outlen, fin, opcode, payload, len, mask);
#endif
    if (n < 0) return -1;
    c->outlen += (size_t)n;
    return 0;
}
/* Control frames (pong, close) -- never counted in bufferedAmount (RFC 6455's
 * bufferedAmount is defined over application data, not protocol overhead). */
static int ws_enqueue_control(struct wsconn *c, int opcode, const uint8_t *payload, size_t len)
{ return ws_raw_enqueue(c, 1, opcode, payload, len); }
/* Application messages -- one frame per message (this file never fragments
 * an OUTGOING message; RFC 6455 permits but does not require it, and a
 * single frame is simpler and correct). Tracked in outq so bufferedAmount
 * drains exactly when the bytes actually leave via the transport. */
static int ws_enqueue_message(JSContext *ctx, struct wsconn *c, int opcode, const uint8_t *payload, size_t len)
{
    if (c->outq_n >= WS_OUTQ_MAX) return -1;
    if (ws_raw_enqueue(c, 1, opcode, payload, len) != 0) return -1;
    c->outq[c->outq_n].end_off = c->outlen;
    c->outq[c->outq_n].paylen = len;
    c->outq_n++;
    JSValue cur = JS_GetPropertyStr(ctx, c->self, "bufferedAmount");
    int64_t v = 0; JS_ToInt64(ctx, &v, cur); JS_FreeValue(ctx, cur);
    JS_SetPropertyStr(ctx, c->self, "bufferedAmount", JS_NewInt64(ctx, v + (int64_t)len));
    return 0;
}

/* ---- handshake -------------------------------------------------------- */

static int ws_send_handshake(struct wsconn *c, const char *host, int port, int def_port,
                              const char *target, const char *protocols_csv, const char *origin)
{
    uint8_t nonce[16];
    if (ws_rand(nonce, 16) != 0) return -1;
    ws_make_key(nonce, c->key_b64);

    struct h1_request q;
    if (h1_request_init(&q, "GET", target) != H1_OK) return -1;

    char hostport[256];
    if (port != def_port) {
        char pn[12]; int i = 11; pn[11] = 0; int v = port; if (v == 0) { pn[10] = '0'; i = 10; }
        while (v > 0 && i > 0) { pn[--i] = (char)('0' + v % 10); v /= 10; }
        char tmp[256]; scopy(tmp, host, (int)sizeof tmp);
        int o = (int)strlen(tmp);
        if (o < (int)sizeof tmp - 1) tmp[o++] = ':';
        for (const char *p = pn + i; *p && o < (int)sizeof tmp - 1; p++) tmp[o++] = *p;
        tmp[o] = 0;
        scopy(hostport, tmp, (int)sizeof hostport);
    } else scopy(hostport, host, (int)sizeof hostport);

    h1_request_set_header(&q, "Host", hostport);
    h1_request_set_header(&q, "Upgrade", "websocket");
    h1_request_set_header(&q, "Connection", "Upgrade");
    h1_request_set_header(&q, "Sec-WebSocket-Key", c->key_b64);
    h1_request_set_header(&q, "Sec-WebSocket-Version", "13");
    h1_request_set_header(&q, "User-Agent", "Mozilla/5.0 (LogitOS) Logit/1.0");
    if (origin && *origin) h1_request_set_header(&q, "Origin", origin);
    if (protocols_csv && *protocols_csv) h1_request_set_header(&q, "Sec-WebSocket-Protocol", protocols_csv);

    char *raw = 0; int rawlen = 0;
    int rc = h1_request_build(&q, &raw, &rawlen);
    h1_request_free(&q);
    if (rc != H1_OK || !raw) { free(raw); return -1; }

    struct h1_transport t = { hs_read, hs_write, 0, (void *)(long)c->fd };
    if (h1_conn_start(&c->hconn, &t, raw, rawlen) != H1_OK) { free(raw); return -1; }
    h1_response_head(&c->hconn.resp, 0);
    return 0;
}

/* Validate the 101 response. 0 ok, -1 refuse -- both leave c->hconn intact
 * for the caller to read spill[] before freeing it. */
static int ws_validate_handshake(struct wsconn *c, const char *protocols_lc_csv)
{
    struct h1_response *r = &c->hconn.resp;
    if (r->code != 101) return -1;
    if (!header_has_token(h1_headers_get(&r->hdr, "Upgrade"), "websocket")) return -1;
    if (!header_has_token(h1_headers_get(&r->hdr, "Connection"), "Upgrade")) return -1;
    const char *accept = h1_headers_get(&r->hdr, "Sec-WebSocket-Accept");
#ifndef WS_ACCEPT_ANY
    if (!ws_accept_matches(c->key_b64, accept)) return -1;
#else
    /* THE NEGATIVE CONTROL for RFC 6455's Sec-WebSocket-Accept check -- see
     * tests/unit/ws_test.c's -DWS_ACCEPT_ANY build. Skips the ONE check that
     * proves the peer actually understood the handshake rather than echoing
     * an arbitrary 101. Never defined in a real build. */
    (void)accept;
#endif
    /* No extension negotiated is offered; ANY answer here is one we did not
     * ask for -- RFC 6455 4.1 numbered requirement 8 makes that a failure. */
    if (h1_headers_get(&r->hdr, "Sec-WebSocket-Extensions")) return -1;
    const char *proto = h1_headers_get(&r->hdr, "Sec-WebSocket-Protocol");
    if (proto) {
        if (!protocols_lc_csv || !*protocols_lc_csv) return -1;   /* chose one we never offered */
        char lc[128]; int i = 0;
        for (const char *p = proto; *p && i < (int)sizeof lc - 1; p++) lc[i++] = (char)lc_c((unsigned char)*p);
        lc[i] = 0;
        /* substring match over the comma-joined lower-cased offer list --
         * bounded by the caller having already validated each token as a
         * legal RFC 2616 token with no comma inside it, so a substring hit
         * cannot straddle two entries. */
        if (!strstr(protocols_lc_csv, lc)) return -1;
        scopy(c->protocol, proto, (int)sizeof c->protocol);
    } else {
        c->protocol[0] = 0;
    }
    return 0;
}

static void ws_enter_open(JSContext *ctx, struct wsconn *c)
{
    ws_parser_init(&c->rx, WS_MSG_MAX, 0 /* client: server frames MUST NOT be masked */);
    JS_SetPropertyStr(ctx, c->self, "protocol", JS_NewString(ctx, c->protocol));
    JS_SetPropertyStr(ctx, c->self, "extensions", JS_NewString(ctx, ""));
    c->state = WSC_OPEN;
    ws_set_ready(ctx, c, 1 /* OPEN */);
    ws_fire_simple(ctx, c, "open");
    /* Bytes h1_conn read past the header section are the head of the first
     * WebSocket frame -- see the file header. Feed them now, before the fd
     * is read again, or they are lost. */
    if (c->hconn.spill_len > 0) {
        uint8_t spill[sizeof c->hconn.spill];
        int slen = c->hconn.spill_len;
        memcpy(spill, c->hconn.spill, (size_t)slen);
        h1_conn_free(&c->hconn);
        memset(&c->hconn, 0, sizeof c->hconn);
        extern int ws_feed(JSContext *ctx, struct wsconn *c, const uint8_t *data, int len); /* fwd */
        ws_feed(ctx, c, spill, slen);
    } else {
        h1_conn_free(&c->hconn);
        memset(&c->hconn, 0, sizeof c->hconn);
    }
}

/* 1 if `code`, arriving in a peer's Close frame, is legal to RECEIVE. Distinct
 * from ws_close_code_sendable() (ws.c/ws.h), which governs what WE originate:
 * 1005/1006 describe a closure that carried no Close frame at all and 1004/
 * 1015 are reserved -- RFC 6455 7.4.1. */
static int ws_close_code_receivable(int code)
{
    if (code == 1004 || code == WS_CLOSE_NO_STATUS || code == WS_CLOSE_ABNORMAL || code == 1015) return 0;
    if (code < 1000 || code > 4999) return 0;
    return 1;
}

/* ---- protocol-error / message-too-big / bad-utf8 refusal ------------------
 * "Fail the WebSocket Connection" (RFC 6455 7.1.7): best-effort a Close frame
 * naming the reason, then close without waiting for an echo -- WE found the
 * violation, so there is nothing to wait for. */
static void ws_protocol_fail(JSContext *ctx, struct wsconn *c, int code)
{
    if (!c->close_sent) {
        uint8_t body[2] = { (uint8_t)(code >> 8), (uint8_t)code };
        if (ws_enqueue_control(c, WS_OP_CLOSE, body, 2) == 0) c->close_sent = 1;
    }
    ws_finish_close(ctx, c, 0, code, "");
}

/* ---- incoming message assembly -------------------------------------------- */

static int ws_deliver_message(JSContext *ctx, struct wsconn *c)
{
    if (c->rxmsg_opcode == WS_OP_TEXT) {
        if (!ws_utf8_valid(c->rxmsg_buf ? c->rxmsg_buf : (const uint8_t *)"", c->rxmsg_len)) {
            ws_protocol_fail(ctx, c, WS_CLOSE_INVALID_PAYLOAD);
            return 1;
        }
        ws_fire_message_text(ctx, c, c->rxmsg_buf ? c->rxmsg_buf : (const uint8_t *)"", c->rxmsg_len);
    } else {
        ws_fire_message_binary(ctx, c, c->rxmsg_buf ? c->rxmsg_buf : (const uint8_t *)"", c->rxmsg_len);
    }
    c->rxmsg_active = 0;
    c->rxmsg_len = 0;
    return 1;
}

static int ws_handle_frame(JSContext *ctx, struct wsconn *c)
{
    struct ws_parser *p = &c->rx;
    int work = 0;

    if (p->opcode == WS_OP_PING) {
        if (c->state == WSC_OPEN) {
            if (ws_enqueue_control(c, WS_OP_PONG, p->payload, (size_t)p->paylen) != 0) {
                ws_finish_close(ctx, c, 0, WS_CLOSE_INTERNAL_ERROR, "");
                return 1;
            }
        }
    } else if (p->opcode == WS_OP_PONG) {
        /* unsolicited -- we never originate a ping; ignored per RFC 6455 5.5.3 */
    } else if (p->opcode == WS_OP_CLOSE) {
        int code = WS_CLOSE_NO_STATUS;
        const uint8_t *reason = 0; size_t rlen = 0;
        if (p->paylen == 1) { ws_protocol_fail(ctx, c, WS_CLOSE_PROTOCOL_ERROR); return 1; }
        if (p->paylen >= 2) {
            code = (p->payload[0] << 8) | p->payload[1];
            reason = p->payload + 2; rlen = (size_t)p->paylen - 2;
            /* RFC 6455 7.4.1: 1004, 1005, 1006 and 1015 must never appear ON
             * THE WIRE (they describe closures that carried no Close frame
             * at all, or are reserved) -- receiving one is a protocol error,
             * distinct from ws_close_code_sendable() which governs what WE
             * are allowed to originate. */
            if (!ws_close_code_receivable(code)) { ws_protocol_fail(ctx, c, WS_CLOSE_PROTOCOL_ERROR); return 1; }
            if (!ws_utf8_valid(reason, rlen)) { ws_protocol_fail(ctx, c, WS_CLOSE_INVALID_PAYLOAD); return 1; }
        }
        if (rlen >= sizeof c->final_reason) rlen = sizeof c->final_reason - 1;
        memcpy(c->final_reason, reason ? reason : (const uint8_t *)"", rlen);
        c->final_reason[rlen] = 0;
        c->final_code = (p->paylen >= 2) ? code : WS_CLOSE_NO_STATUS;
        c->close_received = 1;
        c->final_wasclean = 1;
        if (!c->close_sent) {
            /* Echo the same code back (RFC 6455 5.5.1 permits any code; the
             * one we received is the simplest correct answer). */
            uint8_t body[2]; int ec = (c->final_code == WS_CLOSE_NO_STATUS) ? WS_CLOSE_NORMAL : c->final_code;
            body[0] = (uint8_t)(ec >> 8); body[1] = (uint8_t)ec;
            if (ws_enqueue_control(c, WS_OP_CLOSE, body, 2) == 0) c->close_sent = 1;
        }
        if (c->state != WSC_CLOSING) { c->state = WSC_CLOSING; c->deadline = now_ms() + WS_CLOSING_MS; }
        work = 1;
    } else {
        /* CONT / TEXT / BIN -- see RFC 6455 5.4. */
        if (p->opcode == WS_OP_CONT) {
            if (!c->rxmsg_active) { ws_protocol_fail(ctx, c, WS_CLOSE_PROTOCOL_ERROR); return 1; }
        } else {
            if (c->rxmsg_active) { ws_protocol_fail(ctx, c, WS_CLOSE_PROTOCOL_ERROR); return 1; }
            c->rxmsg_active = 1;
            c->rxmsg_opcode = p->opcode;
            c->rxmsg_len = 0;
        }
        if (p->paylen > 0) {
            if (c->rxmsg_len + p->paylen > WS_MSG_MAX) { ws_protocol_fail(ctx, c, WS_CLOSE_MESSAGE_TOO_BIG); return 1; }
            if (c->rxmsg_len + p->paylen + 1 > c->rxmsg_cap) {
                size_t ncap = c->rxmsg_cap ? c->rxmsg_cap * 2 : 4096;
                while (ncap < c->rxmsg_len + p->paylen + 1) ncap *= 2;
                uint8_t *nb = realloc(c->rxmsg_buf, ncap);
                if (!nb) { ws_finish_close(ctx, c, 0, WS_CLOSE_INTERNAL_ERROR, ""); return 1; }
                c->rxmsg_buf = nb; c->rxmsg_cap = ncap;
            }
            memcpy(c->rxmsg_buf + c->rxmsg_len, p->payload, (size_t)p->paylen);
            c->rxmsg_len += (size_t)p->paylen;
        }
        if (p->fin) work = ws_deliver_message(ctx, c);
    }
    return work;
}

/* Feed raw bytes to the frame parser, handling as many complete frames as
 * are present. Returns JS-observable work done. Declared non-static (see the
 * forward decl in ws_enter_open) only so the spill hand-off above can call
 * it before this definition -- everything else in this TU is static. */
int ws_feed(JSContext *ctx, struct wsconn *c, const uint8_t *data, int len)
{
    int work = 0;
    size_t off = 0;
    while (off < (size_t)len && (c->state == WSC_OPEN || c->state == WSC_CLOSING)) {
        int status;
        size_t used = ws_parser_feed(&c->rx, data + off, (size_t)len - off, &status);
        off += used;
        if (status == WS_P_ERROR) { ws_protocol_fail(ctx, c, WS_CLOSE_PROTOCOL_ERROR); return work + 1; }
        if (status == WS_P_FRAME) {
            work += ws_handle_frame(ctx, c);
            ws_parser_next(&c->rx);
            if (c->state == WSC_FREE || c->state == WSC_DONE) return work; /* released mid-loop */
        } else {
            break; /* WS_P_AGAIN -- no more complete frames in this chunk */
        }
    }
    return work;
}

/* ---- per-connection pump -------------------------------------------------- */

static int ws_drain_out(JSContext *ctx, struct wsconn *c)
{
    (void)ctx;
    int work = 0;
    while (c->outoff < c->outlen) {
        int n = g_wsnet->send ? g_wsnet->send(c->fd, c->outbuf + c->outoff, (int)(c->outlen - c->outoff)) : -1;
        if (n < 0) return -1;         /* caller fails the connection */
        if (n == 0) break;
        c->outoff += (size_t)n;
        work = 1;
        while (c->outq_n > 0 && c->outq[0].end_off <= c->outoff) {
            JSValue cur = JS_GetPropertyStr(ctx, c->self, "bufferedAmount");
            int64_t v = 0; JS_ToInt64(ctx, &v, cur); JS_FreeValue(ctx, cur);
            v -= (int64_t)c->outq[0].paylen; if (v < 0) v = 0;
            JS_SetPropertyStr(ctx, c->self, "bufferedAmount", JS_NewInt64(ctx, v));
            memmove(c->outq, c->outq + 1, (size_t)(c->outq_n - 1) * sizeof c->outq[0]);
            c->outq_n--;
        }
    }
    if (c->outoff == c->outlen) c->outoff = c->outlen = 0;
    return work;
}

static int ws_pump_open(JSContext *ctx, struct wsconn *c)
{
    int work = 0;
    int dr = ws_drain_out(ctx, c);
    if (dr < 0) { ws_finish_close(ctx, c, 0, WS_CLOSE_ABNORMAL, ""); return 1; }
    work += dr;

    if (c->state == WSC_CLOSING && c->close_sent && c->close_received && c->outoff == c->outlen) {
        ws_finish_close(ctx, c, c->final_wasclean, c->final_code == WS_CLOSE_NO_STATUS ? WS_CLOSE_NORMAL : c->final_code, c->final_reason);
        return work + 1;
    }

    int bits = g_wsnet->poll ? g_wsnet->poll(c->fd) : -1;
    if (bits < 0 || (bits & SOCK_P_ERROR)) { ws_finish_close(ctx, c, 0, WS_CLOSE_ABNORMAL, ""); return work + 1; }
    if (bits & SOCK_P_READABLE) {
        uint8_t buf[4096];
        int n = g_wsnet->recv ? g_wsnet->recv(c->fd, buf, (int)sizeof buf) : -1;
        if (n > 0) work += ws_feed(ctx, c, buf, n);
        if (c->state == WSC_FREE) return work; /* released inside ws_feed */
    }
    if (bits & SOCK_P_EOF) {
        if (c->state == WSC_CLOSING && c->close_sent && c->close_received)
            ws_finish_close(ctx, c, c->final_wasclean, c->final_code == WS_CLOSE_NO_STATUS ? WS_CLOSE_NORMAL : c->final_code, c->final_reason);
        else
            ws_finish_close(ctx, c, 0, WS_CLOSE_ABNORMAL, "");
        return work + 1;
    }
    if (c->state == WSC_CLOSING && now_ms() > c->deadline) {
        ws_finish_close(ctx, c, 0, WS_CLOSE_ABNORMAL, "");
        return work + 1;
    }
    return work;
}

static int ws_step(JSContext *ctx, struct wsconn *c)
{
    int work = 0;

    if (c->state == WSC_DIAL) {
        if (c->abort_requested) { ws_fail_connect(ctx, c); return 1; }
        if (c->fd < 0) { ws_fail_connect(ctx, c); return 1; }
        int bits = g_wsnet->poll ? g_wsnet->poll(c->fd) : -1;
        if (bits < 0 || (bits & SOCK_P_ERROR)) { ws_fail_connect(ctx, c); return 1; }
        if (bits & SOCK_P_CONNECTED) {
            c->state = WSC_HANDSHAKE;
            c->deadline = now_ms() + WS_CONNECT_MS;
        } else {
            if (now_ms() > c->deadline) { ws_fail_connect(ctx, c); return 1; }
            return 0;
        }
    }

    if (c->state == WSC_HANDSHAKE) {
        if (c->abort_requested) { ws_fail_connect(ctx, c); return 1; }
        uint64_t before = c->hconn.rx_bytes;
        int st = h1_conn_pump(&c->hconn);
        if (c->hconn.rx_bytes != before) { c->deadline = now_ms() + WS_CONNECT_MS; work = 1; }
        if (st == H1_C_ERROR) { ws_fail_connect(ctx, c); return work + 1; }
        if (st == H1_C_DONE) {
            /* protocols_lc_csv was stashed in protocols_offered by __wsOpen */
            if (ws_validate_handshake(c, c->protocols_offered) != 0) { ws_fail_connect(ctx, c); return work + 1; }
            ws_enter_open(ctx, c);
            return work + 1;
        }
        if (now_ms() > c->deadline) { ws_fail_connect(ctx, c); return work + 1; }
        return work;
    }

    if (c->state == WSC_OPEN || c->state == WSC_CLOSING)
        work += ws_pump_open(ctx, c);

    return work;
}

int js_websocket_pump(JSContext *ctx)
{
    int work = 0;
    for (int i = 0; i < WS_MAX; i++)
        if (g_ws[i].state != WSC_FREE) work += ws_step(ctx, &g_ws[i]);
    return work;
}
int js_websocket_pending(void)
{
    for (int i = 0; i < WS_MAX; i++) if (g_ws[i].state != WSC_FREE) return 1;
    return 0;
}
void js_websocket_close(JSContext *ctx)
{
    for (int i = 0; i < WS_MAX; i++)
        if (g_ws[i].state != WSC_FREE) {
            /* Page is going away -- do not fire further events into a
             * runtime about to be freed. Release resources only. */
            struct wsconn *c = &g_ws[i];
            if (c->fd >= 0 && g_wsnet->close) g_wsnet->close(c->fd);
            c->fd = -1;
            h1_conn_free(&c->hconn); memset(&c->hconn, 0, sizeof c->hconn);
            ws_parser_free(&c->rx); memset(&c->rx, 0, sizeof c->rx);
            free(c->rxmsg_buf); c->rxmsg_buf = 0;
            free(c->outbuf); c->outbuf = 0;
            if (ctx) JS_FreeValue(ctx, c->self);
            c->self = JS_UNDEFINED;
            c->state = WSC_FREE;
            c->gen = (c->gen + 1) & 0xFFFF;
        }
    if (ctx && WS_FIRE_VALID(g_ws_fire)) JS_FreeValue(ctx, g_ws_fire);
    g_ws_fire = JS_UNDEFINED;
}

/* ---- JS-facing C primitives ------------------------------------------------
 *
 * __wsOpen(host, port, tls, target, protocolsCsv, protocolsLowerCsv, origin,
 *          self) -> id (int32)
 *
 * NEVER throws for a network reason -- see the file header's termination
 * argument. The only throw is local resource exhaustion (every slot busy),
 * which is synchronous and involves no network, so a throw there does not
 * violate "every async path terminates" (nothing async was ever promised). */
static JSValue js_ws_open(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 8) return JS_ThrowTypeError(ctx, "__wsOpen: wrong arity");
    struct wsconn *c = 0;
    for (int i = 0; i < WS_MAX; i++) if (g_ws[i].state == WSC_FREE) { c = &g_ws[i]; break; }
    if (!c) return JS_ThrowRangeError(ctx, "too many WebSocket connections");

    const char *host = JS_ToCString(ctx, argv[0]);
    int32_t port = 0; JS_ToInt32(ctx, &port, argv[1]);
    int32_t tls = 0; JS_ToInt32(ctx, &tls, argv[2]);
    const char *target = JS_ToCString(ctx, argv[3]);
    const char *proto_csv = JS_ToCString(ctx, argv[4]);
    const char *proto_lc_csv = JS_ToCString(ctx, argv[5]);
    const char *origin = JS_ToCString(ctx, argv[6]);

    int gen = c->gen;
    memset(c, 0, sizeof *c);
    c->gen = gen;
    c->fd = -1;
    c->self = JS_DupValue(ctx, argv[7]);
    c->final_code = WS_CLOSE_NO_STATUS;
    scopy(c->protocols_offered, proto_lc_csv, (int)sizeof c->protocols_offered);

    c->fd = (host && g_wsnet->open) ? g_wsnet->open(host, (int)port, tls != 0) : -1;
    c->state = WSC_DIAL;
    c->deadline = now_ms() + WS_CONNECT_MS;

    if (c->fd >= 0) {
        int def_port = tls ? 443 : 80;
        if (ws_send_handshake(c, host, (int)port, def_port, target && *target ? target : "/",
                               proto_csv, origin) != 0) {
            /* Handshake could not even be BUILT (e.g. entropy failure) --
             * mark the dial as failed; the next pump fails it the same way
             * a dead socket would. No throw: still a transport-shaped
             * failure, not a local-resource one. */
            if (g_wsnet->close) g_wsnet->close(c->fd);
            c->fd = -1;
        }
    }
    /* c->fd < 0 here (open failed OR handshake build failed) is deliberately
     * not an error return -- ws_step's WSC_DIAL branch fails it on the very
     * next pump, asynchronously, exactly like a fetch whose dial failed. */

    if (host) JS_FreeCString(ctx, host);
    if (target) JS_FreeCString(ctx, target);
    if (proto_csv) JS_FreeCString(ctx, proto_csv);
    if (proto_lc_csv) JS_FreeCString(ctx, proto_lc_csv);
    if (origin) JS_FreeCString(ctx, origin);

    return JS_NewInt32(ctx, ws_mkid((int)(c - g_ws), c->gen));
}

/* __wsSend(id, isBinary, data) -- data is a JS string (text) or an
 * ArrayBuffer (binary, already copied/sliced by the JS prelude). Wholly
 * synchronous: nothing here can leave a pending operation. */
static JSValue js_ws_send(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 3) return JS_UNDEFINED;
    int32_t id = 0; JS_ToInt32(ctx, &id, argv[0]);
    struct wsconn *c = ws_lookup(id);
    if (!c) return JS_UNDEFINED;             /* stale handle -- nothing to do */
    int32_t isbin = 0; JS_ToInt32(ctx, &isbin, argv[1]);

    const uint8_t *bytes; size_t len; uint8_t *tofree = 0;
    if (isbin) {
        size_t sz = 0;
        uint8_t *p = JS_GetArrayBuffer(ctx, &sz, argv[2]);
        if (!p) { JS_FreeValue(ctx, JS_GetException(ctx)); return JS_UNDEFINED; }
        bytes = p; len = sz;
    } else {
        size_t sz = 0;
        const char *s = JS_ToCStringLen(ctx, &sz, argv[2]);
        if (!s) return JS_UNDEFINED;
        uint8_t *copy = malloc(sz ? sz : 1);
        if (copy) memcpy(copy, s, sz);
        JS_FreeCString(ctx, s);
        if (!copy && sz) return JS_UNDEFINED;
        bytes = copy; len = sz; tofree = copy;
    }

    if (c->state == WSC_OPEN) {
        int opcode = isbin ? WS_OP_BIN : WS_OP_TEXT;
        if (ws_enqueue_message(ctx, c, opcode, bytes, len) != 0)
            /* Local resource refusal (cap exceeded or entropy failed) --
             * fail the connection rather than silently drop the message.
             * See the file header on "refuse rather than degrade". */
            ws_finish_close(ctx, c, 0, WS_CLOSE_INTERNAL_ERROR, "");
    } else if (c->state == WSC_CLOSING) {
        /* Per spec: bump bufferedAmount, send nothing. */
        JSValue cur = JS_GetPropertyStr(ctx, c->self, "bufferedAmount");
        int64_t v = 0; JS_ToInt64(ctx, &v, cur); JS_FreeValue(ctx, cur);
        JS_SetPropertyStr(ctx, c->self, "bufferedAmount", JS_NewInt64(ctx, v + (int64_t)len));
    }
    /* CONNECTING/CLOSED: the JS wrapper already guards CONNECTING with a
     * throw before calling here; CLOSED discards silently, matching spec. */
    free(tofree);
    return JS_UNDEFINED;
}

/* __wsClose(id, code, reason) -- code/reason are ALREADY validated by the JS
 * wrapper (InvalidAccessError / SyntaxError thrown there, synchronously,
 * before this is ever called). */
static JSValue js_ws_close(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 3) return JS_UNDEFINED;
    int32_t id = 0; JS_ToInt32(ctx, &id, argv[0]);
    struct wsconn *c = ws_lookup(id);
    if (!c) return JS_UNDEFINED;
    int32_t code = 1000; JS_ToInt32(ctx, &code, argv[1]);
    size_t rlen = 0; const char *reason = JS_ToCStringLen(ctx, &rlen, argv[2]);

    if (c->state == WSC_DIAL || c->state == WSC_HANDSHAKE) {
        c->abort_requested = 1;                   /* failed on the NEXT pump, not here */
        ws_set_ready(ctx, c, 2 /* CLOSING */);
    } else if (c->state == WSC_OPEN) {
        uint8_t body[2 + 123];
        body[0] = (uint8_t)(code >> 8); body[1] = (uint8_t)code;
        size_t rl = rlen; if (rl > 123) rl = 123;
        if (reason) memcpy(body + 2, reason, rl);
        if (ws_enqueue_control(c, WS_OP_CLOSE, body, 2 + rl) == 0) c->close_sent = 1;
        c->state = WSC_CLOSING;
        c->deadline = now_ms() + WS_CLOSING_MS;
        ws_set_ready(ctx, c, 2 /* CLOSING */);
    }
    /* CLOSING/CLOSED (WSC_DONE never actually observed -- slot is freed the
     * instant readyState reaches CLOSED): a second close() is a no-op. */
    if (reason) JS_FreeCString(ctx, reason);
    return JS_UNDEFINED;
}

/* ---- install ------------------------------------------------------------- */

#include "js_websocket_prelude.inc"

void js_websocket_install(JSContext *ctx)
{
    for (int i = 0; i < WS_MAX; i++) { g_ws[i].state = WSC_FREE; g_ws[i].fd = -1; g_ws[i].self = JS_UNDEFINED; }
    g_ws_fire = JS_UNDEFINED;

    JSValue g = JS_GetGlobalObject(ctx);
    JSValue fn = JS_Eval(ctx, WS_PRELUDE, sizeof(WS_PRELUDE) - 1, "<websocket>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[websocket] prelude failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, g);
        return;
    }
    JSValue args[3];
    args[0] = JS_NewCFunction(ctx, js_ws_open, "__wsOpen", 8);
    args[1] = JS_NewCFunction(ctx, js_ws_send, "__wsSend", 3);
    args[2] = JS_NewCFunction(ctx, js_ws_close, "__wsClose", 3);
    JSValue hooks = JS_Call(ctx, fn, JS_UNDEFINED, 3, (JSValueConst *)args);
    for (int i = 0; i < 3; i++) JS_FreeValue(ctx, args[i]);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(hooks)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[websocket] prelude call failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, hooks);
        JS_FreeValue(ctx, g);
        return;
    }
    g_ws_fire = JS_GetPropertyStr(ctx, hooks, "fire");
    JS_FreeValue(ctx, hooks);
    JS_FreeValue(ctx, g);
}
