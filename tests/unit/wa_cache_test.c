/* wa_cache_test.c -- host unit test for the cross-navigation HTTP cache's
 * POLICY (the guest measures the speed; this pins the rules the speed is
 * allowed to follow).
 *
 * WHAT IS AND IS NOT HERE: the freshness rules, the validator arms, the
 * refusal rules (no-store, foreign Vary), the clamps, replacement, eviction,
 * the 304 refresh, and (2026-09-02) the cookie half of the key -- everything
 * http_cache.c decides from header values plus the request's own Cookie
 * line. NOT here: anything about connections, pools or dials; those belong
 * to the guest gate (tests/qmp/qmp_webaccel.py), which measures them on the
 * machine they run on.
 *
 * THE CLOCK IS FAKE AND THAT IS THE POINT: WAC_NOW_MS() is defined to a
 * controllable counter before http_cache.c is included, so "after 59 s" and
 * "after 61 s" are two assignments, not two sleeps -- a freshness test that
 * sleeps is a test that gets deleted the first time CI is slow.
 *
 * MOST CALLS BELOW PASS `0` FOR cookie_line: that is not a shortcut around
 * the new parameter, it IS the cookieless case -- ck_hash(0) is a fixed
 * constant (see http_cache.c), so every one of these calls keys purely on
 * url, exactly as before this parameter existed. test_cookie_key() below is
 * the one that actually exercises two different non-NULL cookie lines
 * against the same url.
 *
 * THE NEGATIVE CONTROL is -DWACACHE_OFF on THIS file's compile line: the
 * cache compiles to nothing, every lookup misses, and every assertion below
 * that expects a hit must FAIL. tests/webaccel.mk inverts that failure.
 *
 * THE SECOND CONTROL is -DWACACHE_NO_COOKIE_KEY: the cache stays on but the
 * cookie hash collapses to a constant, so the key degenerates to url-alone
 * -- test_cookie_key() must FAIL under it (two different cookie lines to the
 * same url collide into one entry), which is exactly the douyin bug this
 * key change closes. Not wired as a third tests/webaccel.mk target here
 * (the guest chl- fixture is the driven form of this control per the task
 * spec); this file's own build is proof enough that the assertion exists to
 * fail.
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
    CHECK(wacache_store("/a", 0, doc, sizeof doc, "max-age=60", 0, 0, 0, 0, 0, 0) == 0,
          "max-age entry stores");
    fake_ms = 1000 + 59000;
    CHECK(wacache_lookup("/a", 0, &b, &n) == 0, "fresh inside max-age");
    free(b);
    fake_ms = 1000 + 61000;
    CHECK(wacache_lookup("/a", 0, &b, &n) == -1, "stale past max-age");

    /* Stale is where validators arm. */
    char et[256], lm[64];
    CHECK(wacache_validators("/a", 0, et, sizeof et, lm, sizeof lm) == 0,
          "a stale entry arms its validators");
    CHECK(et[0] == 0 && lm[0] == 0, "no ETag/Last-Modified was stored");

    /* no-store: nothing stored at all. */
    wacache_reset(); fake_ms = 1000;
    CHECK(wacache_store("/ns", 0, doc, sizeof doc, "no-store", 0, 0, 0, 0, 0, 0) == -1,
          "no-store refuses the store");
    CHECK(wacache_lookup("/ns", 0, &b, &n) == -1, "and nothing is servable");

    /* no-cache: stored, but lifetime 0 -- only ever via validators. */
    wacache_reset(); fake_ms = 1000;
    CHECK(wacache_store("/nc", 0, doc, sizeof doc, "no-cache", 0, 0, 0, 0, 0, 0) == 0,
          "no-cache stores the body");
    CHECK(wacache_lookup("/nc", 0, &b, &n) == -1, "no-cache never serves fresh");
    CHECK(wacache_body("/nc", 0, &b, &n) == 0 && n == (int)sizeof doc,
          "but a 304 can still pull it");
    free(b);

    /* Expires minus Date, the no-wall-clock lifetime. */
    wacache_reset(); fake_ms = 1000;
    CHECK(wacache_store("/ex", 0, doc, sizeof doc, 0,
                        "Sun, 06 Nov 1994 08:51:37 GMT",
                        "Sun, 06 Nov 1994 08:49:37 GMT", 0, 0, 0, 0) == 0,
          "Expires/Date entry stores");
    fake_ms = 1000 + 119000;
    CHECK(wacache_lookup("/ex", 0, &b, &n) == 0, "fresh at Expires-Date minus 1 s");
    free(b);
    fake_ms = 1000 + 121000;
    CHECK(wacache_lookup("/ex", 0, &b, &n) == -1, "stale past Expires-Date");

    /* The documented heuristic: no freshness headers at all -> 60 s. */
    wacache_reset(); fake_ms = 1000;
    CHECK(wacache_store("/h", 0, doc, sizeof doc, 0, 0, 0, 0, 0, 0, 0) == 0,
          "headerless entry stores");
    fake_ms = 1000 + 59000;
    CHECK(wacache_lookup("/h", 0, &b, &n) == 0, "headerless fresh at 59 s");
    free(b);
    fake_ms = 1000 + 61000;
    CHECK(wacache_lookup("/h", 0, &b, &n) == -1, "headerless stale at 61 s");

    /* Last-Modified heuristic: 10% of (Date - LM), capped at 300. */
    wacache_reset(); fake_ms = 1000;
    CHECK(wacache_store("/lm", 0, doc, sizeof doc, 0, 0,
                        "Sun, 06 Nov 1994 08:49:37 GMT",
                        "Sun, 06 Nov 1994 08:16:17 GMT", 0, 0, 0) == 0,
          "Last-Modified heuristic entry stores (2000 s old -> 200 s fresh)");
    fake_ms = 1000 + 199000;
    CHECK(wacache_lookup("/lm", 0, &b, &n) == 0, "heuristic 10% fresh at 199 s");
    free(b);
    fake_ms = 1000 + 201000;
    CHECK(wacache_lookup("/lm", 0, &b, &n) == -1, "heuristic 10% stale at 201 s");

    /* The ceiling: max-age may ask for a day, it gets 300 s. */
    wacache_reset(); fake_ms = 1000;
    wacache_store("/cap", 0, doc, sizeof doc, "max-age=86400", 0, 0, 0, 0, 0, 0);
    fake_ms = 1000 + 299000;
    CHECK(wacache_lookup("/cap", 0, &b, &n) == 0, "capped fresh at 299 s");
    free(b);
    fake_ms = 1000 + 301000;
    CHECK(wacache_lookup("/cap", 0, &b, &n) == -1, "capped stale at 301 s");

    /* Set-Cookie clamp: 60 s however long the headers asked. */
    wacache_reset(); fake_ms = 1000;
    wacache_store("/ck", 0, doc, sizeof doc, "max-age=86400", 0, 0, 0, 0, 0, 1);
    fake_ms = 1000 + 59000;
    CHECK(wacache_lookup("/ck", 0, &b, &n) == 0, "cookie-clamped fresh at 59 s");
    free(b);
    fake_ms = 1000 + 61000;
    CHECK(wacache_lookup("/ck", 0, &b, &n) == -1, "cookie-clamped stale at 61 s");
}

