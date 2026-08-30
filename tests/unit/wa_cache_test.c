/* wa_cache_test.c -- host unit test for the cross-navigation HTTP cache's
 * POLICY (the guest measures the speed; this pins the rules the speed is
 * allowed to follow).
 *
 * WHAT IS AND IS NOT HERE: the freshness rules, the validator arms, the
 * refusal rules (no-store, foreign Vary), the clamps, replacement, eviction,
 * and the 304 refresh -- everything http_cache.c decides from header values.
 * NOT here: anything about connections, pools or dials; those belong to the
 * guest gate (tests/qmp/qmp_webaccel.py), which measures them on the machine
 * they run on.
 *
 * THE CLOCK IS FAKE AND THAT IS THE POINT: WAC_NOW_MS() is defined to a
 * controllable counter before http_cache.c is included, so "after 59 s" and
 * "after 61 s" are two assignments, not two sleeps -- a freshness test that
 * sleeps is a test that gets deleted the first time CI is slow.
 *
 * THE NEGATIVE CONTROL is -DWACACHE_OFF on THIS file's compile line: the
 * cache compiles to nothing, every lookup misses, and every assertion below
 * that expects a hit must FAIL. tests/webaccel.mk inverts that failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long long fake_ms;
#define WAC_NOW_MS() fake_ms
#include "../../c/apps/browser/http_cache.c"

static int fails;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } \
} while (0)

/* The one date every RFC uses. 784111777 = Sun, 06 Nov 1994 08:49:37 GMT. */
static void test_dates(void)
{
    CHECK(parse_imf_fixdate("Sun, 06 Nov 1994 08:49:37 GMT") == 784111777,
          "IMF-fixdate parses to its unix seconds");
    CHECK(parse_imf_fixdate("Sun, 06 Nov 1994 08:49:37 GMT ") == 784111777,
          "trailing space is tolerated");
    CHECK(parse_imf_fixdate("Sunday, 06-Nov-94 08:49:37 GMT") < 0,
          "the obsolete RFC-850 form is refused, not half-parsed");
    CHECK(parse_imf_fixdate("Sun Nov  6 08:49:37 1994") < 0,
          "the obsolete asctime form is refused");
    CHECK(parse_imf_fixdate("Sun, 06 Nov 1994 08:49:37 EST") < 0,
          "a non-GMT zone is refused (we would need a tz table to honour it)");
    CHECK(parse_imf_fixdate("Sun, 99 Nov 1994 08:49:37 GMT") < 0,
          "day 99 is refused");
    CHECK(parse_imf_fixdate(0) < 0, "NULL is refused");
}

static void test_cc_tokens(void)
{
    struct cc_parse cc;
    parse_cc("public, max-age=120", &cc);
    CHECK(!cc.no_store && !cc.no_cache && cc.max_age_ms == 120000,
          "max-age in a token list");
    parse_cc("no-cache", &cc);
    CHECK(cc.no_cache && cc.max_age_ms < 0, "no-cache alone");
    parse_cc("max-age = 5", &cc);
    CHECK(cc.max_age_ms == 5000, "spaces around '=' are tolerated");
    parse_cc("s-maxage=999, max-age=7", &cc);
    CHECK(cc.max_age_ms == 7000,
          "s-maxage does not shadow max-age for a private cache");
    parse_cc("maximal-age=9", &cc);
    CHECK(cc.max_age_ms < 0, "a token that merely CONTAINS max-age is not it");
    parse_cc(0, &cc);
    CHECK(cc.max_age_ms < 0 && !cc.no_store, "absent header");
    CHECK(val_has_token("no-store, must-revalidate", "no-store"),
          "token after a comma is found");
    CHECK(!val_has_token("no-stored", "no-store"),
          "a longer token is not a substring hit");
}

