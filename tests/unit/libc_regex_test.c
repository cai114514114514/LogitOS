/* regex.h case list, diffed against glibc the same way libc_fnmatch_test.c
 * is. Cases are chosen to AVOID ambiguous alternation (e.g. "a|ab" against
 * "ab") on purpose: <regex.h> documents that this engine is leftmost-FIRST
 * for alternation rather than POSIX leftmost-LONGEST, so a case where those
 * two rules disagree would fail this diff by design, not by bug. Repetition
 * (*, +, {m,n}) is greedy-longest in both, so it is fair game throughout. */
#include <regex.h>
#include <stdio.h>
#include <string.h>

static void run(const char *pat, const char *str, int cflags)
{
    regex_t re;
    int rc = regcomp(&re, pat, cflags);
    if (rc != 0) { printf("%s|%s -> COMPILE_ERR %d\n", pat, str, rc); return; }
    regmatch_t m[6];
    int found = regexec(&re, str, 6, m, 0);
    if (found != 0) { printf("%s|%s -> NOMATCH\n", pat, str); regfree(&re); return; }
    printf("%s|%s -> [%ld,%ld)", pat, str, (long)m[0].rm_so, (long)m[0].rm_eo);
    for (int i = 1; i < 6 && i <= re.re_nsub; i++) {
        if (m[i].rm_so < 0) printf(" g%d=-", i);
        else printf(" g%d=[%ld,%ld)", i, (long)m[i].rm_so, (long)m[i].rm_eo);
    }
    printf("\n");
    regfree(&re);
}

int main(void)
{
    int E = REG_EXTENDED;
    run("abc", "xxabcxx", E);
    run("^abc", "abcxx", E);
    run("^abc", "xabc", E);
    run("abc$", "xxabc", E);
    run("a.c", "abc", E);
    run("a.c", "axc", E);
    run("a*", "aaaa", E);
    run("a*", "", E);
    run("a+", "aaab", E);
    run("a+", "b", E);
    run("a?b", "b", E);
    run("a?b", "ab", E);
    run("a{2,4}", "aaaaa", E);
    run("a{2,4}", "a", E);
    run("a{3}", "aaaa", E);
    run("[0-9]+", "abc123def", E);
    run("[^0-9]+", "abc123def", E);
    run("[[:alpha:]]+", "abc123", E);
    run("(ab)+", "ababab", E);
    run("(a)(b)(c)", "abc", E);
    run("[a-z]+@[a-z]+\\.[a-z]+", "contact: foo@bar.com here", E);
    run("^[[:space:]]*$", "   ", E);
    run("^[[:space:]]*$", "  x ", E);
    run("colou?r", "color", E);
    run("colou?r", "colour", E);
    run("(foo|bar)baz", "foobaz", E);      /* unambiguous: only one alt can match here */
    run("(foo|bar)baz", "barbaz", E);
    run("x.*y", "xaaay bbb y", E);         /* greedy: should reach the LAST y */
    run("^$", "", E);
    run("^$", "x", E);

    /* BRE mode: literal ( ) { } + ? | unless escaped */
    run("a(b)c", "a(b)c", 0);
    run("a\\(b\\)c", "abc", 0);
    run("a*", "aaa", 0);
    run("*a", "*a", 0);                    /* leading '*' is literal in BRE */

    return 0;
}
