/* ch_sse_test -- the host gate for the SSE reader behind /bin/ch.aex.
 *
 * THE ONE CASE THIS EXISTS FOR. A recorded SSE stream replayed in a single
 * write is not a test of an SSE reader; it is a test of a string search. The
 * bug that this gate is built around is a delta split across a read boundary,
 * and it is invisible to any harness that hands the parser its whole input at
 * once. So the recorded stream here is replayed THREE ways -- whole, one byte
 * at a time, and once for EVERY possible two-way split -- and all three must
 * produce byte-identical output. The split sweep is ~1 KB of stream, so it is
 * about a thousand parses and takes milliseconds.
 *
 * The negative control is `make test-ch-negctl`: the same file compiled against
 * ch_sse.c with -DSSE_NAIVE_CHUNK, which discards the partial line at the start
 * of every feed. Case 1 (whole) still passes there. Cases 2 and 3 must not.
 *
 * No key, no network, no kernel: this links exactly ch_sse.c and the host libc.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ch_sse.h"

static int fails;
#define CHECK(cond, ...) do {                                               \
        if (!(cond)) { fails++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); \
                       printf(__VA_ARGS__); printf("\n"); }                 \
    } while (0)

/* ---- a sink that concatenates everything the parser emits ---- */

struct sink {
    char text[65536];
    int  len;
    int  ndelta;
    int  nrefuse;
    int  last_refuse;
    char refuse_why[256];
};

static void on_delta(void *ctx, const char *u, int n)
{
    struct sink *k = ctx;
    if (k->len + n < (int)sizeof k->text) { memcpy(k->text + k->len, u, n); k->len += n; }
    k->text[k->len] = 0;
    k->ndelta++;
}

static void on_refuse(void *ctx, int code, const char *why)
{
    struct sink *k = ctx;
    k->nrefuse++;
    k->last_refuse = code;
    snprintf(k->refuse_why, sizeof k->refuse_why, "%s", why);
}

/* Feed `stream` in pieces of `step` bytes (step <= 0 means "all at once").
 * Returns the sse_eof() verdict. */
static int run(const char *stream, int len, int step, struct sink *k, struct sse *s)
{
    memset(k, 0, sizeof *k);
    sse_init(s, on_delta, on_refuse, k);
    if (step <= 0) sse_feed(s, stream, len);
    else for (int o = 0; o < len; o += step)
        sse_feed(s, stream + o, (len - o < step) ? len - o : step);
    return sse_eof(s);
}

/* Feed `stream` as exactly two writes split at `cut`. */
static int run_split(const char *stream, int len, int cut, struct sink *k, struct sse *s)
{
    memset(k, 0, sizeof *k);
    sse_init(s, on_delta, on_refuse, k);
    sse_feed(s, stream, cut);
    sse_feed(s, stream + cut, len - cut);
    return sse_eof(s);
}

/* ==================================================================== */
/* The recorded stream. Deliberately not uniform: CRLF and bare-LF records
 * both appear, there is a keep-alive comment, a role-only first frame with no
 * content member, a multi-byte UTF-8 delta, an escaped quote, a surrogate
 * pair, a content:null frame, and the [DONE] terminator. */

static const char REC[] =
    "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\"}}]}\r\n"
    "\r\n"
    ": keep-alive\r\n"
    "\r\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\r\n"
    "\r\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"lo, \"}}]}\n"
    "\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"\\u4f60\\u597d\"}}]}\r\n"
    "\r\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\" \\\"quoted\\\" \"}}]}\r\n"
    "\r\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"\\ud83d\\ude80\"}}]}\r\n"
    "\r\n"
    "data: {\"choices\":[{\"delta\":{\"content\":null},\"finish_reason\":\"stop\"}]}\r\n"
    "\r\n"
    "data: [DONE]\r\n"
    "\r\n";

#define EXPECT "Hello, \xe4\xbd\xa0\xe5\xa5\xbd \"quoted\" \xf0\x9f\x9a\x80"

