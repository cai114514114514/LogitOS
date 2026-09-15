/* Deterministic gate and host benchmark for HTML raw-text tokenization.
 *
 * Pointer identity is the performance invariant: ordinary script/style/
 * textarea/plaintext/CDATA runs must be returned as borrowed input slices.
 * The semantic checks keep every state-changing byte on the slow WHATWG path.
 * Compile with HTML_RAWTEXT_BYTEWISE for the watched negative control.
 *
 * `--bench` is attribution evidence for this tokenizer path on the host.  It
 * is not a page-load number and must not be quoted as guest browser latency.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "html_tokenizer.h"

static int checks, failures;

static void check(int ok, const char *why)
{
    checks++;
    if (ok) printf("ok: %s\n", why);
    else { printf("FAIL: %s\n", why); failures++; }
}

static int collect(int state, const char *src, size_t len,
                   unsigned char *out, size_t cap)
{
    struct html_tokenizer t;
    struct html_token tok;
    size_t n = 0;
    html_tok_init(&t, src, len, 1);
    html_tok_set_state(&t, state);
    for (;;) {
        int r = html_tok_next(&t, &tok);
        if (r < 0) break;
        if (r == 0) { html_tok_free(&t); return -1; }
        if (tok.type == TOK_EOF) break;
        if (tok.type != TOK_CHARS || n + tok.datalen > cap) {
            html_tok_free(&t);
            return -1;
        }
        memcpy(out + n, tok.data, tok.datalen);
        n += tok.datalen;
    }
    html_tok_free(&t);
    return (int)n;
}

static int borrowed_run(int state)
{
    static const char src[] =
        "ordinary characters stay in one borrowed tokenizer run";
    struct html_tokenizer t;
    struct html_token tok;
    html_tok_init(&t, src, sizeof src - 1, 1);
    html_tok_set_state(&t, state);
    int r = html_tok_next(&t, &tok);
    int ok = r == 1 && tok.type == TOK_CHARS && tok.data == src &&
             tok.datalen == sizeof src - 1 &&
             !memcmp(tok.data, src, sizeof src - 1);
    html_tok_free(&t);
    return ok;
}

static void run_checks(void)
{
    int borrowed =
        borrowed_run(HTML_STATE_RCDATA) &&
        borrowed_run(HTML_STATE_RAWTEXT) &&
        borrowed_run(HTML_STATE_SCRIPT_DATA) &&
        borrowed_run(HTML_STATE_PLAINTEXT) &&
        borrowed_run(HTML_STATE_CDATA_SECTION);
    check(borrowed, "ordinary raw-text runs borrow the input buffer");

    unsigned char got[64];
    static const unsigned char rc_expect[] = "a&b\nc";
    static const char rc_in[] = "a&amp;b\r\nc";
    int n = collect(HTML_STATE_RCDATA, rc_in, sizeof rc_in - 1, got, sizeof got);
    check(n == (int)sizeof rc_expect - 1 && !memcmp(got, rc_expect, sizeof rc_expect - 1),
          "RCDATA still expands entities and normalises CRLF");

    static const char nul_in[] = { 'a', 0, 'b' };
    static const unsigned char nul_expect[] = { 'a', 0xef, 0xbf, 0xbd, 'b' };
    const int replace_states[] = {
        HTML_STATE_RCDATA, HTML_STATE_RAWTEXT,
        HTML_STATE_SCRIPT_DATA, HTML_STATE_PLAINTEXT
    };
    int replaced = 1;
    for (unsigned i = 0; i < sizeof replace_states / sizeof replace_states[0]; i++) {
        n = collect(replace_states[i], nul_in, sizeof nul_in, got, sizeof got);
        if (n != (int)sizeof nul_expect || memcmp(got, nul_expect, sizeof nul_expect))
            replaced = 0;
    }
    check(replaced, "RCDATA raw-text script and plaintext still replace NUL");

    static const char cr_in[] = "a\rb\r\nc";
    static const unsigned char cr_expect[] = "a\nb\nc";
    int normalised = 1;
    for (unsigned i = 0; i < sizeof replace_states / sizeof replace_states[0]; i++) {
        n = collect(replace_states[i], cr_in, sizeof cr_in - 1, got, sizeof got);
        if (n != (int)sizeof cr_expect - 1 || memcmp(got, cr_expect, sizeof cr_expect - 1))
            normalised = 0;
    }
    check(normalised, "raw-text states still normalise CR and CRLF");

    static const char cdata_in[] = { 'a', 0, 'b', '\r', '\n', 'c' };
    static const unsigned char cdata_expect[] = { 'a', 0, 'b', '\n', 'c' };
    n = collect(HTML_STATE_CDATA_SECTION, cdata_in, sizeof cdata_in, got, sizeof got);
    check(n == (int)sizeof cdata_expect && !memcmp(got, cdata_expect, sizeof cdata_expect),
          "CDATA preserves NUL while normalising CRLF");

    static const unsigned char raw_expect[] = "a&amp;b";
    n = collect(HTML_STATE_RAWTEXT, "a&amp;b", 7, got, sizeof got);
    check(n == (int)sizeof raw_expect - 1 && !memcmp(got, raw_expect, sizeof raw_expect - 1),
          "RAWTEXT leaves ampersands literal");

    struct html_tokenizer stream;
    struct html_token tok;
    static const char stream_src[] = "stream boundary";
    html_tok_init(&stream, stream_src, 6, 0);
    html_tok_set_state(&stream, HTML_STATE_SCRIPT_DATA);
    int need = html_tok_next(&stream, &tok);
    html_tok_feed(&stream, stream_src, sizeof stream_src - 1, 1);
    int ready = html_tok_next(&stream, &tok);
    check(need == 0 && ready == 1 && tok.type == TOK_CHARS &&
          tok.datalen == sizeof stream_src - 1 &&
          !memcmp(tok.data, stream_src, sizeof stream_src - 1),
          "non-final input boundary rewinds and replays the whole run");
    html_tok_free(&stream);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static volatile uint64_t bench_sink;

static void bench_one(const char *name, int state, const char *src,
                      size_t len, int iters)
{
    uint64_t bytes = 0, tokens = 0;
    uint64_t begin = now_ns();
    for (int i = 0; i < iters; i++) {
        struct html_tokenizer t;
        struct html_token tok;
        html_tok_init(&t, src, len, 1);
        html_tok_set_state(&t, state);
        for (;;) {
            int r = html_tok_next(&t, &tok);
            if (r < 0 || (r == 1 && tok.type == TOK_EOF)) break;
            if (r != 1 || tok.type != TOK_CHARS) abort();
            bytes += tok.datalen;
            tokens++;
            if (tok.datalen) bench_sink +=
                (unsigned char)tok.data[0] + (unsigned char)tok.data[tok.datalen - 1];
        }
        html_tok_free(&t);
    }
    uint64_t elapsed = now_ns() - begin;
    double mib = (double)bytes / 1048576.0;
    printf("state=%-9s bytes=%llu tokens=%llu ns=%llu mib_s=%.2f\n",
           name, (unsigned long long)bytes, (unsigned long long)tokens,
           (unsigned long long)elapsed, mib * 1e9 / (double)elapsed);
}

static int run_bench(int iters)
{
    const size_t len = 1024u * 1024u;
    char *src = malloc(len);
    if (!src || iters <= 0) return 2;
    memset(src, 'a', len);
    bench_one("data",      HTML_STATE_DATA,          src, len, iters);
    bench_one("rcdata",    HTML_STATE_RCDATA,        src, len, iters);
    bench_one("rawtext",   HTML_STATE_RAWTEXT,       src, len, iters);
    bench_one("script",    HTML_STATE_SCRIPT_DATA,   src, len, iters);
    bench_one("plaintext", HTML_STATE_PLAINTEXT,     src, len, iters);
    bench_one("cdata",     HTML_STATE_CDATA_SECTION, src, len, iters);
    free(src);
    return bench_sink == UINT64_MAX ? 3 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--bench"))
        return run_bench(argc > 2 ? atoi(argv[2]) : 64);
    run_checks();
    printf("html raw-text fast path: %d checks, %d failure%s\n",
           checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
