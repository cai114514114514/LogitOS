/* Host tests for streaming: Response.body as a ReadableStream, the
 * text/event-stream framing, EventSource, the incremental TextDecoder, and
 * AbortController actually cancelling.
 *
 * THE ASSERTION THAT MATTERS is not "the text arrived". A fully buffered
 * implementation passes that. It is "the page held the first tokens WHILE the
 * response was still open", and this file is compiled twice to prove that the
 * assertion can fail:
 *
 *     make test-stream            -- the real thing; partial delivery required
 *     make test-stream-control    -- js_webapi.c with -DWEBAPI_NO_STREAM, which
 *                                    is the old buffer-until-complete fetch;
 *                                    partial delivery required NOT to happen
 *
 * If the control ever shows partial delivery, this test is measuring something
 * other than the change.
 */

#include "stream_net.h"

/* ---- the routes ------------------------------------------------------- */

static void router(struct fakesock *s, const char *method, const char *target)
{
    (void)method;
    if (!strncmp(target, "/sse", 4)) {
        /* Headers only. The test releases the body itself, a chunk at a time,
         * which is how "the server has not finished" becomes observable. */
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                   "Cache-Control: no-cache\r\nTransfer-Encoding: chunked\r\n\r\n");
        s->avail = s->rsp_len;
        s->finished = 0;
    } else if (!strcmp(target, "/stall")) {
        /* Queued but not one byte released: the request is in flight and the
         * status line has not arrived, which is where abort-before-headers is. */
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Content-Length: 2\r\n\r\nhi");
        s->avail = -1;
        s->finished = 0;
    } else if (!strcmp(target, "/slow")) {
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Content-Length: 12\r\n\r\n");
        s->avail = s->rsp_len;
        s->finished = 0;
    } else if (!strcmp(target, "/chunked")) {
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Transfer-Encoding: chunked\r\n\r\n"
                   "5\r\nchunk\r\n3\r\ned!\r\n0\r\n\r\n");
    } else if (!strcmp(target, "/wrongtype")) {
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Content-Length: 2\r\n\r\nhi");
    } else if (!strcmp(target, "/notfound")) {
        rsp_add(s, "HTTP/1.1 404 Not Found\r\nContent-Type: text/event-stream\r\n"
                   "Content-Length: 0\r\n\r\n");
    } else if (!strcmp(target, "/utf8")) {
        /* "你好世界" -- every character three bytes, so the 7-byte recv slice
         * cuts two of them in half. */
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Content-Length: 12\r\n\r\n"
                   "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c");
    } else {
        rsp_add(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 8\r\n\r\nno route");
    }
}

/* One SSE chunk, wrapped in chunked framing and released. */
static void sse(int fd, const char *payload)
{
    char buf[2048];
    snprintf(buf, sizeof buf, "%x\r\n%s\r\n", (unsigned)strlen(payload), payload);
    fs_push(fd, buf);
}
/* The same thing, named for the tests that use it to cut a field in half. */
#define sse_raw sse

/* ---- 1. the timing claim ---------------------------------------------- */

static void test_partial_before_complete(void)
{
    printf("\n-- a body the page reads while the server is still writing --\n");
    fs_reset();
    run("var seen = [], sdone = false, serr = null, gotHeaders = false;"
        "var dec = new TextDecoder();"
        "fetch('/slow').then(function (r) {"
        "  gotHeaders = true;"
        "  var rd = r.body.getReader();"
        "  (function loop() { rd.read().then(function (c) {"
        "     if (c.done) { sdone = true; return; }"
        "     seen.push(dec.decode(c.value, { stream: true })); loop();"
        "  }, function (e) { serr = e; }); })();"
        "}, function (e) { serr = e; });");

    settle(10);                       /* connect + request + the headers */
    int fd = fs_live();
    ck(fd >= 0, "the socket is open");

    /* Release the first six bytes of a twelve-byte body and stop. */
    fs_push(fd, "abcdef");
    settle(20);

    int partial = JS_ToBool(ctx, eval("seen.join('').length > 0"));
    int complete = JS_ToBool(ctx, eval("sdone"));

#ifdef WEBAPI_NO_STREAM
    ck(!partial,
       "CONTROL: a buffered fetch delivers NOTHING while the response is open");
    ck(!JS_ToBool(ctx, eval("gotHeaders")),
       "CONTROL: ...and does not even settle the promise until the body is done");
#else
    if (partial) printf("      the page already holds: %s\n", jsstr("seen.join('')"));
    ck(partial, "the page holds body bytes WHILE the response is still open");
    ck(JS_ToBool(ctx, eval("gotHeaders")),
       "fetch() settled at the HEADERS, not at the end of the body");
#endif
    ck(!complete, "and the stream has NOT reported done -- the server is mid-body");

    fs_push(fd, "ghijkl");
    fs_finish(fd);
    settle(30);
    ckjs("sdone === true", "the stream closes when the message completes");
    ckjs("seen.join('') === 'abcdefghijkl'", "and every byte arrived, in order");
    ckjs("serr === null", "no error");
}

