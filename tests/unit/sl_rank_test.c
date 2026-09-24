/* Spotlight's ranking (c/apps/gui/spotlight/sl_rank.h): the ORDER results come
 * out in for a query, which is what a person sees. Each case is an ordering a
 * user would call obviously right. The control (-DSL_RANK_NEGCTL) drops the
 * prefix and word-start tiers -- ranking by "appears somewhere" alone -- and
 * must fail. */
#include <stdio.h>
#include "sl_rank.h"

static int checks, fails;
static void above(const char *q, const char *a, const char *b)
{
    int sa = sl_score(q, a), sb = sl_score(q, b);
    checks++;
    if (!(sa > sb)) { fails++; printf("FAIL \"%s\": \"%s\" (%d) should rank above \"%s\" (%d)\n", q, a, sa, b, sb); }
}
static void none(const char *q, const char *s)
{
    checks++;
    if (sl_score(q, s)) { fails++; printf("FAIL \"%s\" must not match \"%s\"\n", q, s); }
}

int main(void)
{
    above("term", "Terminal", "determinism-notes.txt");       /* prefix over substring */
    above("te", "TextEdit", "Terminal-transcripts-2026.txt"); /* shorter prefix wins */
    above("edit", "TextEdit", "credits.txt");                 /* word start over substring */
    above("set", "Settings", "reset-password.md");
    above("rep", "report.pdf", "grep");
    above("tml", "Terminal", "zzz");                          /* subsequence beats nothing */
    above("brow", "Browser", "eyebrow.png");
    above("dark", "Dark appearance", "remarks.txt");
    above("中文", "中文说明.txt", "说明中文版本的一个很长的文件名.txt");
    none("xyz", "Terminal");
    none("terminals", "Terminal");
    printf("sl-rank: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
