/* Host test for the ONE fetcher-safety property a second browsing context
 * needs before js_frame.c can exist: bfetch (c/apps/browser/bfetch.h) is
 * already safe for a same-runtime, single-threaded second context PROVIDED
 * every frame-side call passes its base EXPLICITLY and never touches
 * bfetch_set_base(). That is not a new mechanism -- js_worker.c already
 * relies on exactly this (bfetch_resolve(w->url, ref, abs, ...) then
 * bfetch_sync(abs, ...), :608/:830) -- this file is the first thing that
 * PROVES it as a property of bfetch rather than assumes it because worker.c
 * happens to do it right.
 *
 * WHAT THIS FILE DOES NOT TEST: concurrency across a real socket, HTTP/2
 * multiplexing, the connection pool. Those are browser_rt.c's own gates
 * (http1_test.c, hpool_test.c) and this file does not re-test the fake that
 * stands in for them here -- see loader_fakebfetch.c's own header. This file
 * tests exactly one thing: does interleaving a PAGE fetch (implicit base,
 * via g_base) with a FRAME fetch (explicit base) leave the page's
 * resolution untouched, and does the discipline failure mode look like what
 * the design doc predicts (silent misresolution against the WRONG origin,
 * not a crash -- which is what makes it dangerous enough to gate).
 *
 *   make test-framefetch                    this file, positive path
 *   make test-framefetch-negctl-discipline   the SAME file, linked with
 *                                            -DFRAME_FETCH_NO_DISCIPLINE, which
 *                                            makes the simulated frame call
 *                                            bfetch_start() (implicit base)
 *                                            instead of resolving explicitly
 *                                            first -- must FAIL, and fail by
 *                                            fetching the WRONG URL (the
 *                                            page's origin), not by crashing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bfetch.h"
#include "loader_fakebfetch.h"

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        g_fail++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } else { \
        printf("ok   "); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static const char *PAGE_CSS  = "PAGE-CSS-BODY";
static const char *PAGE_CSS2 = "PAGE-CSS2-BODY";
static const char *FRAME_JS  = "FRAME-JS-BODY";

/* The simulated frame's own fetch, done the SAFE way: resolve against the
 * frame's own base explicitly, then hand bfetch_sync the resulting ABSOLUTE
 * url. This is js_worker.c:608's exact shape and is what js_frame.c must
 * copy -- never bfetch_set_base(), never a bare relative ref into
 * bfetch_start()/bfetch_sync() from frame code. */
static int frame_fetch_disciplined(const char *frame_base, const char *ref,
                                    unsigned char **out, int *outlen)
    __attribute__((unused));
static int frame_fetch_disciplined(const char *frame_base, const char *ref,
                                    unsigned char **out, int *outlen)
{
    char abs[512];
    if (bfetch_resolve(frame_base, ref, abs, sizeof abs) != 0) return -1;
    return bfetch_sync(abs, out, outlen);
}

#ifdef FRAME_FETCH_NO_DISCIPLINE
/* THE VIOLATION, compiled in only for the negative control: the frame calls
 * bfetch_start() with a bare relative ref and NO base of its own. Whatever
 * g_base currently holds -- which is the PAGE's document, because that is
 * the only thing here with standing to call bfetch_set_base() -- silently
 * decides where this goes. No crash, no error: a plausible URL under the
 * WRONG origin. That silence is exactly why this is gated rather than
 * trusted to convention. */
static int frame_fetch_undisciplined(const char *frame_base, const char *ref,
                                      unsigned char **out, int *outlen)
{
    (void)frame_base;
    return bfetch_sync(ref, out, outlen);
}
#endif

int main(void)
{
    fake_site_reset();
    fake_site_add("http://page.example/style.css",  PAGE_CSS);
    fake_site_add("http://page.example/style2.css", PAGE_CSS2);
    fake_site_add("http://frame.example/sub/script.js", FRAME_JS);

    /* The page installs its own base exactly once, the way js_page.c does
     * for the top-level document -- this is the ONE legitimate call site for
     * bfetch_set_base() in the whole system. */
    bfetch_set_base("http://page.example/index.html");

    /* 1. The page's own relative fetch resolves against its own base. */
    unsigned char *body; int blen;
    int rc = bfetch_sync("style.css", &body, &blen);
    CHECK(rc == 0 && blen == (int)strlen(PAGE_CSS) &&
          !memcmp(body, PAGE_CSS, (size_t)blen),
          "page fetch #1 (style.css) resolves against the page's own base");
    CHECK(fake_site_fetched("page.example/style.css") == 1,
          "page fetch #1 landed on page.example, not elsewhere");
    free(body);

    /* 2. The frame fetches something, from a DIFFERENT origin, using its own
     * explicit base -- interleaved between two page fetches, which is the
     * shape a real event loop produces (frame script runs on a page task). */
    unsigned char *fbody; int flen;
#ifdef FRAME_FETCH_NO_DISCIPLINE
    int frc = frame_fetch_undisciplined("http://frame.example/sub/doc.html",
                                         "script.js", &fbody, &flen);
#else
    int frc = frame_fetch_disciplined("http://frame.example/sub/doc.html",
                                       "script.js", &fbody, &flen);
#endif
    CHECK(frc == 0, "frame fetch (script.js) completed");
    if (frc == 0) {
        CHECK(flen == (int)strlen(FRAME_JS) && !memcmp(fbody, FRAME_JS, (size_t)flen),
              "frame fetch returned the FRAME's body, not the page's");
        free(fbody);
    }
    CHECK(fake_site_fetched("frame.example/sub/script.js") == 1,
          "frame fetch landed on frame.example (the frame's own origin)");
    CHECK(fake_site_fetched("page.example/script.js") == 0,
          "frame fetch did NOT silently land on the page's origin");

    /* 3. THE PROPERTY THAT MATTERS: after the frame fetch, the page's OWN
     * base must be untouched -- g_base is one jar with (at most) one
     * legitimate door, and the frame must never have opened the other one. */
    rc = bfetch_sync("style2.css", &body, &blen);
    CHECK(rc == 0 && blen == (int)strlen(PAGE_CSS2) &&
          !memcmp(body, PAGE_CSS2, (size_t)blen),
          "page fetch #2 (style2.css), AFTER the frame fetch, still resolves "
          "against the page's own base");
    CHECK(fake_site_fetched("page.example/style2.css") == 1,
          "page fetch #2 landed on page.example -- g_base was not perturbed "
          "by the frame's fetch");
    free(body);

    printf("\n%d failure(s), %d requests logged, %d dials\n",
           g_fail, fake_site_requests(), fake_site_dials());
    return g_fail ? 1 : 0;
}
