#ifndef _REGEX_H
#define _REGEX_H
#include <stddef.h>

/* POSIX regcomp/regexec, a real backtracking engine (recursive
 * continuation-passing over a parsed tree -- see src/regex.c), not a stub.
 *
 * WHAT IS AND ISN'T HONEST HERE, STATED PLAINLY:
 *   - BRE and ERE are both accepted (REG_EXTENDED selects which).
 *   - Character classes ([...], including [:alpha:]-style named classes),
 *     anchors (^ $), ., alternation (|), grouping (()), and the standard
 *     repetition operators (* + ? {m,n}) all work and are captured
 *     correctly ($1.. via pmatch[]).
 *   - ALTERNATION IS LEFTMOST-FIRST, NOT POSIX LEFTMOST-LONGEST. Given
 *     "a|ab" against "ab", POSIX regexec is specified to return the LONGER
 *     match ("ab"); this engine, like Perl/PCRE, returns whichever
 *     alternative is written first ("a"). Repetition (*, +, {m,n}) IS
 *     greedy-longest, which covers the overwhelming majority of what real
 *     patterns rely on -- the divergence is specifically ambiguous
 *     alternation. A program relying on POSIX's leftmost-longest tie-break
 *     between alternatives will see a different (but still POSIX-legal
 *     ERE-matching) answer.
 *   - Backreferences (\1 inside the pattern) are NOT supported: they make
 *     matching NP-hard and are a BRE/GNU extension outside POSIX ERE proper.
 *     A pattern using one fails regcomp() with REG_BADPAT rather than being
 *     silently misinterpreted.
 *   - Collating symbols ([.ch.]) and equivalence classes ([=a=]) are not
 *     implemented (this system has one locale, "C" -- see <locale.h> -- where
 *     neither can mean anything beyond the literal character anyway).
 */

typedef struct {
    int re_nsub;        /* number of capturing subexpressions */
    void *__opaque;      /* the compiled tree; owned by regcomp/regfree */
    int __ext, __icase, __nosub, __newline;
} regex_t;

typedef long regoff_t;
typedef struct { regoff_t rm_so, rm_eo; } regmatch_t;

#define REG_EXTENDED (1 << 0)
#define REG_ICASE    (1 << 1)
#define REG_NOSUB    (1 << 2)
#define REG_NEWLINE  (1 << 3)

#define REG_NOTBOL (1 << 0)
#define REG_NOTEOL (1 << 1)

enum {
    REG_NOMATCH = 1, REG_BADPAT, REG_ECOLLATE, REG_ECTYPE, REG_EESCAPE,
    REG_ESUBREG, REG_EBRACK, REG_EPAREN, REG_EBRACE, REG_BADBR, REG_ERANGE,
    REG_ESPACE, REG_BADRPT,
};

int    regcomp(regex_t *preg, const char *pattern, int cflags);
int    regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int eflags);
size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size);
void   regfree(regex_t *preg);

#endif /* _REGEX_H */
