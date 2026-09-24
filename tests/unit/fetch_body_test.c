/* SPDX-License-Identifier: MIT
 *
 * What goes on the wire for each kind of request body: the exact bytes and the
 * Content-Type, for fetch(), for a Request handed to fetch(), and for
 * XMLHttpRequest.send(). The Fetch Standard calls this "extract a body"
 * (fetch.spec.whatwg.org/#concept-bodyinit-extract); XHR's send() uses the
 * same extraction and then applies its own Content-Type rules.
 *
 * WHY. Cloudflare's managed challenge on chatgpt.com posts its payload from a
 * script, and the JS probe caught "TypeError: ArrayBuffer object expected in
 * fqStart": the native request primitive only understood an ArrayBuffer and
 * fell back to stringifying everything else. Every body type a page can
 * construct therefore has to arrive as exactly the bytes a conforming browser
 * would send -- a string is UTF-8 with text/plain;charset=UTF-8, a typed-array
 * VIEW is its own byteOffset/byteLength window (the classic bug sends the
 * whole underlying buffer), a Blob is its bytes with its type, a FormData is
 * multipart/form-data, and so on.
 *
 * THE ORACLE IS THE SPEC, WRITTEN OUT BY HAND HERE, NOT OUR OWN ENCODER. The
 * multipart body is built below in C from the HTML "multipart/form-data
 * encoding algorithm" (newline normalization, then %0A/%0D/%22 escaping of
 * names and filenames) using only the boundary the page received -- so the
 * encoder in fetch_body_prelude.inc cannot mark its own homework.
 *
 * The transport is tests/unit/stream_net.h's in-memory server; the router
 * below keeps a byte-exact copy of the last request (the fake's own last_req
 * is a C string and would stop at the first NUL of a binary body).
 *
 * Negative controls (tests/fetch_body.mk): -DWEBAPI_FETCH_BODY_AB_ONLY
 * restores the ArrayBuffer-only handling (every other object is stringified)
 * and must fail exactly the view/Blob/FormData checks; and
 * -DWEBAPI_FETCH_BODY_VIEW_WHOLE_BUFFER is the classic view bug, which must
 * fail exactly the checks whose view does not span its whole buffer. */
#include "stream_net.h"

static char cap[REQ_MAX];          /* the most recent request, byte-exact */
static int  cap_len, cap_n;

static void route(struct fakesock *s, const char *method, const char *target)
{
    (void)method; (void)target;
    int n = s->req_len < REQ_MAX ? s->req_len : REQ_MAX;
    memcpy(cap, s->req, (size_t)n);
    cap_len = n;
    cap_n++;
    rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nok");
}

static void fresh(void)
{
    fs_reset();
    fs_set_router(route);
    cap_len = 0; cap_n = 0;
}

/* ---- reading the captured request ------------------------------------- */

static int head_len(void)
{
    for (int i = 0; i + 3 < cap_len; i++)
        if (!memcmp(cap + i, "\r\n\r\n", 4)) return i + 2;   /* keeps the last header's CRLF */
    return -1;
}

