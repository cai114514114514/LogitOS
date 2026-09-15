/* Exercise the REAL browser_rt.c -> HTTP/2 -> HPACK path with the socket
 * fixture already used by h2mux. The consumer has fetch's documented contract:
 * it inspects headers AFTER bxfer_pump returns and can hold only 4096 bytes
 * before that point. Existing h2mux responses were 900 bytes and its streaming
 * test withheld DATA until after headers; neither could expose a coalesced
 * first HEADERS + DATA batch. This test keeps the wire coalescing observable.
 * It does not link QuickJS or claim to measure a guest page. */
#define main h2mux_existing_main
#include "h2mux_test.c"
#undef main

struct header_consumer {
    int headers_observed, early_calls, bytes;
    unsigned char body[16384];
};

static int ordered_sink(void *opaque, const uint8_t *p, int n)
{
    struct header_consumer *s = opaque;
    if (!s->headers_observed) {
        s->early_calls++;
        /* This is js_webapi.c's pre-header hold boundary, not a new body
         * limit: after headers, this same consumer accepts the whole sample. */
        if (n > 4096) return H1_E_TOOLARGE;
    }
    if (n > (int)sizeof s->body - s->bytes) return H1_E_TOOLARGE;
    memcpy(s->body + s->bytes, p, (size_t)n);
    s->bytes += n;
    return H1_OK;
}

static void coalesced_response(int cancel_after_headers, int encoded)
{
    reset_world();
    struct xfer x;
    struct header_consumer sink = {0};
    OK(x_start(&x, "GET", "/first-batch", "h2.example", 443, 0, 0, 0) == 0);
    h1_response_sink(&x.c.resp, ordered_sink, &sink);
    struct hsock *s = sk(x.fd);
    OK(s && s->h2);
    if (!s || !s->h2) { x_free(&x); return; }
    /* Let the normal fixture parse the request, but supply our own response.
     * Suppress its automatic header and body, not any client-side operation. */
    s->h2s.hdr_sent[0] = 1;
    s->h2s.hold_body = 1;
    for (int k = 0; k < 20 && !s->h2s.req_seen[0]; k++) bxfer_pump(&x.c);
    OK(s->h2s.req_seen[0]);
    unsigned char payload[8192];
    for (int k = 0; k < (int)sizeof payload; k++) payload[k] = (unsigned char)(k * 7);
    /* Legal gzip member for 8192 'x' bytes. The adapter must leave encoded
     * bytes buffered for http1.c's existing whole-body decoder. */
    static const unsigned char gz[] = {
        31,139,8,0,0,0,0,0,2,19,237,193,1,13,0,0,0,194,160,218,143,111,14,55,160,0,0,0,0,0,0,0,128,119,3,197,5,57,18,0,32,0,0
    };
    const unsigned char *body = encoded ? gz : payload;
    int size = encoded ? (int)sizeof gz : (int)sizeof payload;
    char cl[24]; snprintf(cl, sizeof cl, "%d", size);
    const char *kv[] = { ":status", "200", "content-length", cl,
        "content-encoding", encoded ? "gzip" : "identity", NULL };
    srv_headers(&s->h2s, 1, kv, 0);
    srv_send(&s->h2s, H2_F_DATA, H2_FLAG_END_STREAM, 1, body, size);
    s->h2s.done_sent[0] = 1;
    printf("coalesced encoded=%d cancel=%d wire_batch=%d body=%d\n",
           encoded, cancel_after_headers, pb_avail(&s->s2c), size);
    for (int k = 0; k < 20 && !h1_response_headers_done(&x.c.resp); k++)
        bxfer_pump(&x.c);
    OKM(sink.early_calls == 0, "header-order: sink called %d times before caller observed headers (err=%d)",
        sink.early_calls, x.c.err);
    OKM(h1_response_headers_done(&x.c.resp), "header-order: response headers unavailable err=%d", x.c.err);
    sink.headers_observed = 1;
    if (cancel_after_headers) {
        x_free(&x);
        OK(sink.bytes == 0);
        return;
    }
    /* A paused consumer does not pump. The adapter must retain the queued
     * bytes without delivering them behind its back. */
    OK(sink.bytes == 0);
    for (int k = 0; k < 30 && x.c.state != H1_C_DONE && x.c.state != H1_C_ERROR; k++)
        bxfer_pump(&x.c);
    OKM(x.c.state == H1_C_DONE, "body completion state=%d err=%d", x.c.state, x.c.err);
    if (encoded) {
        OK(sink.bytes == 0);
        OK(x.c.resp.body_len == size);
        OK(x.c.resp.body && !memcmp(x.c.resp.body, body, (size_t)size));
    } else {
        OKM(sink.bytes == size, "identity body got=%d expected=%d", sink.bytes, size);
        OK(!memcmp(sink.body, body, (size_t)size));
        OK(x.c.resp.body_len == 0);
    }
    x_free(&x);
}

int main(void)
{
    coalesced_response(0, 0);
    coalesced_response(1, 0);
    coalesced_response(0, 1);
    reset_world();
    printf("fetch-header-order: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
