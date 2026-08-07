#ifndef LOGIT_JS_WEBAPI_H
#define LOGIT_JS_WEBAPI_H

#include "quickjs.h"

/* The Web API surface that is not the DOM tree: fetch, XMLHttpRequest,
 * localStorage/sessionStorage, history, a parsed location, URL,
 * URLSearchParams and matchMedia.
 *
 * WHY IT IS A SEPARATE FILE FROM js_page.c.  js_page.c owns the runtime's
 * LIFETIME -- open, close, timers, the microtask pump.  This file owns things
 * that need a network transaction or a store to survive a navigation, and it
 * is the only place in the browser that talks to the socket syscalls.  The
 * split keeps js_page.c host-linkable with no transport at all: every entry
 * point below is declared WEAK when js_page.c includes this header, so the
 * existing host tests that link js_page.c without js_webapi.c still link, and
 * simply come up with no fetch.
 *
 * THE FETCH IS NON-BLOCKING, AND THAT IS THE WHOLE POINT.  A request is a
 * socket opened with sock_open() plus an h1_conn (c/net/http/http1.c) that is
 * stepped from js_webapi_pump(), which js_page_run_due() calls from the
 * browser's main loop.  Nothing here ever waits for the network: between two
 * steps of a transfer the loop keeps draining input, running timers and
 * repainting.  The old SYS_HTTP_GET path did the whole transaction inside one
 * ring-0 syscall, which is why loading a page froze the machine.
 *
 * STREAMING IS REAL, AND IT IS WHY THE REST WORKS.  fetch() resolves when the
 * HEADERS arrive, as the spec says, and Response.body is a ReadableStream that
 * the network fills afterwards -- so a text/event-stream endpoint delivers its
 * first token to script while the response is still open.  That is not a
 * refinement: Kimi and DeepSeek both answer as SSE, token by token, and a
 * buffer-until-complete fetch cannot show a chat reply at all.  EventSource is
 * here too, with the framing and the reconnection.
 *
 * COOKIES AND CORS ARE ENFORCED, NOT IGNORED.  Requests carry Cookie: from
 * c/net/http/cookies.c, responses may set cookies, and document.cookie reads
 * and writes the same jar minus HttpOnly.  Once a page can hold a session, a
 * browser that ignores CORS is not more capable -- it is one where any page the
 * user visits can read their mail.  So cross-origin requests are classified,
 * preflighted when they are not simple, and REFUSED when the server does not
 * opt in.  See the CORS section of js_webapi.c for exactly which cases fail.
 *
 * WHAT IS DELIBERATELY NOT HERE: no FormData/Blob, no sync XHR, no WebSocket,
 * no Service Worker, no connection reuse on this path (one socket per request:
 * c/net/http/hpool.c is behind bfetch, whose interface is GET-only -- see the
 * note at fetch_send_request), and no backpressure beyond a queue-depth stall. */

/* Declared weak for js_page.c only -- see the header comment. js_webapi.c
 * itself includes this without the macro and so defines them strongly. */
/* `__weak__`, not `weak`: mini-libc's features.h is force-included into every
 * browser TU (`-include features.h`) and it #defines `weak`, so the plain
 * spelling expands inside the attribute and stops compiling. */
#ifdef JS_WEBAPI_OPTIONAL
#  define WEBAPI_FN __attribute__((__weak__))
#else
#  define WEBAPI_FN
#endif

/* ---- the transport, injected -------------------------------------------
 * The OS build wires this to sock_open/sock_poll/sock_send/sock_recv (see
 * logit.h); the host test injects an in-memory server. That indirection is
 * what makes "a 302 redirect to a second origin resolves the right body" a
 * host unit test instead of a QEMU boot. Semantics match the syscalls:
 *   open  -> handle >= 0, or < 0 to fail the request immediately
 *   poll  -> SOCK_P_* bits (CONNECTED/READABLE/WRITABLE/EOF/ERROR), or < 0
 *   send  -> bytes taken, 0 = queue full (not an error), < 0 = dead
 *   recv  -> bytes, 0 = nothing yet, < 0 = stream finished or dead
 */
struct webapi_net {
    int  (*open)(const char *host, int port, int tls);
    int  (*poll)(int fd);
    int  (*send)(int fd, const void *buf, int len);
    int  (*recv)(int fd, void *buf, int max);
    void (*close)(int fd);
    unsigned long long (*now_ms)(void);
    /* Unix seconds. now_ms is monotonic and cannot answer "has this cookie
     * expired"; a cookie jar without a wall clock either keeps everything for
     * ever or throws it all away. May be NULL: cookies then behave as though
     * the epoch never advances, which expires nothing and is stated rather
     * than silently wrong. */
    long long (*now_unix)(void);
};
WEBAPI_FN void js_webapi_set_net(const struct webapi_net *n);   /* NULL = default */

/* Install everything into `ctx`. `url` is the document's address, used as the
 * base for relative fetches, as location's value and as the storage origin.
 * Call AFTER js_dom_init (it defines window.location, which nothing else may
 * have claimed) and once per page. */
WEBAPI_FN void js_webapi_install(JSContext *ctx, const char *url);

/* Step every in-flight request. Returns how many JS callbacks (promise
 * resolutions, popstate dispatches) it ran -- 0 means nothing observable
 * happened, which is what the embedder uses to decide whether to repaint.
 * Cheap to call every frame: with nothing in flight it is a loop over a small
 * table and no syscall. */
WEBAPI_FN int  js_webapi_pump(JSContext *ctx);

/* 1 while anything needs pumping (a request in flight, a queued popstate).
 * The browser's loop tests this before calling pump, so an idle page costs a
 * pointer test. */
WEBAPI_FN int  js_webapi_pending(void);

/* Abort every request and release every JSValue held here. MUST run before
 * JS_FreeContext -- the fetch table holds promise resolvers, history holds
 * state objects and matchMedia holds listeners, and JS_FreeRuntime asserts on
 * live objects. Storage is NOT cleared: it outlives the page, which is the
 * only reason it lives in C. */
WEBAPI_FN void js_webapi_close(JSContext *ctx);

/* The viewport matchMedia and window.innerWidth/innerHeight report. Defaults
 * to the browser's fixed content area; the embedder may correct it. */
WEBAPI_FN void js_webapi_set_viewport(int w, int h);

/* A navigation the page asked for by assigning location.href (or calling
 * location.assign/replace/reload). Returns 1 and copies the absolute URL out,
 * or 0 when none is pending; a second call returns 0.
 *
 * NOTHING CONSUMES THIS YET. Assigning location.href therefore updates the
 * location object and does not navigate -- the browser's loader owns
 * navigation, and re-entering a page load from inside a JS callback would free
 * the DOM the caller is standing on. This is the hook for doing it safely from
 * the top of the event loop. */
WEBAPI_FN int  js_webapi_take_navigation(char *out, int max);

#endif /* LOGIT_JS_WEBAPI_H */