/* ---- 2. SSE framing + EventSource ------------------------------------- */

static void test_eventsource(void)
{
    printf("\n-- EventSource and the text/event-stream framing --\n");
    fs_reset();
    run("var msgs = [], ids = [], pings = [], opens = 0, errs = 0;"
        "var es = new EventSource('/sse');"
        "es.onopen = function () { opens++; };"
        "es.onmessage = function (e) { msgs.push(e.data); ids.push(e.lastEventId); };"
        "es.onerror = function () { errs++; };"
        "es.addEventListener('ping', function (e) { pings.push(e.data); });");
    settle(15);
    int fd = fs_live();
    ck(fd >= 0, "EventSource opened a connection");
    ck(req_has(nth_req(0), "Accept: text/event-stream"),
       "...and asked for text/event-stream");
    ckjs("opens === 1", "onopen fired once the headers arrived");
    ckjs("es.readyState === 1", "readyState is OPEN");

    sse(fd, "data: hello\n\n");
    settle(20);
    ckjs("msgs.length === 1 && msgs[0] === 'hello'", "a simple message dispatches");

    /* The one that a naive implementation gets wrong: a field cut in half
     * between two reads.  The chunk itself is split, AND the 7-byte recv slice
     * cuts it again. */
    sse_raw(fd, "data: wo");
    settle(10);
    ckjs("msgs.length === 1", "a half-delivered field dispatches nothing yet");
    sse_raw(fd, "rld\n\n");
    settle(20);
    ckjs("msgs.length === 2 && msgs[1] === 'world'",
         "...and the two halves become one message");

    sse(fd, "data: a\ndata: b\n\n");
    settle(20);
    ckjs("msgs.length === 3 && msgs[2] === 'a\\nb'",
         "several data lines join with a newline (not concatenation)");

    sse(fd, ": keepalive comment\n\n");
    settle(20);
    ckjs("msgs.length === 3", "a comment line dispatches nothing");

    sse(fd, "\n");
    settle(20);
    ckjs("msgs.length === 3", "a blank line with an empty data buffer dispatches nothing");

    sse(fd, "data:nospace\n\n");
    settle(20);
    ckjs("msgs.length === 4 && msgs[3] === 'nospace'",
         "the space after the colon is optional, and only ONE is stripped");

    sse(fd, "data:  two-spaces\n\n");
    settle(20);
    ckjs("msgs[4] === ' two-spaces'", "the second space is data, not syntax");

    sse(fd, "event: ping\ndata: p1\n\n");
    settle(20);
    ckjs("pings.length === 1 && pings[0] === 'p1'", "a named event goes to its listener");
    ckjs("msgs.length === 5", "...and not to onmessage");

    sse(fd, "id: 42\ndata: x\n\n");
    settle(20);
    ckjs("ids[ids.length - 1] === '42'", "id sets lastEventId");

    sse(fd, "data: y\n\n");
    settle(20);
    ckjs("ids[ids.length - 1] === '42'", "...and it persists to the next event");

    sse(fd, "unknown: field\ndata: z\n\n");
    settle(20);
    ckjs("msgs[msgs.length - 1] === 'z'", "an unknown field is ignored, not fatal");

    /* CRLF and bare CR are line terminators too, and a CR at the end of a read
     * must not be treated as a terminator until the next byte is known. */
    sse_raw(fd, "data: crlf\r");
    settle(10);
    sse_raw(fd, "\ndata: two\r\n\r\n");
    settle(20);
    ckjs("msgs[msgs.length - 1] === 'crlf\\ntwo'",
         "a CRLF split across two reads is one terminator, not two");

    /* Reconnection. */
    run("es._retry = 20;");
    sse(fd, "retry: 25\n\n");
    settle(20);
    fs_push(fd, "0\r\n\r\n");
    fs_finish(fd);
    int before = fs_opened;
    settle(60);
    ck(fs_opened > before, "the stream ending triggers a reconnect");
    ckjs("errs >= 1", "...and fires error first, as the spec requires");
    ck(req_has(nth_req(req_count() - 1), "Last-Event-ID: 42"),
       "the reconnect carries Last-Event-ID so the stream resumes");

    run("es.close();");
    settle(10);
    ckjs("es.readyState === 2", "close() moves to CLOSED");
    before = fs_opened;
    settle(60);
    ck(fs_opened == before, "...and a closed EventSource never reconnects again");
}

