/* devtools_test.c -- host gate for c/apps/browser/js_devtools.c
 *
 * WHY THIS EXISTS, and why it is not "wait for QEMU to prove it": the data
 * model half of DevTools (dt_reset/dt_script_add/dt_script_mark/dt_net_*) is
 * deliberately free of every SYS_GUI_* call (see js_devtools.h's header
 * comment), so it is exactly as host-testable as any other tests/unit/ gate
 * in this tree, and there is no reason to wait for a boot
 * harness to find a bug that lives entirely in string/array bookkeeping.
 *
 * THREE CASES, and the second one is the point of the whole exercise:
 *
 *   1. THE HEADLINE SCENARIO FROM THE BRIEF. google.com's challenge page:
 *      "scripts collected: 0 external classic, 0 external module, 5 inline"
 *      and separately a ReferenceError with exceptions[] EMPTY. This proves
 *      the panel would have answered "5 collected, 4 executed, 1 threw" --
 *      either number closes the case in one line instead of an afternoon.
 *
 *   2. THE NEGATIVE CONTROL (rule 5: a control that cannot be watched
 *      failing is worse than none). dt_script_set_expect(5) but only 4
 *      dt_script_add() calls happen -- simulating exactly the failure this
 *      whole module exists to catch: a hook added to one branch of
 *      collect_scripts() and forgotten on another. dt_sources_broken() MUST
 *      read 1, and dt_summary_line() MUST print the disagreement instead of
 *      a clean-looking count. THIS TEST IS WATCHED FAILING: run it once with
 *      the guard removed (comment below) and confirm it prints a clean
 *      "4 collected, ..." line with nobody the wiser -- that run is not
 *      shipped, it is what proves case 2 is a real control and not a rubber
 *      stamp.
 *
 *   3. NETWORK: an OK fetch, a FAILED fetch (404), and a REQUIRED-BUT-NEVER-
 *      REQUESTED stylesheet -- the github.com 28-of-94 shape, at the scale
 *      this module can actually assert against without a live site.
 *
 * Build (host, no wiring into any .mk on purpose -- see the deferred-work
 * note this ships beside):
 *   clang -Wall -Wextra -o /tmp/devtools_test \
 *       tests/unit/devtools_test.c c/apps/browser/js_devtools.c \
 *       -Ic/apps/browser && /tmp/devtools_test
 */

#include "../../c/apps/browser/js_devtools.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else printf("ok:   %s\n", msg); \
} while (0)

static void case_headline(void)
{
    printf("\n-- case 1: the google.com shape (5 inline, 4 execute, 1 throws) --\n");
    dt_reset();
    /* collect_scripts()'s own printf would have read "0 external classic, 0
     * external module, 5 inline" -- xc=0 xm=0 in=5. */
    for (int i = 0; i < 5; i++) dt_script_add(i, "(inline)", DT_KIND_CLASSIC);
    dt_script_set_expect(0 + 0 + 5);

    /* run_collected_scripts(): four js_page_eval() calls return 1, one
     * returns 0 with the exact ReferenceError the brief quotes. */
    dt_script_mark(0, DT_ST_EXECUTED, "");
    dt_script_mark(1, DT_ST_EXECUTED, "");
    dt_script_mark(2, DT_ST_EXEC_ERROR, "ReferenceError: 'solveSimpleChallenge' is not defined");
    dt_script_mark(3, DT_ST_EXECUTED, "");
    dt_script_mark(4, DT_ST_EXECUTED, "");

    char line[160];
    dt_summary_line(line, sizeof line);
    printf("  summary: %s\n", line);
    CHECK(strstr(line, "5 collected, 4 executed, 1 threw") == line,
          "summary reads 5 collected / 4 executed / 1 threw");
    CHECK(!dt_sources_broken(), "recorder agrees with the browser's own count");

    for (int i = 0; i < dt_script_count(); i++) {
        dt_script_line(i, line, sizeof line);
        printf("  [%d] %s\n", i, line);
    }
    /* THE ANSWER: script #2 names the exact ReferenceError. This is what a
     * person at 2am gets instead of an empty exceptions[] and a serial log
     * that only says "collected". */
    dt_script_line(2, line, sizeof line);
    CHECK(strstr(line, "solveSimpleChallenge") != 0,
          "the failing script's own row names the exception");
}