static void test_vary_and_replace(void)
{
    unsigned char *b; int n;
    const unsigned char doc[] = "v1";
    const unsigned char doc2[] = "v2-second";

    wacache_reset(); fake_ms = 1000;
    CHECK(wacache_store("/v1", 0, doc, sizeof doc, "max-age=60", 0, 0, 0,
                        0, "Accept-Encoding", 0) == 0,
          "Vary: Accept-Encoding is ignorable (every request sends the same one)");
    CHECK(wacache_store("/v1b", 0, doc, sizeof doc, "max-age=60", 0, 0, 0,
                        0, "Cookie", 0) == 0,
          "Vary: Cookie is now storable -- the key already carries the "
          "exact Cookie header, a finer partition than Vary asks for");
    CHECK(wacache_store("/v2", 0, doc, sizeof doc, "max-age=60", 0, 0, 0,
                        0, "User-Agent", 0) == -1,
          "any other Vary refuses the store");
    CHECK(wacache_store("/v3", 0, doc, sizeof doc, "max-age=60", 0, 0, 0,
                        0, "accept-encoding, accept-language", 0) == -1,
          "Vary naming two fields (one foreign) refuses");
    CHECK(wacache_store("/v3b", 0, doc, sizeof doc, "max-age=60", 0, 0, 0,
                        0, "cookie, accept-language", 0) == -1,
          "Vary naming Cookie plus a foreign field still refuses -- the "
          "foreign field is the one that matters");
    CHECK(wacache_store("/v4", 0, doc, sizeof doc, "max-age=60", 0, 0, 0,
                        0, "*", 0) == -1,
          "Vary: * refuses");
    CHECK(wacache_lookup("/v1", 0, &b, &n) == 0, "the ignorable-Vary entry serves");
    free(b);
    CHECK(wacache_lookup("/v1b", 0, &b, &n) == 0, "the Vary: Cookie entry serves");
    free(b);

    /* Replacement keeps ONE entry with the NEWEST bytes. */
    wacache_store("/v1", 0, doc2, sizeof doc2, "max-age=60", 0, 0, 0, 0, 0, 0);
    int ents = 0, by = 0, h = 0, rv = 0;
    wacache_stats(&ents, &by, &h, &rv);
    CHECK(wacache_lookup("/v1", 0, &b, &n) == 0 && n == (int)sizeof doc2,
          "the newest bytes are what serve");
    free(b);
}