static void test_eventsource_fatal(void)
{
    printf("\n-- EventSource failures that must NOT retry --\n");
    fs_reset();
    run("var e1 = 0; var s1 = new EventSource('/wrongtype');"
        "s1.onerror = function () { e1++; };");
    settle(40);
    ckjs("e1 >= 1 && s1.readyState === 2",
         "a response that is not text/event-stream is a permanent failure");
    int before = fs_opened;
    settle(60);
    ck(fs_opened == before, "...and is not retried (a 200 text/plain is not a hiccup)");

    fs_reset();
    run("var e2 = 0; var s2 = new EventSource('/notfound');"
        "s2.onerror = function () { e2++; };");
    settle(40);
    ckjs("e2 >= 1 && s2.readyState === 2", "a 404 is a permanent failure too");
    before = fs_opened;
    settle(60);
    ck(fs_opened == before, "...and does not become an infinite request loop");
}

/* ---- 3. chunked framing, split everywhere ----------------------------- */

static void test_chunk_splits(void)
{
    printf("\n-- chunked framing, split at every read size --\n");
    int bad = 0;
    for (int slice = 1; slice <= 12; slice++) {
        fs_reset();
        g_next_slice = slice;
        char js[256];
        snprintf(js, sizeof js, "var CT%d = null; fetch('/chunked')"
                                ".then(function (r) { return r.text(); })"
                                ".then(function (t) { CT%d = t; });", slice, slice);
        run(js);
        settle(200);
        snprintf(js, sizeof js, "CT%d === 'chunked!'", slice);
        JSValue v = eval(js);
        if (!JS_ToBool(ctx, v)) { bad++; printf("      slice %d gave %s\n", slice, "the wrong body"); }
        JS_FreeValue(ctx, v);
    }
    g_next_slice = 7;
    ck(bad == 0, "a chunked body de-chunks identically for every recv size 1..12");
}

/* ---- 4. the incremental decoder --------------------------------------- */

static void test_decoder(void)
{
    printf("\n-- TextDecoder across read boundaries --\n");
    run("var d = new TextDecoder(), out = '';"
        "out += d.decode(new Uint8Array([0xE4,0xBD]), { stream: true });"
        "out += d.decode(new Uint8Array([0xA0,0xE5,0xA5]), { stream: true });"
        "out += d.decode(new Uint8Array([0xBD]), { stream: true });");
    ckjs("out === '\\u4f60\\u597d'",
         "a 3-byte character split across three reads decodes once, whole");
    ckjs("d.decode(new Uint8Array([0x61])) === 'a'", "and the decoder is reusable");

    run("var e = new TextEncoder();"
        "var b = e.encode('\\u4f60A\\u00e9');");
    ckjs("b.length === 6 && b[0] === 0xE4 && b[3] === 0x41 && b[4] === 0xC3",
         "TextEncoder produces UTF-8");
    ckjs("new TextDecoder().decode(e.encode('\\ud83d\\ude00')) === '\\ud83d\\ude00'",
         "a surrogate pair round-trips as a 4-byte sequence");

    fs_reset();
    run("var U = null; fetch('/utf8').then(function (r) { return r.text(); })"
        ".then(function (t) { U = t; });");
    settle(60);
    ckjs("U === '\\u4f60\\u597d\\u4e16\\u754c'",
         "a multi-byte body read 7 bytes at a time still decodes correctly");
}

/* ---- 5. the stream object --------------------------------------------- */

