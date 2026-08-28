#ifndef LOGIT_JS_WEBSOCKET_H
#define LOGIT_JS_WEBSOCKET_H

#include "quickjs.h"

/* `WebSocket` (RFC 6455) as a real EventTarget in ring 3.
 *
 * Same shape as js_webapi.h's fetch: a dedicated socket per connection (see
 * js_websocket.c's header comment for why it must NOT be bxfer_open, which
 * fetch's net vtable is wired to and which pools/multiplexes by origin), an
 * h1_conn (c/net/http/http1.c) for the Upgrade handshake, and
 * c/net/http/ws.c for the frame codec once the handshake completes. Stepped
 * from js_websocket_pump(), which js_page_run_due() must call every frame --
 * see the long comment there about what an unwired pump looks like.
 *
 * WEAK for the same reason as js_webapi.h's declarations: js_page.c's own
 * host tests link without this TU and simply have no WebSocket. */
#include "../../../include/weaksym.h"
#ifdef JS_WEBSOCKET_OPTIONAL
#  define WS_JS_FN LOGIT_WEAK
#else
#  define WS_JS_FN
#endif

/* The transport, injected -- same contract as struct webapi_net (js_webapi.h),
 * deliberately a SEPARATE vtable rather than a shared one: fetch's default
 * net is bxfer_open (pooled, multiplexed, ALPN-negotiated h2-or-http/1.1);
 * a WebSocket on a pooled/multiplexed socket would corrupt both fetch's
 * framing and this one's. The device default here dials a raw socket
 * directly (offering http/1.1 ALPN only, since an Upgrade cannot ride h2 --
 * see js_websocket.c). The host default is ABSENT (every field 0): a dial
 * fails immediately and the connection terminates through the ordinary
 * error path -- see the termination argument in js_websocket.c -- rather
 * than silently doing nothing. tests/unit/ws_test.c installs an in-memory
 * one. */
struct wsnet {
    int  (*open)(const char *host, int port, int tls);
    int  (*poll)(int fd);      /* SOCK_P_* bits */
    int  (*send)(int fd, const void *buf, int len);
    int  (*recv)(int fd, void *buf, int max);
    void (*close)(int fd);
    unsigned long long (*now_ms)(void);
};
WS_JS_FN void js_websocket_set_net(const struct wsnet *n);   /* NULL = default */

/* Install `WebSocket` into `ctx`. Call AFTER js_events_install (needs
 * G.EventTarget, G.CloseEvent, G.MessageEvent) and after js_webapi_install
 * (needs G.TextEncoder/TextDecoder and, ideally, js_url_install's G.URL). */
WS_JS_FN void js_websocket_install(JSContext *ctx);

/* Step every connection. Returns how many JS-observable callbacks it ran (an
 * event fired) -- 0 means nothing happened, same convention as
 * js_webapi_pump, and for the same reason: the embedder repaints only when
 * this is non-zero. */
WS_JS_FN int  js_websocket_pump(JSContext *ctx);

/* 1 while any connection needs pumping: CONNECTING, OPEN, or CLOSING with an
 * unflushed close. A CLOSED connection has already released its socket and
 * does not hold this true -- see js_websocket.c's state machine. */
WS_JS_FN int  js_websocket_pending(void);

/* Close every socket and drop every JSValue held here. Call before
 * js_dom_cleanup, same position js_webapi_close has in js_page_close(). */
WS_JS_FN void js_websocket_close(JSContext *ctx);

/* The Mach-O half of the weak declarations above -- see js_webapi.h's
 * identical comment. Emitted only under JS_WEBSOCKET_OPTIONAL, i.e. only in
 * the TU (js_page.c) that may not link js_websocket.c. */
#ifdef JS_WEBSOCKET_OPTIONAL
LOGIT_WEAK_STUB(js_websocket_set_net);
LOGIT_WEAK_STUB(js_websocket_install);
LOGIT_WEAK_STUB(js_websocket_pump);
LOGIT_WEAK_STUB(js_websocket_pending);
LOGIT_WEAK_STUB(js_websocket_close);
#endif

#endif /* LOGIT_JS_WEBSOCKET_H */