static void test_freshness(void)
{
    unsigned char *b; int n;
    const unsigned char doc[] = "body-bytes";

    wacache_reset(); fake_ms = 1000;
    /* max-age=60: fresh at +59, stale at +61. */
    CHECK(wacache_store("/a", doc, sizeof doc, "max-age=60", 0, 0, 0, 0, 0, 0) == 0,
          "max-age entry stores");
    fake_ms = 1000 + 59000;
    CHECK(wacache_lookup("/a", &b, &n) == 0, "fresh inside max-age");
    free(b);
    fake_ms = 1000 + 61000;
    CHECK(wacache_lookup("/a", &b, &n) == -1, "stale past max-age");

    /* Stale is where validators arm. */
    char et[256], lm[64];
    CHECK(wacache_validators("/a", et, sizeof et, lm, sizeof lm) == 0,
          "a stale entry arms its validators");
    CHECK(et[0] == 0 && lm[0] == 0, "no ETag/Last-Modified was stored");

    /* no-store: nothing stored at all. */
    wacache_reset(); fake_ms = 1000;
    CHECK(wacache_store("/ns", doc, sizeof doc, "no-store", 0, 0, 0, 0, 0, 0) == -1,
          "no-store refuses the store");
    CHECK(wacache_lookup("/ns", &b, &n) == -1, "and nothing is servable");

    /* no-cache: stored, but lifetime 0 -- only ever via validators. */
    wacache_reset(); fake_ms = 1000;
    CHECK(wacache_store("/nc", doc, sizeof doc, "no-cache", 0, 0, 0, 0, 0, 0) == 0,
          "no-cache stores the body");
    CHECK(wacache_lookup("/nc", &b, &n) == -1, "no-cache never serves fresh");
    CHECK(wacache_body("/nc", &b, &n) == 0 && n == (int)sizeof doc,
          "but a 304 can still pull it");
    free(b);

    /* Expires minus Date, the no-wall-clock lifetime. */
    wacache_reset(); fake_ms = 1000;
    CHECK(wacache_store("/ex", doc, sizeof doc, 0,
                        "Sun, 06 Nov 1994 08:51:37 GMT",
                        "Sun, 06 Nov 1994 08:49:37 GMT", 0, 0, 0, 0) == 0,
          "Expires/Date entry stores");
    fake_ms = 1000 + 119000;
    CHECK(wacache_lookup("/ex", &b, &n) == 0, "fresh at Expires-Date minus 1 s");
    free(b);
    fake_ms = 1000 + 121000;
    CHECK(wacache_lookup("/ex", &b, &n) == -1, "stale past Expires-Date");

    /* The documented heuristic: no freshness headers at all -> 60 s. */
    wacache_reset(); fake_ms = 1000;
    CHECK(wacache_store("/h", doc, sizeof doc, 0, 0, 0, 0, 0, 0, 0) == 0,
          "headerless entry stores");
    fake_ms = 1000 + 59000;
    CHECK(wacache_lookup("/h", &b, &n) == 0, "headerless fresh at 59 s");
    free(b);
    fake_ms = 1000 + 61000;
    CHECK(wacache_lookup("/h", &b, &n) == -1, "headerless stale at 61 s");

    /* Last-Modified heuristic: 10% of (Date - LM), capped at 300. */
    wacache_reset(); fake_ms = 1000;
    CHECK(wacache_store("/lm", doc, sizeof doc, 0, 0,
                        "Sun, 06 Nov 1994 08:49:37 GMT",
                        "Sun, 06 Nov 1994 08:16:17 GMT", 0, 0, 0) == 0,
          "Last-Modified heuristic entry stores (2000 s old -> 200 s fresh)");
    fake_ms = 1000 + 199000;
    CHECK(wacache_lookup("/lm", &b, &n) == 0, "heuristic 10% fresh at 199 s");
    free(b);
    fake_ms = 1000 + 201000;
    CHECK(wacache_lookup("/lm", &b, &n) == -1, "heuristic 10% stale at 201 s");

    /* The ceiling: max-age may ask for a day, it gets 300 s. */
    wacache_reset(); fake_ms = 1000;
    wacache_store("/cap", doc, sizeof doc, "max-age=86400", 0, 0, 0, 0, 0, 0);
    fake_ms = 1000 + 299000;
    CHECK(wacache_lookup("/cap", &b, &n) == 0, "capped fresh at 299 s");
    free(b);
    fake_ms = 1000 + 301000;
    CHECK(wacache_lookup("/cap", &b, &n) == -1, "capped stale at 301 s");

    /* Set-Cookie clamp: 60 s however long the headers asked. */
    wacache_reset(); fake_ms = 1000;
    wacache_store("/ck", doc, sizeof doc, "max-age=86400", 0, 0, 0, 0, 0, 1);
    fake_ms = 1000 + 59000;
    CHECK(wacache_lookup("/ck", &b, &n) == 0, "cookie-clamped fresh at 59 s");
    free(b);
    fake_ms = 1000 + 61000;
    CHECK(wacache_lookup("/ck", &b, &n) == -1, "cookie-clamped stale at 61 s");
}

