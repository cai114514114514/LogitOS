/* fnmatch() case list, compiled twice by tests/libc.mk: once against our own
 * <fnmatch.h>/fnmatch.c (freestanding-ish host build, clang's resource-dir
 * headers only -- no glibc), once as a completely normal host program
 * against glibc's <fnmatch.h>. Byte-identical stdout from both runs is the
 * proof; tests/libc.mk diffs them. Named FNM_* macros are used throughout
 * rather than raw integers on purpose: each build resolves them against its
 * OWN header, so the comparison is of matching BEHAVIOUR, not of two
 * implementations happening to assign the same bit to PATHNAME. */
#include <fnmatch.h>
#include <stdio.h>

struct Case { const char *pat, *str; int flags; };

static const struct Case cases[] = {
    { "*", "", 0 }, { "*", "a", 0 }, { "*", "abc", 0 },
    { "?", "", 0 }, { "?", "a", 0 }, { "?", "ab", 0 },
    { "a?c", "abc", 0 }, { "a?c", "ac", 0 }, { "a?c", "abbc", 0 },
    { "[abc]", "a", 0 }, { "[abc]", "d", 0 }, { "[^abc]", "d", 0 }, { "[^abc]", "a", 0 },
    { "[a-z]", "m", 0 }, { "[a-z]", "M", 0 }, { "[a-zA-Z]", "M", 0 },
    { "[]a]", "]", 0 }, { "[]a]", "a", 0 }, { "[!]a]", "b", 0 },
    { "[[:digit:]]", "5", 0 }, { "[[:digit:]]", "x", 0 }, { "[[:alpha:]]*", "hello", 0 },
    { "*.c", "foo.c", 0 }, { "*.c", "foo.h", 0 }, { "*.c", ".c", 0 },
    { "a*b*c", "aXbYc", 0 }, { "a*b*c", "abc", 0 }, { "a*b*c", "ac", 0 },
    { "*/*.c", "src/foo.c", FNM_PATHNAME }, { "*/*.c", "src/sub/foo.c", FNM_PATHNAME },
    { "*", "src/foo.c", FNM_PATHNAME }, { "src/*", "src/foo.c", FNM_PATHNAME },
    { "src?foo.c", "src/foo.c", FNM_PATHNAME },
    { ".*", ".hidden", 0 }, { "*", ".hidden", FNM_PERIOD }, { "*", ".hidden", 0 },
    { "a/.b", "a/.b", FNM_PATHNAME | FNM_PERIOD }, { "a/*", "a/.b", FNM_PATHNAME | FNM_PERIOD },
    { "ABC", "abc", FNM_CASEFOLD }, { "ABC", "abc", 0 },
    { "a\\*b", "a*b", 0 }, { "a\\*b", "axb", 0 }, { "a\\*b", "a*b", FNM_NOESCAPE },
    { "[a-c-e]", "d", 0 }, { "", "", 0 }, { "", "x", 0 },
    /* An unterminated bracket expression ("a[b-" with no closing ']') is
     * UNSPECIFIED by POSIX -- glibc and this engine legitimately disagree
     * here (glibc: no match; ours: '[' falls back to literal), so it is
     * deliberately not a case in this diff (see the malformed-bracket note
     * in fnmatch.c's match_bracket()). */
    { "**/*.txt", "a/b/c.txt", FNM_PATHNAME },
    { "foo", "foo", 0 }, { "foo", "foobar", 0 }, { "foo*", "foobar", 0 },
    { "[[:upper:]]*", "Hello", 0 }, { "[[:upper:]]*", "hello", 0 },
};

int main(void)
{
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        int r = fnmatch(cases[i].pat, cases[i].str, cases[i].flags);
        printf("%zu\t%d\n", i, r == 0 ? 0 : 1);
    }
    return 0;
}