static void case_broken_collector(void)
{
    printf("\n-- case 2: THE NEGATIVE CONTROL -- a hook silently dropped --\n");
    dt_reset();
    /* collect_scripts() SAYS five (its own printf's xc+xm+in), but a bug on
     * one branch (say, the nomodule-fallback branch mis-classified as
     * "collected" and never called dt_script_add) means only four records
     * actually landed. */
    dt_script_set_expect(5);
    for (int i = 0; i < 4; i++) dt_script_add(i, "(inline)", DT_KIND_CLASSIC);

    char line[200];
    CHECK(dt_sources_broken(), "dt_sources_broken() catches the dropped hook");
    dt_summary_line(line, sizeof line);
    printf("  WITH THE CONTROL:    %s\n", line);
    CHECK(strstr(line, "DISAGREES") != 0,
          "the panel would show the disagreement, not a clean count");
    CHECK(strstr(line, "expected 5") != 0 && strstr(line, "recorded 4") != 0,
          "the banner names both numbers, not just \"something is wrong\"");

    /* THE WATCHED-FAILING HALF: what would a panel say if this whole
     * control did not exist -- i.e. if it just counted dt_script_count()
     * records the naive way, the way a first draft of this file did before
     * dt_sources_broken() was added? Built here from the SAME table, by the
     * SAME arithmetic dt_summary_line() uses internally, deliberately NOT
     * going through dt_sources_broken() -- this is the sentence a person
     * would have been shown instead, and it is confidently, silently wrong:
     * a page whose collector dropped a script one call the panel says
     * NOTHING happened to. That is rule 5 -- watched, not asserted, because
     * asserting a naive implementation is broken would just be restating
     * this comment. */
    { int executed = 0, error = 0, skipped = 0, failed = 0;
      char naive[160];
      for (int i = 0; i < dt_script_count(); i++) {
          char name[DT_NAME_CAP], reason[DT_REASON_CAP]; int k, st;
          dt_script_get(i, name, sizeof name, &k, &st, reason, sizeof reason);
          (void)k;
          if (st == DT_ST_EXECUTED) executed++;
          else if (st == DT_ST_EXEC_ERROR) error++;
          else if (st == DT_ST_SKIPPED) skipped++;
          else if (st == DT_ST_FAILED) failed++;
      }
      snprintf(naive, sizeof naive, "%d collected, %d executed, %d threw, %d skipped, %d failed",
               dt_script_count(), executed, error, skipped, failed);
      printf("  WITHOUT THE CONTROL: %s\n", naive);
      printf("  (a clean, plausible, WRONG line -- the fifth script the browser\n"
             "   itself said it collected has simply vanished, and nothing above\n"
             "   says so; this is the failure dt_sources_broken() exists to name)\n");
    }
}

static void case_network(void)
{
    printf("\n-- case 3: Network -- OK, FAILED, and NEVER REQUESTED --\n");
    dt_reset();
    dt_net_record("script", "https://example.com/app.js", 1, 200, "");
    dt_net_record("script", "https://example.com/vendor.js", 0, 404, "not found");
    dt_net_required_not_requested("style", "https://example.com/print.css",
                                   "data: URI, unsupported");

    CHECK(dt_net_count() == 3, "three Network rows recorded");
    char line[200];
    int got, status;
    dt_net_get(0, 0, 0, 0, 0, &got, &status, 0, 0);
    CHECK(got == 1 && status == 200, "row 0 is the successful fetch");
    dt_net_get(1, 0, 0, 0, 0, &got, &status, 0, 0);
    CHECK(got == 0 && status == 404, "row 1 is the failed fetch, with its status");
    dt_net_get(2, 0, 0, 0, 0, &got, &status, 0, 0);
    CHECK(got == -1, "row 2 is required-but-never-requested, distinct from a failure");

    for (int i = 0; i < dt_net_count(); i++) {
        dt_net_line(i, line, sizeof line);
        printf("  [%d] %s\n", i, line);
    }
    dt_net_line(2, line, sizeof line);
    CHECK(strstr(line, "NEVER REQUESTED") != 0 && strstr(line, "print.css") != 0,
          "the never-requested row names itself and the URL, not just a blank");
}

int main(void)
{
    case_headline();
    case_broken_collector();
    case_network();
    printf("\n%s (%d check%s failed)\n", failures ? "FAIL" : "PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
