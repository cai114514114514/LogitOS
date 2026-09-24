#ifndef LRE_H
#define LRE_H

/* lre -- the regular expressions /bin/grep and /bin/sed use.
 *
 * A Pike VM: the pattern compiles to a small program and the input is scanned
 * ONCE, left to right, with every live thread advanced in lock step -- so the
 * time is linear in the input for a given pattern, whatever the pattern is.
 * libc's regex.c (c/apps/libc/src/regex.c) backtracks recursively instead,
 * with a depth that grows with the line; a long line is a stack it does not
 * have. That is why these tools do not use it. Two engines is a rule-3 hazard
 * (CLAUDE.md: one jar, two doors) and it is recorded as such in
 * docs/superpowers/specs/2026-09-24-experience-release-design.md: the libc
 * one should become a wrapper over this one, not a second opinion.
 *
 * SEMANTICS
 *   - POSIX leftmost-LONGEST for the whole match: the earliest start wins,
 *     and among matches at that start the longest. (A backtracker finds the
 *     leftmost-FIRST, which is why `echo abcd | grep -o 'a|ab|abc'` differs
 *     between engines that disagree on this.)
 *   - Sub-matches (for sed's \1..\9) are those of the highest-priority thread
 *     that reaches the longest end -- greedy, left to right. POSIX specifies a
 *     subtler rule for sub-expressions; the whole-match rule is exact.
 *   - Bytes, as in the C locale: a UTF-8 character is several bytes to `.`.
 *   - BRE (default) or ERE (LRE_EXTENDED). BRE takes the GNU extensions
 *     \+ \? \| as well; both take \< \> \b \B \w \W \s \S.
 *   - A back-reference INSIDE a pattern (\1 in the pattern itself) is refused
 *     with an error naming it: a linear engine cannot do it, and matching it
 *     as a literal would be a silent wrong answer.
 */

#define LRE_EXTENDED 1
#define LRE_ICASE    2

#define LRE_NSUB 10                    /* \0 (whole match) .. \9 */

struct lre;

/* Compile. Returns 0, or -1 with a message in err. *out is freed by lre_free. */
int  lre_compile(struct lre **out, const char *pattern, int flags, char *err, int errlen);
void lre_free(struct lre *re);
int  lre_nsub(const struct lre *re);   /* groups in the pattern */

/* Search s[0..n) for the leftmost-longest match starting at or after `from`.
 * `notbol`: position 0 is not the start of a line (^ does not match there).
 * On a match returns 1 and fills sub[2k], sub[2k+1] with the byte offsets of
 * group k (k = 0 is the whole match; -1 where a group did not take part). */
int  lre_exec(const struct lre *re, const char *s, int n, int from, int notbol,
              int sub[2 * LRE_NSUB]);

#endif
