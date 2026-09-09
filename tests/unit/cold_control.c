/* cold_control.c -- the control for the cold-code instrument, and it is here
 * for CLAUDE.md rule 5: "a control that cannot be watched failing is worse
 * than no control".
 *
 * The instrument (tools/coldcode/) claims it can tell a function that RAN from
 * a function that DID NOT. That claim is only worth something if both halves
 * are demonstrated on the same binary, in the same run, by the same reader of
 * the same .profraw. So this TU is linked into the instrumented probe and
 * defines exactly two functions with KNOWN answers:
 *
 *   coldctl_hot_marker()        a constructor. It runs before main(), on every
 *                               invocation, unconditionally. If the report
 *                               calls this cold, the report is broken.
 *
 *   coldctl_cold_unreachable()  its ONE call site is inside
 *                               #ifdef COLDCTL_ENABLE_IMPOSSIBLE, and nothing
 *                               in this tree defines that macro. This is the
 *                               GLASS_FIELD_SLOW shape on purpose: code that
 *                               compiles, links, appears in the coverage map,
 *                               and cannot be reached by any input. If the
 *                               report calls this covered, the report is
 *                               broken in the other direction.
 *
 * `used` + `noinline` are load-bearing. Without them clang at -O2 deletes an
 * uncalled function outright, it never enters the coverage map, and its
 * ABSENCE would read as "no finding" rather than as a cold function -- which
 * is the failure this control exists to make impossible.
 */

int coldctl_hot_value;

__attribute__((constructor))
void coldctl_hot_marker(void)
{
    coldctl_hot_value = 1;
}

__attribute__((noinline, used))
int coldctl_cold_unreachable(int x)
{
    /* Deliberately several lines, so it is visible in a line-ranked report. */
    int a = x * 3;
    int b = a + 1;
    if (b > 1000) b = 1000;
    return b;
}

#ifdef COLDCTL_ENABLE_IMPOSSIBLE
/* The only caller. Nothing defines this macro -- checked by the instrument,
 * which greps the tree for it and fails if anybody ever does. */
int coldctl_impossible_caller(int x) { return coldctl_cold_unreachable(x); }
#endif

/* ------------------------------------------------------------------------
 * THE LINE-LEVEL HALF OF THE CONTROL, AND IT IS THE HALF A FUNCTION-LEVEL
 * READER PASSES WITHOUT NOTICING.
 *
 * The two functions above control a reader that answers "did this FUNCTION
 * run".  tools/coldcode/whatran.py answers a different question -- "did the
 * LINES you just changed run" -- and it can be wrong in a way the pair above
 * cannot catch: a reader that attributes the enclosing function's entry count
 * to every line in its body reports `coldctl_hot_marker` covered, reports
 * `coldctl_cold_unreachable` cold, passes both halves of the control above,
 * and still calls every never-taken branch in the tree COVERED.  That reader
 * would have reported the two singleton iterations that publish Screen and
 * Crypto as covered AND the branch bodies around them as covered, which is
 * the failure this whole instrument exists to prevent.
 *
 * So the line control is ONE function that runs on every invocation and
 * contains a branch that is never taken.  Both answers live inside the same
 * function, in the same profile record, so a reader cannot get one right by
 * being coarse.
 *
 * `volatile` is load-bearing.  Without it clang folds `one != 1` at -O2, the
 * branch never reaches the coverage map, and its ABSENCE reads as "no
 * finding" -- the same trap `used` + `noinline` closes above.
 *
 * THE MARKER COMMENTS ARE THE ANCHOR, NOT THE LINE NUMBERS.  whatran.py
 * --self-test finds these lines by grepping for COLDCTL-RAN / COLDCTL-COLD,
 * for the reason net.c:249 gives about quoting line numbers at all: "it was
 * 5483 when this was written and 5550 four hours later.  The text is the
 * anchor."  Editing anything above this block must not silently retarget the
 * control at the wrong lines -- and if it did, the control would still be
 * watched failing, because the expected answers differ.
 */
int coldctl_line_ran;
int coldctl_line_cold;

__attribute__((constructor))
void coldctl_line_marker(void)
{
    volatile int one = 1;
    coldctl_line_ran = one;                  /* COLDCTL-RAN */
    if (one != 1) {
        coldctl_line_cold = 2;               /* COLDCTL-COLD */
        coldctl_line_cold += 3;              /* COLDCTL-COLD */
        coldctl_line_cold *= 5;              /* COLDCTL-COLD */
    }
}