static int ci_prefix(const char *line, const char *name)
{
    size_t n = strlen(name);
    for (size_t i = 0; i < n; i++) {
        char a = line[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return line[n] == ':';
}

/* How many header lines carry `name`; the value of the last into out. */
static int hdr(const char *name, char *out, int outcap)
{
    int h = head_len(), count = 0;
    if (out && outcap) out[0] = 0;
    if (h < 0) return 0;
    int i = 0;
    while (i < h && !(cap[i] == '\r' && cap[i + 1] == '\n')) i++;   /* skip the request line */
    i += 2;
    while (i < h) {
        int e = i;
        while (e < h && !(cap[e] == '\r' && cap[e + 1] == '\n')) e++;
        char line[1024];
        int ln = e - i < (int)sizeof line - 1 ? e - i : (int)sizeof line - 1;
        memcpy(line, cap + i, (size_t)ln); line[ln] = 0;
        if (ci_prefix(line, name)) {
            count++;
            const char *v = line + strlen(name) + 1;
            while (*v == ' ') v++;
            if (out) snprintf(out, (size_t)outcap, "%s", v);
        }
        i = e + 2;
    }
    return count;
}

static const char *body_ptr(int *n)
{
    int h = head_len();
    if (h < 0) { *n = -1; return 0; }
    *n = cap_len - (h + 2);
    return cap + h + 2;
}

static int body_is(const void *want, int wn)
{
    int n; const char *b = body_ptr(&n);
    if (n != wn) { printf("      body length %d, want %d\n", n, wn); return 0; }
    if (memcmp(b, want, (size_t)wn)) {
        printf("      body bytes differ:");
        for (int i = 0; i < n && i < 48; i++) printf(" %02x", (unsigned char)b[i]);
        printf("\n");
        return 0;
    }
    return 1;
}

static int ct_is(const char *want)
{
    char v[512];
    int n = hdr("content-type", v, sizeof v);
    if (!want) {
        if (n) printf("      unexpected content-type: %s\n", v);
        return n == 0;
    }
    if (n != 1 || strcmp(v, want)) printf("      content-type x%d: '%s', want '%s'\n", n, v, want);
    return n == 1 && !strcmp(v, want);
}

static int clen_is(const char *want)
{
    char v[64];
    int n = hdr("content-length", v, sizeof v);
    if (!want) return n == 0;
    if (n != 1 || strcmp(v, want)) printf("      content-length x%d: '%s', want '%s'\n", n, v, want);
    return n == 1 && !strcmp(v, want);
}

static int method_is(const char *m)
{
    size_t n = strlen(m);
    return cap_len > (int)n && !memcmp(cap, m, n) && cap[n] == ' ';
}

/* Run one fetch/XHR and let it complete. FETCH_BODY_DUMP=1 in the
 * environment prints each captured request, so the wire can be read by eye. */
static void go(const char *js)
{
    fresh();
    run("var R = { t: null, err: null, load: 0, xerr: 0 };");
    run(js);
    settle(60);
    if (getenv("FETCH_BODY_DUMP")) {
        printf("----- %d request(s); last is %d bytes -----\n", cap_n, cap_len);
        fwrite(cap, 1, (size_t)cap_len, stdout);
        printf("\n-----\n");
    }
}

#define FETCH_THEN ".then(function (r) { return r.text(); }).then(function (t) { R.t = t; })" \
                   ".catch(function (e) { R.err = e.name + ': ' + e.message; });"

/* ---- fetch(url, {body}) ----------------------------------------------- */

static void strings(void)
{
    printf("\n-- string / USVString --\n");
    /* A lone surrogate is not UTF-8-encodable: the USVString conversion turns it
     * into U+FFFD (EF BF BD). QuickJS's own C-string conversion would emit the
     * three-byte WTF-8 form ED A0 80 instead, which no conforming browser sends. */
    go("fetch('/s', { method: 'POST', body: 'h\\u00e9 \\ud83d\\ude00 \\ud800!' })" FETCH_THEN);
    static const unsigned char want[] = { 'h', 0xc3, 0xa9, ' ', 0xf0, 0x9f, 0x98, 0x80, ' ',
                                          0xef, 0xbf, 0xbd, '!' };
    ck(cap_n == 1 && method_is("POST"), "string: one POST reached the server");
    ck(body_is(want, sizeof want), "string: UTF-8 bytes, lone surrogate -> U+FFFD");
    ck(ct_is("text/plain;charset=UTF-8"), "string: default Content-Type text/plain;charset=UTF-8");
    ck(clen_is("13"), "string: Content-Length counts bytes, not UTF-16 units");
    ckjs("R.t === 'ok'", "string: the fetch resolves");

    go("fetch('/s', { method: 'POST', body: '' })" FETCH_THEN);
    ck(body_is("", 0) && clen_is("0"), "empty string: non-null empty body sends Content-Length: 0");
    ck(ct_is("text/plain;charset=UTF-8"), "empty string: still text/plain;charset=UTF-8");

    /* Not a BodyInit type at all: WebIDL converts it to a USVString. */
    go("fetch('/s', { method: 'POST', body: 42 })" FETCH_THEN);
    ck(body_is("42", 2) && ct_is("text/plain;charset=UTF-8"), "number: converted to the string '42'");
    go("fetch('/s', { method: 'POST', body: { a: 1 } })" FETCH_THEN);
    ck(body_is("[object Object]", 15) && ct_is("text/plain;charset=UTF-8"),
       "plain object: converted to '[object Object]' like any USVString");
}

static void buffers(void)
{
    printf("\n-- ArrayBuffer and every ArrayBufferView --\n");
    go("fetch('/b', { method: 'POST', body: new Uint8Array([0, 1, 2, 255]).buffer })" FETCH_THEN);
    ck(body_is("\x00\x01\x02\xff", 4), "ArrayBuffer: exact bytes, including NUL and 0xFF");
    ck(ct_is(0), "ArrayBuffer: no Content-Type (a BufferSource carries none)");
    ck(clen_is("4"), "ArrayBuffer: Content-Length 4");

    /* THE CLASSIC BUG: a view is a window onto a larger buffer. */
    go("var big = new Uint8Array([9, 9, 9, 65, 0, 66, 9, 9]);"
       "fetch('/b', { method: 'PUT', body: big.subarray(3, 6) })" FETCH_THEN);
    ck(body_is("A\x00" "B", 3), "Uint8Array view with byteOffset 3: only its 3 bytes are sent");
    ck(clen_is("3"), "Uint8Array view: Content-Length is the view's byteLength");

    go("var buf = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]).buffer;"
       "fetch('/b', { method: 'POST', body: new DataView(buf, 2, 3) })" FETCH_THEN);
    ck(body_is("\x03\x04\x05", 3), "DataView(buf, 2, 3): honours byteOffset and byteLength");

    go("var buf = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]).buffer;"
       "fetch('/b', { method: 'POST', body: new Int16Array(buf, 2, 2) })" FETCH_THEN);
    ck(body_is("\x03\x04\x05\x06", 4), "Int16Array(buf, 2, 2): 2 elements = 4 bytes from offset 2");

    go("var f = new Float32Array(1); f[0] = 1;"
       "fetch('/b', { method: 'POST', body: f })" FETCH_THEN);
    ck(body_is("\x00\x00\x80\x3f", 4), "Float32Array: the element type is ignored, its bytes are sent");

    /* Whole-buffer views must not regress under either control. */
    go("fetch('/b', { method: 'POST', body: new Uint8Array([65, 66, 67]) })" FETCH_THEN);
    ck(body_is("ABC", 3) && ct_is(0), "whole-buffer Uint8Array: its bytes, no Content-Type");
}

static void blobs(void)
{
    printf("\n-- Blob / File --\n");
    go("fetch('/f', { method: 'POST', body: new Blob(['ab', new Uint8Array([0, 255])],"
       " { type: 'Application/Octet-Stream' }) })" FETCH_THEN);
    ck(body_is("ab\x00\xff", 4), "Blob: its bytes, mixed string and binary parts");
    ck(ct_is("application/octet-stream"), "Blob: Content-Type is blob.type (already normalized)");

    go("fetch('/f', { method: 'POST', body: new Blob(['plain']) })" FETCH_THEN);
    ck(body_is("plain", 5) && ct_is(0), "typeless Blob: its bytes and NO Content-Type");

    go("fetch('/f', { method: 'POST', body: new File(['<p>'], 'x.html', { type: 'text/html' }) })" FETCH_THEN);
    ck(body_is("<p>", 3) && ct_is("text/html"), "File: bytes and its type");

    go("fetch('/f', { method: 'POST', body: new Blob(['0123456789']).slice(2, 5, 'text/x-y') })" FETCH_THEN);
    ck(body_is("234", 3) && ct_is("text/x-y"), "sliced Blob: the slice's bytes and type");
}

/* The expected multipart body, built from the HTML multipart/form-data
 * encoding algorithm with the boundary the page received. */
static int multipart_expect(char *o, const char *b)
{
    int n = 0;
#define PUT(s) do { memcpy(o + n, (s), sizeof(s) - 1); n += (int)sizeof(s) - 1; } while (0)
#define PUTB() do { size_t l_ = strlen(b); o[n++] = '-'; o[n++] = '-'; memcpy(o + n, b, l_); n += (int)l_; } while (0)
    PUTB(); PUT("\r\nContent-Disposition: form-data; name=\"plain\"\r\n\r\n");
    PUT("a\r\nb\r\nc\r\nd\r\n");                                   /* value newlines -> CRLF */
    PUTB(); PUT("\r\nContent-Disposition: form-data; name=\"q%22%0D%0Ax\"\r\n\r\nv\r\n");
    PUTB(); PUT("\r\nContent-Disposition: form-data; name=\"bin\"; filename=\"p%22q.png\"\r\n"
                "Content-Type: image/png\r\n\r\n\x00\xff\r\n\r\n");  /* a File's bytes are untouched */
    PUTB(); PUT("\r\nContent-Disposition: form-data; name=\"file\"; filename=\"h.txt\"\r\n"
                "Content-Type: application/octet-stream\r\n\r\nhi\r\n");
    PUTB(); PUT("\r\nContent-Disposition: form-data; name=\"blob\"; filename=\"blob\"\r\n"
                "Content-Type: application/octet-stream\r\n\r\nB\r\n");
    PUTB(); PUT("\r\nContent-Disposition: form-data; name=\"uni\"\r\n\r\n\xc3\xa9\r\n");
    PUTB(); PUT("--\r\n");
#undef PUT
#undef PUTB
    return n;
}

#define FORMDATA_JS \
    "var fd = new FormData();" \
    "fd.append('plain', 'a\\nb\\rc\\r\\nd');" \
    "fd.append('q\"\\nx', 'v');" \
    "fd.append('bin', new Blob([new Uint8Array([0, 255, 13, 10])], { type: 'image/png' }), 'p\"q.png');" \
    "fd.append('file', new File(['hi'], 'h.txt'));" \
    "fd.append('blob', new Blob(['B']));" \
    "fd.append('uni', '\\u00e9');"

static int boundary_ok(const char *ct, char *boundary, int bcap)
{
    const char *pfx = "multipart/form-data; boundary=";
    if (strncmp(ct, pfx, strlen(pfx))) return 0;
    snprintf(boundary, (size_t)bcap, "%s", ct + strlen(pfx));
    int n = (int)strlen(boundary);
    if (n < 1 || n > 70) return 0;
    for (int i = 0; i < n; i++) {                        /* RFC 2046 bcharsnospace, conservatively */
        char c = boundary[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) return 0;
    }
    return 1;
}

static void check_multipart(const char *who)
{
    char ct[512], boundary[128], want[2048], name[160];
    int n = hdr("content-type", ct, sizeof ct);
    int okb = n == 1 && boundary_ok(ct, boundary, sizeof boundary);
    snprintf(name, sizeof name, "%s: Content-Type multipart/form-data; boundary=<token>", who);
    if (!okb) printf("      content-type x%d: '%s'\n", n, ct);
    ck(okb, name);
    int wn = okb ? multipart_expect(want, boundary) : 0;
    snprintf(name, sizeof name, "%s: exact multipart bytes (normalized, escaped, File parts typed)", who);
    ck(okb && body_is(want, wn), name);
}

static void formdata(void)
{
    printf("\n-- FormData -> multipart/form-data --\n");
    go(FORMDATA_JS "fetch('/m', { method: 'POST', body: fd })" FETCH_THEN);
    check_multipart("fetch FormData");

    /* The FormData entry list itself: a Blob becomes a File named 'blob', a
     * filename argument renames it -- both are the spec's "create an entry". */
    ckjs("(function () { var g = new FormData(); var b = new Blob(['x'], { type: 'text/plain' });"
         " g.append('a', b); g.append('b', b, 'n.txt'); g.set('c', 7);"
         " var a = g.get('a'), c = g.get('b');"
         " return a instanceof File && a.name === 'blob' && a.type === 'text/plain' && a.size === 1"
         "   && c instanceof File && c.name === 'n.txt' && g.get('c') === '7'"
         "   && Object.prototype.toString.call(g) === '[object FormData]'; })()",
         "FormData: a Blob entry becomes a File ('blob' or the given filename)");

    go("fetch('/m', { method: 'POST', body: new FormData() })" FETCH_THEN);
    {
        char ct[512], boundary[128], want[256];
        int okb = hdr("content-type", ct, sizeof ct) == 1 && boundary_ok(ct, boundary, sizeof boundary);
        int wn = okb ? snprintf(want, sizeof want, "--%s--\r\n", boundary) : 0;
        ck(okb && body_is(want, wn), "empty FormData: just the closing delimiter");
    }
}

static void urlsearchparams(void)
{
    printf("\n-- URLSearchParams --\n");
    go("fetch('/u', { method: 'POST', body: new URLSearchParams([['a', '1 2'], ['b', '\\u00e9&=']]) })" FETCH_THEN);
    ck(body_is("a=1+2&b=%C3%A9%26%3D", 20), "URLSearchParams: urlencoded serialization");
    ck(ct_is("application/x-www-form-urlencoded;charset=UTF-8"),
       "URLSearchParams: Content-Type application/x-www-form-urlencoded;charset=UTF-8");
}

static void no_body(void)
{
    printf("\n-- null / undefined / GET / HEAD --\n");
    go("fetch('/n', { method: 'POST', body: null })" FETCH_THEN);
    ck(method_is("POST") && body_is("", 0), "POST body:null sends no body");
    ck(clen_is("0"), "POST with a null body carries Content-Length: 0 (Fetch: POST/PUT)");
    ck(ct_is(0), "null body: no Content-Type");

    go("fetch('/n', { method: 'PATCH', body: undefined })" FETCH_THEN);
    ck(method_is("PATCH") && body_is("", 0) && clen_is(0), "PATCH with no body: no body and no Content-Length");

    go("fetch('/n')" FETCH_THEN);
    ck(method_is("GET") && body_is("", 0) && clen_is(0) && ct_is(0), "plain GET: no body headers");

    go("fetch('/n', { method: 'GET', body: 'x' })" FETCH_THEN);
    ckjs("R.err !== null && R.err.indexOf('TypeError') === 0", "GET with a body rejects with TypeError");
    ck(cap_n == 0, "...and nothing reached the server");
    go("fetch('/n', { method: 'head', body: new Uint8Array(1) })" FETCH_THEN);
    ckjs("R.err !== null && R.err.indexOf('TypeError') === 0", "HEAD with a body rejects with TypeError");
    ck(cap_n == 0, "...and nothing reached the server (HEAD)");
    go("fetch('/n', { body: 'x' })" FETCH_THEN);
    ckjs("R.err !== null && R.err.indexOf('TypeError') === 0", "defaulted GET with a body rejects too");
    go("fetch('/n', { method: 'GET', body: null })" FETCH_THEN);
    ck(cap_n == 1 && method_is("GET"), "GET with body:null is fine");

    /* Streaming uploads are not implemented; say so rather than send nothing
     * or the text "[object ReadableStream]". */
    go("fetch('/n', { method: 'POST', duplex: 'half',"
       " body: new ReadableStream({ start: function (c) { c.enqueue(new Uint8Array([1])); c.close(); } }) })" FETCH_THEN);
    ckjs("R.err !== null && R.err.indexOf('TypeError') === 0 && /ReadableStream/.test(R.err)",
         "ReadableStream body rejects with a TypeError naming ReadableStream");
    ck(cap_n == 0, "...and nothing reached the server (ReadableStream)");
}

static void override(void)
{
    printf("\n-- an explicit Content-Type wins --\n");
    go("fetch('/o', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{\"a\":1}' })" FETCH_THEN);
    ck(ct_is("application/json"), "string body + page Content-Type: the page's value, exactly once");
    ck(body_is("{\"a\":1}", 7), "...and the body is unchanged");

    go("fetch('/o', { method: 'POST', headers: [['content-type', 'text/x-mine']],"
       " body: new Blob(['z'], { type: 'image/gif' }) })" FETCH_THEN);
    ck(ct_is("text/x-mine") && body_is("z", 1), "Blob body + page Content-Type: the page's value wins");

    go(FORMDATA_JS "fetch('/o', { method: 'POST', headers: { 'content-type': 'multipart/form-data; boundary=mine' }, body: fd })" FETCH_THEN);
    ck(ct_is("multipart/form-data; boundary=mine"), "FormData + page Content-Type: the page's value wins");
}

/* ---- a Request carrying the body -------------------------------------- */

static void requests(void)
{
    printf("\n-- fetch(Request) --\n");
    go("var q = new Request('/r', { method: 'POST', body: 'req-body' });"
       "fetch(q)" FETCH_THEN);
    ck(method_is("POST") && body_is("req-body", 8), "fetch(Request): the Request's body is sent");
    ck(ct_is("text/plain;charset=UTF-8"), "fetch(Request): with the Content-Type extraction gave it");
    ckjs("q.bodyUsed === true", "fetch(Request) disturbs the Request's body");
    run("var R2 = null; fetch(q).catch(function (e) { R2 = e.name; });");
    settle(10);
    ckjs("R2 === 'TypeError'", "a second fetch of the same Request rejects: its body is used");

    /* Copy at extraction time: a later write through the view changes nothing. */
    go("var big = new Uint8Array([7, 7, 1, 2, 3, 7]);"
       "var q2 = new Request('/r', { method: 'PUT', body: big.subarray(2, 5) });"
       "big[2] = 99; big[3] = 99;"
       "fetch(q2)" FETCH_THEN);
    ck(body_is("\x01\x02\x03", 3), "Request(view): bytes copied when the Request was built");

    go("var q3 = new Request('/r', { method: 'POST', body: new Blob(['k'], { type: 'text/k' }) });"
       "fetch(q3.clone())" FETCH_THEN);
    ck(body_is("k", 1) && ct_is("text/k"), "fetch(request.clone()): the clone carries the body");

    go("var q4 = new Request('/r', { method: 'POST', body: 'old' });"
       "fetch(q4, { body: 'new' })" FETCH_THEN);
    ck(body_is("new", 3), "fetch(Request, {body}): init.body replaces the Request's");
    ckjs("q4.bodyUsed === false", "...and leaves the Request's own body unused");

    go(FORMDATA_JS "fetch(new Request('/r', { method: 'POST', body: fd }))" FETCH_THEN);
    check_multipart("Request FormData");

    go("var q5 = new Request('/r', { method: 'POST', duplex: 'half',"
       " body: new ReadableStream({ start: function (c) { c.close(); } }) });"
       "fetch(q5)" FETCH_THEN);
    ckjs("R.err !== null && /ReadableStream/.test(R.err)", "fetch(Request with a stream body) names ReadableStream");
    ck(cap_n == 0, "...and nothing reached the server (stream Request)");

    ckjs("(function () { try { new Request(new Request('/r', { method: 'POST', body: 'x' }), { method: 'GET' }); return false; }"
         " catch (e) { return e instanceof TypeError; } })()",
         "new Request(postWithBody, {method:'GET'}) throws TypeError");
}

/* ---- Body mixin: the same extraction on the reading side -------------- */

static void responses(void)
{
    printf("\n-- Response/Request bodies read back --\n");
    /* Three separate evaluations: one that throws must not take the others
     * down with it (at the base commit `new Response({})` threw, and a single
     * run() here silently skipped the URLSearchParams case after it). */
    run("var RB = {};");
    run("var big = new Uint8Array([5, 5, 1, 2, 3, 5]);"
        "var resp = new Response(big.subarray(2, 5)); big[2] = 42;"
        "resp.arrayBuffer().then(function (b) { RB.view = Array.from(new Uint8Array(b)).join(','); });");
    run("new Response({}).text().then(function (t) { RB.obj = t; });");
    run("var rq = new Request('/x', { method: 'POST', body: new URLSearchParams('a=b c') });"
        "RB.rqct = rq.headers.get('content-type');"
        "rq.text().then(function (t) { RB.rq = t; });");
    settle(4);
    ckjs("RB.view === '1,2,3'", "new Response(view): the view's own bytes, copied at construction");
    ckjs("RB.obj === '[object Object]'", "new Response({}): converted to a USVString, not a TypeError");
    ckjs("RB.rq === 'a=b+c' && RB.rqct === 'application/x-www-form-urlencoded;charset=UTF-8'",
         "Request(URLSearchParams): body and Content-Type");
}

/* ---- XMLHttpRequest.send() -------------------------------------------- */

#define XHR_JS(method, setup, body) \
    "var x = new XMLHttpRequest(); x.onload = function () { R.load++; }; x.onerror = function () { R.xerr++; };" \
    "x.open('" method "', '/x');" setup "x.send(" body ");"

static void xhr(void)
{
    printf("\n-- XMLHttpRequest.send() --\n");
    go(XHR_JS("POST", "", "'h\\u00e9'"));
    ck(body_is("h\xc3\xa9", 3) && ct_is("text/plain;charset=UTF-8"), "XHR string: UTF-8 and text/plain;charset=UTF-8");
    ckjs("R.load === 1 && R.xerr === 0", "XHR string: load fires");

    go("var big = new Uint8Array([9, 9, 65, 66, 9]);" XHR_JS("POST", "", "big.subarray(2, 4)"));
    ck(body_is("AB", 2) && ct_is(0), "XHR Uint8Array view with byteOffset: only its bytes, no Content-Type");

    go("var buf = new Uint8Array([1, 2, 3, 4, 5, 6]).buffer;" XHR_JS("POST", "", "new DataView(buf, 1, 2)"));
    ck(body_is("\x02\x03", 2), "XHR DataView with byteOffset: only its bytes");

    go(XHR_JS("POST", "", "new Blob(['bb'], { type: 'text/bb' })"));
    ck(body_is("bb", 2) && ct_is("text/bb"), "XHR Blob: bytes and blob.type");

    go(FORMDATA_JS XHR_JS("POST", "", "fd"));
    check_multipart("XHR FormData");

    go(XHR_JS("POST", "", "new URLSearchParams('k=v w')"));
    ck(body_is("k=v+w", 5) && ct_is("application/x-www-form-urlencoded;charset=UTF-8"), "XHR URLSearchParams");

    go(XHR_JS("POST", "", "null"));
    ck(method_is("POST") && body_is("", 0) && clen_is("0") && ct_is(0), "XHR send(null) on POST: empty, Content-Length: 0");

    /* XHR ignores the body of a GET/HEAD instead of failing the request. */
    go(XHR_JS("GET", "", "'ignored'"));
    ck(cap_n == 1 && method_is("GET") && body_is("", 0) && ct_is(0), "XHR GET: the body is ignored, not sent");
    ckjs("R.load === 1 && R.xerr === 0", "XHR GET with a body still loads");

    /* An author Content-Type wins; for a string body its charset is rewritten to UTF-8. */
    go(XHR_JS("POST", "x.setRequestHeader('Content-Type', 'text/plain; charset=ISO-8859-1');", "'x'"));
    ck(ct_is("text/plain;charset=UTF-8") && body_is("x", 1), "XHR string: author charset rewritten to UTF-8");
    go(XHR_JS("POST", "x.setRequestHeader('Content-Type', 'application/json');", "'{}'"));
    ck(ct_is("application/json"), "XHR string: author Content-Type without charset is kept as set");
    go(XHR_JS("POST", "x.setRequestHeader('content-type', 'application/x-mine');", "new Blob(['q'], { type: 'text/q' })"));
    ck(ct_is("application/x-mine"), "XHR Blob: author Content-Type wins over blob.type");

    /* XMLHttpRequestBodyInit has no ReadableStream: WebIDL makes it a string. */
    go(XHR_JS("POST", "", "new ReadableStream({})"));
    ck(body_is("[object ReadableStream]", 23) && ct_is("text/plain;charset=UTF-8"),
       "XHR ReadableStream: converted to a USVString, as WebIDL requires");
}

int main(void)
{
    open_ctx("http://page.example/dir/index.html");
    strings();
    buffers();
    blobs();
    formdata();
    urlsearchparams();
    no_body();
    override();
    requests();
    responses();
    xhr();
    close_ctx();
    printf("\nfetch-body: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
