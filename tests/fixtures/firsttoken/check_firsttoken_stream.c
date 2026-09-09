/* Host repro of the firsttoken shape: a cross-origin POST (JSON content-type,
 * so it is PREFLIGHTED) whose answer is a chunked text/event-stream released
 * a frame at a time, read through fetch() + body.getReader() by the SAME page
 * JS the guest runs.  Built by tests/firsttoken.mk as
 * build-firsttoken/firsttoken_stream_check.
 *
 * WHY THIS EXISTS when tests/unit/stream_test.c already streams: stream_test
 * streams SAME-ORIGIN GETs.  The firsttoken page is the first thing in the
 * tree to stream a response that (a) crossed an origin boundary, (b) needed
 * an OPTIONS preflight, and (c) went out as a POST with a body -- and the
 * guest run of 2026-08-30 stalled exactly there: headers settled (FT-HEADERS
 * 200 text/event-stream) and then not one body chunk ever reached the page's
 * reader.  Same-origin streaming being green proves nothing about that
 * combination, so this file pins it on the host where a step-through costs
 * seconds, not a QEMU boot.
 *
 * The frames below are the first frames of
 * tests/fixtures/firsttoken/oracle_body.sse (an llm7.io recording), trimmed to
 * the three that carry the first tokens; the guest replay gate covers the
 * whole oracle.
 */

#include "stream_net.h"

/* The SSE frames, in wire chunk format. content tokens "1", ",", " ". */
static const char *FRAME1 =
    "data: {\"service_tier\":\"default\",\"id\":\"chatcmpl_x\",\"object\":"
    "\"chat.completion.chunk\",\"model\":\"meta-Llama-3.1-8B-Instruct-Turbo\","
    "\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"1\"}}]}\n\n";
static const char *FRAME2 =
    "data: {\"service_tier\":\"default\",\"id\":\"chatcmpl_x\",\"object\":"
    "\"chat.completion.chunk\",\"model\":\"meta-Llama-3.1-8B-Instruct-Turbo\","
    "\"choices\":[{\"index\":0,\"delta\":{\"content\":\",\"}}]}\n\n";
static const char *FRAME3 =
    "data: {\"service_tier\":\"default\",\"id\":\"chatcmpl_x\",\"object\":"
    "\"chat.completion.chunk\",\"model\":\"meta-Llama-3.1-8B-Instruct-Turbo\","
    "\"choices\":[{\"index\":0,\"delta\":{\"content\":\" \"}}]}\n\n";

/* The page JS the guest runs, verbatim in structure: POST + JSON, then the
 * line-splitting reader loop. GOT accumulates tokens, N counts them, ST is
 * the state machine ('h' = headers seen, 'd' = done, 'e' = error). */
static const char *PAGE_JS =
    "var GOT='', N=0, ST='';"
    "fetch('http://api.example/v1/chat/completions', { method: 'POST',"
    "  headers: { 'Content-Type': 'application/json' },"
    "  body: JSON.stringify({ model: 'x', messages: [{role:'user',content:'c'}],"
    "                          stream: true }) })"
    ".then(function (r) {"
    "  ST = String(r.status);"
    "  var rd = r.body.getReader(), dec = new TextDecoder(), buf = '';"
    "  (function loop() {"
    "    rd.read().then(function (c) {"
    "      if (c.done) { ST = 'd'; return; }"
    "      buf += dec.decode(c.value, { stream: true });"
    "      var i;"
    "      while ((i = buf.indexOf('\\n')) >= 0) {"
    "        var ln = buf.slice(0, i); buf = buf.slice(i + 1);"
    "        if (ln.charCodeAt(ln.length - 1) === 13) ln = ln.slice(0, -1);"
    "        if (ln.indexOf('data:') !== 0) continue;"
    "        var p = ln.slice(5).trim();"
    "        if (!p || p === '[DONE]') continue;"
    "        try {"
    "          var d = JSON.parse(p);"
    "          var tok = d.choices && d.choices[0] && d.choices[0].delta"
    "                  ? d.choices[0].delta.content : null;"
    "          if (tok) { N += 1; GOT += tok; }"
    "        } catch (e) { ST = 'parse:' + e; }"
    "      }"
    "      loop();"
    "    }, function (e) { ST = 'readerr:' + e; });"
    "  })();"
    "}, function (e) { ST = 'fail:' + e.name + ':' + e.message; });";