static void test_vary_and_replace(void)
{
    unsigned char *b; int n;
    const unsigned char doc[] = "v1";
    const unsigned char doc2[] = "v2-second";

    wacache_reset(); fake_ms = 1000;
    CHECK(wacache_store("/v1", doc, sizeof doc, "max-age=60", 0, 0, 0, 0,
                        "Accept-Encoding", 0) == 0,
          "Vary: Accept-Encoding is ignorable (every request sends the same one)");
    CHECK(wacache_store("/v2", doc, sizeof doc, "max-age=60", 0, 0, 0, 0,
                        "User-Agent", 0) == -1,
          "any other Vary refuses the store");
    CHECK(wacache_store("/v3", doc, sizeof doc, "max-age=60", 0, 0, 0, 0,
                        "accept-encoding, accept-language", 0) == -1,
          "Vary naming two fields (one foreign) refuses");
    CHECK(wacache_store("/v4", doc, sizeof doc, "max-age=60", 0, 0, 0, 0,
                        "*", 0) == -1,
          "Vary: * refuses");
    CHECK(wacache_lookup("/v1", &b, &n) == 0, "the ignorable-Vary entry serves");
    free(b);

    /* Replacement keeps ONE entry with the NEWEST bytes. */
    wacache_store("/v1", doc2, sizeof doc2, "max-age=60", 0, 0, 0, 0, 0, 0);
    int ents = 0, by = 0, h = 0, rv = 0;
    wacache_stats(&ents, &by, &h, &rv);
    CHECK(ents == 1, "replace does not duplicate the entry");
    CHECK(wacache_lookup("/v1", &b, &n) == 0 && n == (int)sizeof doc2,
          "the newest bytes are what serve");
    free(b);
}

static void test_revalidation_cycle(void)
{
    unsigned char *b; int n;
    const unsigned char doc[] = "revalidated-body";

    wacache_reset(); fake_ms = 1000;
    wacache_store("/r", doc, sizeof doc, "max-age=1",
                  0, 0, 0, "\"etag-42\"", 0, 0);
    fake_ms = 5000;                        /* now stale */
    char et[256], lm[64];
    CHECK(wacache_validators("/r", et, sizeof et, lm, sizeof lm) == 0,
          "stale entry arms");
    CHECK(strcmp(et, "\"etag-42\"") == 0, "the stored ETag round-trips");
    /* The 304 arrives; the caller refreshes from ITS headers. */
    wacache_refresh("/r", "max-age=60", 0, 0);
    CHECK(wacache_lookup("/r", &b, &n) == 0 && n == (int)sizeof doc,
          "after a 304 refresh the body serves fresh again");
    free(b);
    /* A truncation check on validator copying: a 300-char etag must not
     * overflow a 256 buffer; it is truncated and the request still goes out. */
    char long_et[300];
    memset(long_et, 'x', sizeof long_et - 1);
    long_et[sizeof long_et - 1] = 0;
    wacache_reset(); fake_ms = 1000;
    wacache_store("/r2", doc, sizeof doc, "no-cache", 0, 0, 0, long_et, 0, 0);
    char out[256];
    CHECK(wacache_validators("/r2", out, sizeof out, lm, sizeof lm) == 0 &&
          strlen(out) == sizeof out - 1,
          "an over-long ETag is truncated, never overflowing");
}

static void test_eviction(void)
{
    unsigned char *b; int n;
    /* Two bodies that together OVERFLOW the cap by a KiB each way: the second
     * must evict the first by LRU. (Exactly-half bodies fit at exactly the
     * cap -- the first draft of this test used halves and proved nothing.) */
    static unsigned char half[WAC_MAX_BYTES / 2 + 1024];
    memset(half, 'a', sizeof half);
    wacache_reset(); fake_ms = 1000;
    CHECK(wacache_store("/big1", half, sizeof half, "max-age=60", 0, 0, 0, 0, 0, 0) == 0,
          "first over-half-cap body stores");
    fake_ms = 2000;
    CHECK(wacache_store("/big2", half, sizeof half, "max-age=60", 0, 0, 0, 0, 0, 0) == 0,
          "second over-half-cap body stores (evicting the first)");
    CHECK(wacache_lookup("/big1", &b, &n) == -1,
          "the LRU victim is gone");
    CHECK(wacache_lookup("/big2", &b, &n) == 0,
          "the newest big body survives");
    free(b);
    /* A body bigger than the whole cache is refused, not thrashed for. */
    static unsigned char huge[WAC_MAX_BYTES + 1];
    CHECK(wacache_store("/huge", huge, sizeof huge, "max-age=60", 0, 0, 0, 0, 0, 0) == -1,
          "an over-cap body is refused outright");
    /* Invalidate. */
    wacache_invalidate("/big2");
    CHECK(wacache_lookup("/big2", &b, &n) == -1, "invalidate drops the entry");
}

int main(void)
{
    test_dates();
    test_cc_tokens();
    test_freshness();
    test_vary_and_replace();
    test_revalidation_cycle();
    test_eviction();
    if (fails) { printf("%d FAIL\n", fails); return 1; }
    printf("wa_cache_test: all policy checks green\n");
    return 0;
}
