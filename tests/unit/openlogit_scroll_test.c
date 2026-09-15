/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <limits.h>
#include "aui_scroll_motion.h"
static int checks, failures;
#define CHECK(c, label) do { ++checks; if (!(c)) { ++failures; printf("FAIL %s\n", label); } } while (0)
static int sample(struct aui_scroll_motion *s, long long delta, unsigned now)
{ return aui_scroll_sample(s, s->current, s->limit, delta, now, 180, 0, 0, 0); }
int main(void)
{
    struct aui_scroll_motion s = {0}, other = {0};
    aui_scroll_sync(&s, 0, 1000);
    CHECK(sample(&s, 48, 1000) == 0, "wheel starts at displayed offset");
    CHECK(sample(&s, 0, 1090) == 42, "scroll has a real SDK intermediate value");
    CHECK(s.running, "intermediate requests next frame");
    CHECK(sample(&s, 0, 1180) == 48 && !s.running, "endpoint ends animation deadline");
    CHECK(sample(&s, 0, 9000) == 48 && !s.running, "idle remains at endpoint");
    sample(&s, 48, 9000); sample(&s, 48, 9000); sample(&s, 48, 9000);
    CHECK(s.target == 192, "rapid wheel notches accumulate against target");
    CHECK(sample(&s, 0, 9090) == 174, "coalesced wheel still interpolates");
    CHECK(sample(&s, -144, 9090) == 174 && s.target == 48,
          "reverse re-aims continuously from displayed offset");
    CHECK(sample(&s, 0, 9180) == 64, "reverse has an intermediate value");
    CHECK(sample(&s, 0, 9270) == 48 && !s.running, "reverse lands exactly");
    sample(&s, 400, 10000);
    CHECK(sample(&s, 0, 20000) == 448 && !s.running, "missed frames finish by caller time");
    sample(&s, 48, 21000);
    CHECK(aui_scroll_sample(&s, 300, 1000, 0, 21050, 180, 0, 1, 0) == 300 && !s.running,
          "thumb drag cancels pending wheel motion immediately");
    CHECK(sample(&s, 0, 23000) == 300, "release does not resurrect old target");
    sample(&s, 48, 24000);
    CHECK(aui_scroll_sample(&s, 0, 1000, 0, 24030, 180, 0, 0, 0) == 0 && !s.running,
          "programmatic Home assignment wins over animation");
    sample(&s, 48, 25000);
    CHECK(aui_scroll_sample(&s, s.current, 1000, 0, 25040, 180, 0, 0, 1) == 48 && !s.running,
          "live reduced motion finishes current leg without wakes");
    CHECK(aui_scroll_sample(&s, 48, 1000, 48, 25100, 180, 0, 0, 1) == 96 && !s.running,
          "reduced motion applies wheel endpoint in input frame");
    sample(&s, 900, 26000);
    CHECK(aui_scroll_sample(&s, s.current, 30, 0, 26050, 180, 0, 0, 0) == 30 && !s.running,
          "resize clamps current offset and discards obsolete target");
    CHECK(aui_scroll_sample(&s, 30, 0, 48, 27000, 180, 0, 0, 0) == 0 && !s.running,
          "short content has no scroll or wake");
    aui_scroll_sync(&s, 48, 1000); sample(&s, 48, 28000);
    CHECK(aui_scroll_sample(&s, s.current, 1000, 0, 28020, 180, 1, 0, 0) == s.from && !s.running,
          "returning absent container latches displayed state");
    aui_scroll_sync(&s, 0, 1000); sample(&s, 48, UINT_MAX - 39);
    CHECK(sample(&s, 0, 50) == 42, "millisecond wrap preserves intermediate sample");
    CHECK(sample(&s, 0, 140) == 48 && !s.running, "millisecond wrap preserves completion");
    aui_scroll_sync(&s, INT_MAX - 48, INT_MAX);
    sample(&s, (long long)INT_MAX * 48, 30000);
    CHECK(sample(&s, 0, 30180) == INT_MAX, "large wheel delta saturates without integer overflow");
    sample(&s, (long long)INT_MIN * 48, 31000);
    CHECK(sample(&s, 0, 31180) == 0, "large negative wheel delta saturates at zero");
    aui_scroll_sync(&other, 500, 1000); sample(&s, 48, 32000);
    CHECK(sample(&other, 0, 32090) == 500 && !other.running, "independent containers keep separate state");
    aui_scroll_sync(&s, 0, 1000);
    CHECK(aui_scroll_sample(&s, 0, 1000, 48, 33000, 0, 0, 0, 0) == 48 && !s.running,
          "zero duration is immediate and quiescent");
    printf("OpenLogit scroll: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