static void router(struct fakesock *s, const char *method, const char *target)
{
    if (!strcmp(method, "OPTIONS") && !strcmp(target, "/v1/chat/completions")) {
        /* llm7's preflight answer, verbatim in shape. */
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Access-Control-Allow-Origin: *\r\n"
                   "Access-Control-Allow-Methods: DELETE, GET, HEAD, OPTIONS, "
                   "PATCH, POST, PUT\r\n"
                   "Access-Control-Allow-Headers: content-type\r\n"
                   "Access-Control-Max-Age: 600\r\n"
                   "Content-Length: 0\r\n\r\n");
    } else if (!strcmp(method, "POST") && !strcmp(target, "/v1/chat/completions")) {
        /* SSE headers only; the test releases the body a frame at a time,
         * which is how "the response has not finished" stays observable. */
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; "
                   "charset=utf-8\r\nCache-Control: no-store\r\n"
                   "Access-Control-Allow-Origin: *\r\n"
                   "Transfer-Encoding: chunked\r\n\r\n");
        s->avail = s->rsp_len;
        s->finished = 0;
    } else {
        rsp_add(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 8\r\n\r\nno route");
    }
}

static void push_chunk(int fd, const char *payload)
{
    char buf[RSP_MAX];
    snprintf(buf, sizeof buf, "%zx\r\n%s\r\n", strlen(payload), payload);
    fs_push(fd, buf);
}

int main(void)
{
    fake_now = 1000;
    fs_reset();
    fs_set_router(router);
    open_ctx("http://page.example/dir/page.html");

    printf("-- the preflight went out BEFORE the POST --\n");
    run(PAGE_JS);
    settle(40);
    ck(req_count() >= 2, "two requests crossed the wire (preflight + POST)");
    ck(req_has(nth_req(0), "OPTIONS /v1/chat/completions"),
       "request 1 was the OPTIONS preflight");
    ck(req_has(nth_req(0), "Access-Control-Request-Method: POST"),
       "...and it announced the real method");
    ck(req_has(nth_req(1), "POST /v1/chat/completions"),
       "request 2 was the POST itself");

    printf("\n-- the headers settled before any body byte --\n");
    settle(40);
    ckjs("ST === '200'", "the fetch promise settled with status 200");
    ckjs("typeof GOT === 'string' && GOT === ''",
         "...and NOT ONE token had arrived yet (nothing released)");

    printf("\n-- the FIRST token, while the response is still open --\n");
    int fd = fs_live();
    ck(fd >= 0, "the POST socket is still open (response not finished)");
    push_chunk(fd, FRAME1);
    settle(40);
    ckjs("GOT === '1'", "TOKEN #1 reached the page after the first frame alone");
    push_chunk(fd, FRAME2);
    push_chunk(fd, FRAME3);
    settle(40);
    ckjs("GOT === '1, '", "tokens 2 and 3 arrived incrementally");
    ck(fs[fd].finished == 0, "the response STILL has not finished");
    ckjs("ST === '200'", "...and the reader is still waiting for more");

    printf("\n-- the end of the stream --\n");
    push_chunk(fd, "data: [DONE]\n\n");
    fs_push(fd, "0\r\n\r\n");
    fs_finish(fd);
    settle(60);
    ckjs("ST === 'd'", "the reader saw done after the terminating chunk");
    ckjs("GOT === '1, '", "the [DONE] sentinel added no text");

    close_ctx();
    printf("\n%d checks, %d failures\n", checks, failures);
    if (failures) { printf("firsttoken_stream_check: FAIL\n"); return 1; }
    printf("firsttoken_stream_check: ALL PASS\n");
    return 0;
}