static void test_stream_object(void)
{
    printf("\n-- Response.body semantics --\n");
    fs_reset();
    run("var B = null; fetch('/chunked').then(function (r) { B = r; });");
    settle(60);
    ckjs("B && B.body instanceof ReadableStream", "Response.body is a ReadableStream");
    ckjs("B.bodyUsed === false", "an unread body is not used");
    ckjs("B.body.locked === false", "...and its stream is unlocked");

    run("var C = B.clone(); var T1 = null, T2 = null;"
        "B.text().then(function (t) { T1 = t; }); C.text().then(function (t) { T2 = t; });");
    settle(60);
    ckjs("T1 === 'chunked!' && T2 === 'chunked!'",
         "clone() tees the body so both copies read it");

    fs_reset();
    run("var L = 'no'; fetch('/chunked').then(function (r) {"
        "  var rd = r.body.getReader();"
        "  try { r.body.getReader(); L = 'twice'; } catch (e) { L = e.name; }"
        "});");
    settle(60);
    ckjs("L === 'TypeError'", "a second getReader() on a locked stream throws");

    fs_reset();
    run("var TT = null; fetch('/utf8').then(function (r) {"
        "  return r.body.pipeThrough(new TextDecoderStream()).getReader().read();"
        "}).then(function (c) { TT = c.value; });");
    settle(80);
    ckjs("TT && TT.length > 0 && '\\u4f60\\u597d\\u4e16\\u754c'.indexOf(TT) === 0",
         "pipeThrough(new TextDecoderStream()) yields text chunks");
}

/* ---- 6. AbortController really cancels -------------------------------- */

static void test_abort(void)
{
    printf("\n-- AbortController --\n");
    /* Aborted BEFORE the headers arrived: the promise itself rejects. */
    fs_reset();
    run("var AE = 'pending'; var ac = new AbortController();"
        "fetch('/stall', { signal: ac.signal }).then(function () { AE = 'resolved'; },"
        "  function (e) { AE = e.name; });");
    settle(8);
    int fd = fs_live();
    ck(fd >= 0 && fs[fd].used, "the socket is open with the request sent");
    int closed_before = fs_closed_count;
    run("ac.abort();");
    settle(10);
    ck(fs_closed_count > closed_before,
       "abort() CLOSED the socket -- the transfer stopped on the wire");
    ckjs("AE === 'AbortError'",
         "aborting before the headers rejects the promise with an AbortError");

#ifndef WEBAPI_NO_STREAM
    /* Aborted AFTER the headers: the promise has already settled, so the
     * failure belongs to the body stream -- which is what the spec says and
     * what a browser does when a download is cancelled mid-flight. There is no
     * such moment in the control build, which settles only at completion. */
    fs_reset();
    run("var SE = 'pending', SR = null; var ac3 = new AbortController();"
        "fetch('/slow', { signal: ac3.signal }).then(function (r) { SR = r;"
        "  return r.body.getReader().read().then(function () { SE = 'read'; },"
        "    function (e) { SE = e.name; }); }, function (e) { SE = 'rejected:' + e.name; });");
    settle(10);
    closed_before = fs_closed_count;
    run("ac3.abort();");
    settle(20);
    ck(fs_closed_count > closed_before, "the socket is closed there too");
    ckjs("SR !== null", "the promise had already settled at the headers");
    ckjs("SE === 'AbortError'",
         "...so the abort errors the BODY STREAM, with the AbortError name intact");
#endif

    fs_reset();
    run("var A2 = 'pending'; var ac2 = new AbortController(); ac2.abort();"
        "fetch('/chunked', { signal: ac2.signal }).then(function () { A2 = 'resolved'; },"
        "  function (e) { A2 = e.name; });");
    settle(20);
    ckjs("A2 === 'AbortError'", "an already-aborted signal rejects immediately");

    fs_reset();
    run("var X3 = 0; var x = new XMLHttpRequest();"
        "x.onabort = function () { X3++; };"
        "x.open('GET', '/slow'); x.send();");
    settle(6);
    closed_before = fs_closed_count;
    run("x.abort();");
    settle(10);
    ck(fs_closed_count > closed_before, "XHR.abort() closes the socket too, now");
    ckjs("X3 === 1", "...and fires onabort");
}

int main(void)
{
    fake_now = 1000;
    fs_reset();
    fs_set_router(router);
    open_ctx("http://page.example/dir/index.html");

    test_partial_before_complete();
#ifndef WEBAPI_NO_STREAM
    test_eventsource();
    test_eventsource_fatal();
#endif
    test_chunk_splits();
    test_decoder();
    test_stream_object();
    test_abort();

    close_ctx();

    printf("\n%d checks, %d failures\n", checks, failures);
#ifdef WEBAPI_NO_STREAM
    if (failures) { printf("stream_test (CONTROL): FAIL\n"); return 1; }
    printf("stream_test (CONTROL): ALL PASS -- without streaming, nothing is "
           "delivered before completion, which is what the real build refutes\n");
#else
    if (failures) { printf("stream_test: FAIL\n"); return 1; }
    printf("stream_test: ALL PASS\n");
#endif
    return 0;
}