static void test_revalidation_cycle(void)
{
    unsigned char *b; int n;
    const unsigned char doc[] = "revalidated-body";

    wacache_reset(); fake_ms = 1000;
    wacache_store("/r", 0, doc, sizeof doc, "max-age=1",
                  0, 0, 0, "\"etag-42\"", 0, 0);
    fake_ms = 5000;                        /* now stale */
    char et[256], lm[64];
    CHECK(wacache_validators("/r", 0, et, sizeof et, lm, sizeof lm) == 0,
          "stale entry arms");
    CHECK(strcmp(et, "\"etag-42\"") == 0, "the stored ETag round-trips");
    /* The 304 arrives; the caller refreshes from ITS headers. */
    wacache_refresh("/r", 0, "max-age=60", 0, 0);
    CHECK(wacache_lookup("/r", 0, &b, &n) == 0 && n == (int)sizeof doc,
          "after a 304 refresh the body serves fresh again");
    free(b);
    /* A truncation check on validator copying: a 300-char etag must not
     * overflow a 256 buffer; it is truncated and the request still goes out. */
    char long_et[300];
    memset(long_et, 'x', sizeof long_et - 1);
    long_et[sizeof long_et - 1] = 0;
    wacache_reset(); fake_ms = 1000;
    wacache_store("/r2", 0, doc, sizeof doc, "no-cache", 0, 0, 0, long_et, 0, 0);
    char out[256];
    CHECK(wacache_validators("/r2", 0, out, sizeof out, lm, sizeof lm) == 0 &&
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
    CHECK(wacache_store("/big1", 0, half, sizeof half, "max-age=60", 0, 0, 0, 0, 0, 0) == 0,
          "first over-half-cap body stores");
    fake_ms = 2000;
    CHECK(wacache_store("/big2", 0, half, sizeof half, "max-age=60", 0, 0, 0, 0, 0, 0) == 0,
          "second over-half-cap body stores (evicting the first)");
    CHECK(wacache_lookup("/big1", 0, &b, &n) == -1,
          "the LRU victim is gone");
    CHECK(wacache_lookup("/big2", 0, &b, &n) == 0,
          "the newest big body survives");
    free(b);
    /* A body bigger than the whole cache is refused, not thrashed for. */
    static unsigned char huge[WAC_MAX_BYTES + 1];
    CHECK(wacache_store("/huge", 0, huge, sizeof huge, "max-age=60", 0, 0, 0, 0, 0, 0) == -1,
          "an over-cap body is refused outright");
    /* Invalidate. */
    wacache_invalidate("/big2");
    CHECK(wacache_lookup("/big2", 0, &b, &n) == -1, "invalidate drops the entry");
}

/* THE FIX ITSELF (2026-09-02): the douyin loop. A challenge response sets a
 * cookie and is stored under the request that had NO cookie yet; the
 * re-navigation that follows carries the cookie the challenge just set and
 * must NOT be answered from that entry -- it is a different request, and
 * under the old url-only key it was answered anyway (CLAUDE.md's "the loop,
 * measured on the guest" is this exact shape, replayed at the unit level
 * with a fake clock instead of a QEMU boot). */
static void test_cookie_key(void)
{
    unsigned char *b; int n;
    const unsigned char pageA[] = "challenge-shell";
    const unsigned char pageB[] = "real-page";

    wacache_reset(); fake_ms = 1000;
    /* Store page A under a cookieless request (the first hit -- no cookie
     * exists yet) with Set-Cookie set, exactly what douyin's shell sends. */
    CHECK(wacache_store("/chl", "", pageA, sizeof pageA, "max-age=86400",
                        0, 0, 0, 0, 0, /*had_setcookie=*/1) == 0,
          "the challenge response stores under the cookieless key");

    /* Same URL, cookieless request again: still a hit -- nothing about this
     * request changed. */
    CHECK(wacache_lookup("/chl", "", &b, &n) == 0 &&
          n == (int)sizeof pageA,
          "a second cookieless request to the same url still hits page A");
    free(b);

    /* The re-navigation: same URL, but NOW carrying the cookie the shell's
     * Set-Cookie put in the jar. THIS is the request that used to be
     * answered from page A's entry and must not be. */
    CHECK(wacache_lookup("/chl", "chl=1", &b, &n) == -1,
          "the re-navigation's Cookie header keys to a DIFFERENT entry -- "
          "a miss, not page A served again");

    /* The real fetch goes out, the server sees the cookie and answers with
     * page B, which stores under the cookie-bearing key. */
    CHECK(wacache_store("/chl", "chl=1", pageB, sizeof pageB, "max-age=86400",
                        0, 0, 0, 0, 0, 0) == 0,
          "page B stores under the cookie-bearing key");
    CHECK(wacache_lookup("/chl", "chl=1", &b, &n) == 0 &&
          n == (int)sizeof pageB,
          "the cookie-bearing request now hits page B");
    free(b);
    /* And the two entries are independent: the cookieless key still serves
     * page A -- storing B did not silently overwrite A's entry. */
    CHECK(wacache_lookup("/chl", "", &b, &n) == 0 && n == (int)sizeof pageA,
          "the cookieless entry (page A) is untouched by storing page B");
    free(b);

    /* Two distinct non-empty cookie lines to the same url are two entries,
     * not one -- the general case, not just "cookie present vs absent". */
    wacache_reset(); fake_ms = 1000;
    wacache_store("/u", "session=alice", pageA, sizeof pageA, "max-age=60",
                 0, 0, 0, 0, 0, 0);
    wacache_store("/u", "session=bob", pageB, sizeof pageB, "max-age=60",
                 0, 0, 0, 0, 0, 0);
    CHECK(wacache_lookup("/u", "session=alice", &b, &n) == 0 &&
          n == (int)sizeof pageA, "alice's session keys to alice's body");
    free(b);
    CHECK(wacache_lookup("/u", "session=bob", &b, &n) == 0 &&
          n == (int)sizeof pageB, "bob's session keys to bob's body");
    free(b);

    /* invalidate() has no cookie context and must drop BOTH variants. */
    wacache_invalidate("/u");
    CHECK(wacache_lookup("/u", "session=alice", &b, &n) == -1,
          "invalidate drops every cookie variant (alice)");
    CHECK(wacache_lookup("/u", "session=bob", &b, &n) == -1,
          "invalidate drops every cookie variant (bob)");
}

static void test_response_metadata(void)
{
    char type[128], disposition[768]; const unsigned char body[]="attachment";
    wacache_reset(); fake_ms=1000;
    CHECK(wacache_store("/export", "session=a", body, sizeof body, "max-age=60",0,0,0,0,0,0)==0,"attachment body stored");
    wacache_response_set("/export","session=a","application/pdf","attachment; filename=report.pdf");
    wacache_response_get("/export","session=a",type,sizeof type,disposition,sizeof disposition);
    CHECK(!strcmp(type,"application/pdf")&&!strcmp(disposition,"attachment; filename=report.pdf"),"cached response keeps disposition and type");
    wacache_response_get("/export","session=b",type,sizeof type,disposition,sizeof disposition);
    CHECK(!type[0]&&!disposition[0],"metadata respects cookie variant");
    wacache_store("/export","session=a",body,sizeof body,"max-age=60",0,0,0,0,0,0);
    wacache_response_get("/export","session=a",type,sizeof type,disposition,sizeof disposition);
    CHECK(!type[0]&&!disposition[0],"replaced document drops old attachment metadata");
}

int main(void)
{
    test_response_metadata();
    test_dates();
    test_cc_tokens();
    test_freshness();
    test_vary_and_replace();
    test_revalidation_cycle();
    test_eviction();
    test_cookie_key();
    if (fails) { printf("%d FAIL\n", fails); return 1; }
    printf("wa_cache_test: all policy checks green\n");
    return 0;
}
