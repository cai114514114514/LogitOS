#ifndef SL_RANK_H
#define SL_RANK_H

/* How well a Spotlight result's NAME matches what was typed. Pure and
 * header-only so make test-spotlight-rank can drive it on the host.
 *
 * Four tiers, and the tier decides before anything else does:
 *
 *   PREFIX      "term"  -> "Terminal"                 the name starts with it
 *   WORD START  "edit"  -> "TextEdit", "my-notes.txt" a word inside starts with it
 *                          (after a space - _ . / or a lower->Upper case step)
 *   SUBSTRING   "min"   -> "Terminal"                 it appears anywhere
 *   SUBSEQUENCE "tml"   -> "Terminal"                 its letters appear in order
 *
 * Within a tier a SHORTER name wins (typing "te" means TextEdit, not a
 * forty-character file that happens to begin "te"), then an earlier position.
 * ASCII letters compare case-insensitively; any other byte -- including every
 * byte of a CJK name -- must match exactly, which is what a substring search
 * of UTF-8 wants. 0 = no match. */

#define SL_TIER 100000

static inline int sl_lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static inline int sl_word_start(const char *s, int i)
{
    if (i == 0) return 1;
    int p = (unsigned char)s[i - 1], c = (unsigned char)s[i];
    if (p == ' ' || p == '-' || p == '_' || p == '.' || p == '/') return 1;
    return (p >= 'a' && p <= 'z') && (c >= 'A' && c <= 'Z');      /* TextEdit */
}

static inline int sl_at(const char *s, int i, const char *q)
{
    for (int k = 0; q[k]; k++) {
        if (!s[i + k]) return 0;
        if (sl_lc((unsigned char)s[i + k]) != sl_lc((unsigned char)q[k])) return 0;
    }
    return 1;
}

static inline int sl_score(const char *q, const char *s)
{
    int n = 0, m = 0;
    while (s[n]) n++;
    while (q[m]) m++;
    if (!m) return 1;
    if (m > n) return 0;
    int len_pen = n < 999 ? n : 999;
#ifndef SL_RANK_NEGCTL
    if (sl_at(s, 0, q)) return 4 * SL_TIER - len_pen;
    for (int i = 1; i + m <= n; i++)
        if (sl_word_start(s, i) && sl_at(s, i, q)) return 3 * SL_TIER - len_pen * 4 - i;
#endif
    for (int i = 0; i + m <= n; i++)
        if (sl_at(s, i, q)) return 2 * SL_TIER - len_pen * 4 - i;
    /* subsequence: every query byte in order; gaps cost */
    int i = 0, k = 0, gaps = 0, last = -1;
    for (; i < n && k < m; i++)
        if (sl_lc((unsigned char)s[i]) == sl_lc((unsigned char)q[k])) {
            if (last >= 0) gaps += i - last - 1;
            last = i; k++;
        }
    if (k < m) return 0;
    int sc = SL_TIER - gaps * 8 - len_pen;
    return sc > 0 ? sc : 1;
}

#endif