int main(void)
{
    static struct sse s;
    static struct sink k;
    int n = (int)strlen(REC);

    printf("ch_sse_test: %d bytes of recorded stream\n", n);

    /* ---- 1. whole, in one write ------------------------------------- */
    int rc = run(REC, n, 0, &k, &s);
    CHECK(rc == SSE_OK, "one-shot: eof said %d (%s)", rc, sse_strerror(rc));
    CHECK(sse_done(&s), "one-shot: [DONE] not seen");
    CHECK(strcmp(k.text, EXPECT) == 0, "one-shot text was \"%s\"", k.text);
    CHECK(k.ndelta == 5, "one-shot delta count %d, want 5", k.ndelta);
    CHECK(k.nrefuse == 0, "one-shot refused %d records (%s)", k.nrefuse, k.refuse_why);
    CHECK(s.n_comments == 1, "one-shot comment count %u, want 1", s.n_comments);
    CHECK(s.n_records == 8, "one-shot record count %u, want 8", s.n_records);
    printf("  1. one write ...................... %s\n", fails ? "FAIL" : "ok");

    /* ---- 2. one byte at a time --------------------------------------
     * Every line, every `data:` prefix, every JSON token and every UTF-8
     * escape is split. This is the strongest form of the boundary case and it
     * is where the naive reader dies. */
    int f0 = fails;
    rc = run(REC, n, 1, &k, &s);
    CHECK(rc == SSE_OK, "byte-at-a-time: eof said %d (%s)", rc, sse_strerror(rc));
    CHECK(strcmp(k.text, EXPECT) == 0, "byte-at-a-time text was \"%s\"", k.text);
    CHECK(k.ndelta == 5, "byte-at-a-time delta count %d, want 5", k.ndelta);
    printf("  2. one byte at a time ............. %s\n", fails > f0 ? "FAIL" : "ok");

    /* ---- 3. every possible two-way split ----------------------------- */
    f0 = fails;
    int bad = 0, badcut = -1;
    for (int cut = 0; cut <= n; cut++) {
        run_split(REC, n, cut, &k, &s);
        if (strcmp(k.text, EXPECT) != 0 || k.ndelta != 5 || !sse_done(&s)) {
            if (!bad) badcut = cut;
            bad++;
        }
    }
    CHECK(bad == 0, "%d of %d split points changed the output (first at byte %d)",
          bad, n + 1, badcut);
    printf("  3. all %4d two-way splits ........ %s\n", n + 1, fails > f0 ? "FAIL" : "ok");

    /* ---- 4. odd chunk sizes, including ones that straddle CRLF ------- */
    f0 = fails;
    for (int step = 2; step <= 64; step++) {
        run(REC, n, step, &k, &s);
        CHECK(strcmp(k.text, EXPECT) == 0, "chunk=%d text was \"%s\"", step, k.text);
    }
    printf("  4. chunk sizes 2..64 .............. %s\n", fails > f0 ? "FAIL" : "ok");

    /* ---- 5. a truncated tail ----------------------------------------
     * The server dies mid-JSON. Everything already delivered must still be
     * exactly right, and the verdict must be SSE_E_TRUNC -- not silence, and
     * not a fabricated last token. */
    f0 = fails;
    static const char TRUNC[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"abc\"}}]}\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"conte";
    rc = run(TRUNC, (int)strlen(TRUNC), 0, &k, &s);
    CHECK(rc == SSE_E_TRUNC, "truncated: eof said %d, want SSE_E_TRUNC", rc);
    CHECK(!sse_done(&s), "truncated: reported done");
    CHECK(strcmp(k.text, "abc") == 0, "truncated: text was \"%s\", want \"abc\"", k.text);
    /* And one byte at a time, because the truncation point is also a boundary. */
    rc = run(TRUNC, (int)strlen(TRUNC), 1, &k, &s);
    CHECK(rc == SSE_E_TRUNC, "truncated/1B: eof said %d", rc);
    CHECK(strcmp(k.text, "abc") == 0, "truncated/1B: text was \"%s\"", k.text);
    printf("  5. truncated tail ................. %s\n", fails > f0 ? "FAIL" : "ok");

    /* ---- 6. line endings: CRLF, LF and a bare CR --------------------- */
    f0 = fails;
    static const char CRONLY[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\r\r"
        "data: [DONE]\r\r";
    rc = run(CRONLY, (int)strlen(CRONLY), 0, &k, &s);
    CHECK(rc == SSE_OK, "bare CR: eof said %d", rc);
    CHECK(strcmp(k.text, "x") == 0, "bare CR: text was \"%s\"", k.text);
    /* A CRLF split BETWEEN the CR and the LF is the classic off-by-one. */
    static const char CRLF1[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"y\"}}]}\r\n\r\ndata: [DONE]\r\n\r\n";
    int m = (int)strlen(CRLF1);
    for (int cut = 0; cut <= m; cut++) {
        run_split(CRLF1, m, cut, &k, &s);
        CHECK(strcmp(k.text, "y") == 0 && sse_done(&s),
              "CRLF split at %d gave \"%s\" done=%d", cut, k.text, sse_done(&s));
    }
    printf("  6. CRLF / LF / bare CR ............ %s\n", fails > f0 ? "FAIL" : "ok");

    /* ---- 7. multi-line data: is joined with '\n' --------------------- */
    f0 = fails;
    static const char MULTI[] =
        "data: {\"choices\":[{\"delta\":\n"
        "data: {\"content\":\"ab\"}}]}\n"
        "\n"
        "data: [DONE]\n\n";
    rc = run(MULTI, (int)strlen(MULTI), 0, &k, &s);
    CHECK(rc == SSE_OK, "multi-line: eof said %d", rc);
    CHECK(strcmp(k.text, "ab") == 0, "multi-line: text was \"%s\"", k.text);
    printf("  7. multi-line data: joined ........ %s\n", fails > f0 ? "FAIL" : "ok");

    /* ---- 8. a `"content"` inside somebody's message ------------------
     * The reply quotes JSON back at us. A reader that searched for the literal
     * "content": would take the inner one. */
    f0 = fails;
    static const char NESTED[] =
        "data: {\"choices\":[{\"delta\":{\"content\":"
        "\"try {\\\"content\\\": 1} in your body\"}}]}\n\n"
        "data: [DONE]\n\n";
    rc = run(NESTED, (int)strlen(NESTED), 0, &k, &s);
    CHECK(rc == SSE_OK, "nested: eof said %d", rc);
    CHECK(strcmp(k.text, "try {\"content\": 1} in your body") == 0,
          "nested: text was \"%s\"", k.text);
    printf("  8. \"content\" inside a message ..... %s\n", fails > f0 ? "FAIL" : "ok");

    /* ---- 9. refusals, each one out loud ------------------------------ */
    f0 = fails;
    /* 9a: not JSON at all. The stream must survive it. */
    static const char JUNK[] =
        "data: <html>nope</html>\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"after\"}}]}\n\n"
        "data: [DONE]\n\n";
    rc = run(JUNK, (int)strlen(JUNK), 0, &k, &s);
    CHECK(rc == SSE_OK, "junk: eof said %d", rc);
    CHECK(k.nrefuse == 1 && k.last_refuse == SSE_E_JSON,
          "junk: %d refusals, last %d", k.nrefuse, k.last_refuse);
    CHECK(strcmp(k.text, "after") == 0, "junk: stream did not continue (\"%s\")", k.text);

    /* 9b: a line past the bound. The record it belonged to must be refused
     * whole -- half a JSON object is worse than none. */
    {
        static char big[SSE_LINE_MAX + 4096];
        int o = 0;
        o += sprintf(big + o, "data: {\"choices\":[{\"delta\":{\"content\":\"");
        for (int i = 0; i < SSE_LINE_MAX + 100; i++) big[o++] = 'z';
        o += sprintf(big + o, "\"}}]}\n\n"
                              "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\n"
                              "data: [DONE]\n\n");
        rc = run(big, o, 0, &k, &s);
        CHECK(rc == SSE_OK, "long line: eof said %d", rc);
        CHECK(k.nrefuse >= 1 && s.last_err == SSE_E_LINE,
              "long line: %d refusals, last_err %d", k.nrefuse, s.last_err);
        CHECK(strcmp(k.text, "ok") == 0, "long line: text was \"%s\", want \"ok\"", k.text);
    }

    /* 9c: content that is not a string. */
    static const char NOTSTR[] =
        "data: {\"choices\":[{\"delta\":{\"content\":42}}]}\n\n"
        "data: [DONE]\n\n";
    rc = run(NOTSTR, (int)strlen(NOTSTR), 0, &k, &s);
    CHECK(k.nrefuse == 1 && k.last_refuse == SSE_E_JSON,
          "content:42 -> %d refusals, last %d", k.nrefuse, k.last_refuse);
    CHECK(k.len == 0, "content:42 emitted \"%s\"", k.text);

    /* 9d: content:null is NOT a refusal. A well-formed stream ends with one. */
    static const char NUL[] =
        "data: {\"choices\":[{\"delta\":{\"content\":null}}]}\n\n"
        "data: [DONE]\n\n";
    rc = run(NUL, (int)strlen(NUL), 0, &k, &s);
    CHECK(k.nrefuse == 0, "content:null was refused (%s)", k.refuse_why);
    CHECK(k.ndelta == 0, "content:null produced %d deltas", k.ndelta);
    printf("  9. refusals, and the stream lives .. %s\n", fails > f0 ? "FAIL" : "ok");

    /* ---- 10. the JSON reader on an error body ------------------------
     * A 401 answers with one JSON object, not SSE. Same scanner. */
    f0 = fails;
    static const char ERRBODY[] =
        "{\"error\":{\"message\":\"Invalid Authentication\",\"type\":\"invalid_request_error\","
        "\"param\":null,\"code\":\"invalid_api_key\"}}";
    {
        static const char *const P[2] = { "error", "message" };
        char out[128];
        int r = sse_json_string(ERRBODY, (int)strlen(ERRBODY), P, 2, out, sizeof out);
        CHECK(r > 0 && strcmp(out, "Invalid Authentication") == 0,
              "error.message -> %d \"%s\"", r, r > 0 ? out : "");
        static const char *const Q[2] = { "error", "nosuch" };
        CHECK(sse_json_string(ERRBODY, (int)strlen(ERRBODY), Q, 2, out, sizeof out) < 0,
              "a missing key returned success");
        static const char *const R[3] = { "error", "param", "x" };
        CHECK(sse_json_string(ERRBODY, (int)strlen(ERRBODY), R, 3, out, sizeof out) < 0,
              "descending into null returned success");
    }
    printf(" 10. JSON reader on an error body ... %s\n", fails > f0 ? "FAIL" : "ok");

    /* ---- 11. an empty stream, and a stream of only comments ---------- */
    f0 = fails;
    rc = run("", 0, 0, &k, &s);
    CHECK(rc == SSE_E_TRUNC, "empty stream: eof said %d, want SSE_E_TRUNC", rc);
    rc = run(": a\n\n: b\n\n", 8, 0, &k, &s);
    CHECK(rc == SSE_E_TRUNC, "comments only: eof said %d", rc);
    CHECK(k.ndelta == 0, "comments only produced %d deltas", k.ndelta);
    printf(" 11. empty / comments only .......... %s\n", fails > f0 ? "FAIL" : "ok");

    printf("ch_sse_test: %s (%d failure%s)\n",
           fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
