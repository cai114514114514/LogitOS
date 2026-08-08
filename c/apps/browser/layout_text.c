/* CSS Text: UAX #14 line breaking, white-space processing, text-transform and
 * inline line building.  See layout_text.h for the contract and for what is
 * deliberately not here.
 *
 * The rule numbers in the comments below are UAX #14's own (revision 53,
 * Unicode 16.0.0), and the order the rules are tested in IS the order the
 * annex lists them: the algorithm is defined as "first rule that matches
 * wins", so reordering two of these tests is not a refactor, it is a different
 * algorithm.  tests/unit/csstext_test.c runs the Unicode Consortium's own
 * LineBreakTest.txt against it, all 16k cases, and prints the rule the corpus
 * blames whenever we disagree -- which is the only reason writing 40 rules from
 * a spec is a tractable job at all.
 *
 * THE NEGATIVE CONTROL.  Building with -DCSSTEXT_BREAK_ON_SPACE_ONLY replaces
 * the whole engine with the plausible wrong answer -- break after U+0020,
 * nowhere else -- which is what layout.c does today and what almost every
 * hand-rolled inline layout does.  It is indistinguishable from correct on
 * English prose.  It is catastrophic on Chinese, which has no spaces, and the
 * corpus says so out loud.  `make test-csstext-negctl` requires that build to
 * FAIL.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "layout_text.h"
#include "linebreak_data.inc"

/* ------------------------------------------------------------ utf-8 ------- */

/* Decode one code point.  Returns the byte length consumed (>= 1); an
 * ill-formed sequence yields U+FFFD and consumes one byte, which is what the
 * encoding spec requires and, more to the point, is what guarantees this loop
 * terminates on arbitrary bytes off the network. */
static int u8_next(const char *s, int len, int i, uint32_t *out)
{
    unsigned char c = (unsigned char)s[i];
    int n, k;
    uint32_t cp;
    if (c < 0x80)             { *out = c; return 1; }
    else if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
    else                      { *out = 0xFFFD; return 1; }
    if (i + n > len)          { *out = 0xFFFD; return 1; }
    for (k = 1; k < n; k++) {
        unsigned char t = (unsigned char)s[i + k];
        if ((t & 0xC0) != 0x80) { *out = 0xFFFD; return 1; }
        cp = (cp << 6) | (t & 0x3F);
    }
    *out = cp;
    return n;
}

static int u8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6));
                        out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12));
                        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---------------------------------------------------- property lookup ----- */

static int lb_raw(uint32_t cp)
{
    if (cp > 0x10FFFF) return LB_AL;
    return lb_cls_s2[(unsigned)lb_cls_s1[cp >> 8] * 256u + (cp & 0xFF)];
}

static unsigned lb_flags(uint32_t cp)
{
    if (cp > 0x10FFFF) return 0;
    return lb_flg_s2[(unsigned)lb_flg_s1[cp >> 8] * 256u + (cp & 0xFF)];
}

int ltx_class(uint32_t cp) { return lb_raw(cp); }

const char *ltx_class_name(int cls)
{
    if (cls < 0 || cls >= LB_COUNT) return "??";
    return lb_class_names[cls];
}

/* LB1's tailorable half.  SA and SG were already folded by the generator; what
 * is left is the part CSS gets to choose, so it happens here and per character
 * (a page can put `line-break: loose` on one span and not its neighbour). */
static int lb_resolve(int c, const struct ltx_lbopt *o)
{
    switch (c) {
    case LB_AI: return (o && o->ai_is_id) ? LB_ID : LB_AL;
    case LB_XX: return LB_AL;
    case LB_CJ: return (o && o->line_break == LTX_LB_LOOSE) ? LB_ID : LB_NS;
    default:    return c;
    }
}

/* ------------------------------------------------------ the engine -------- */

/* Chain = one base character plus the CM/ZWJ run glued to it by LB9.  Every
 * rule after LB10 is stated over chains, never over code points, which is why
 * building them explicitly is worth an array: it turns "ignore the combining
 * marks" from a condition on forty rules into a preprocessing step. */
struct chain {
    int      start;      /* index of the base character */
    uint32_t cp;         /* the base's code point (LB20a and LB28a name two
                          * specific characters, U+2010 and U+25CC) */
    uint8_t  cls;        /* the class every rule sees */
    uint8_t  flg;        /* Pi / Pf / EastAsian / ExtPict&Cn of the base */
    uint8_t  end_zwj;    /* the chain's LAST character is a ZWJ (LB8a) */
};

static int in_set5(int c, int a, int b, int d, int e, int f)
{ return c == a || c == b || c == d || c == e || c == f; }

/* LB15b's follower set and LB15a's leader set, spelled out once each. */
static int lb15b_follow(int c)
{
    return c == LB_SP || c == LB_GL || c == LB_WJ || c == LB_CL || c == LB_QU ||
           c == LB_CP || c == LB_EX || c == LB_IS || c == LB_SY || c == LB_BK ||
           c == LB_CR || c == LB_LF || c == LB_NL || c == LB_ZW;
}
static int lb15a_lead(int c)
{
    return c == LB_BK || c == LB_CR || c == LB_LF || c == LB_NL || c == LB_OP ||
           c == LB_QU || c == LB_GL || c == LB_SP || c == LB_ZW;
}
static int lb20a_lead(int c)
{
    return c == LB_BK || c == LB_CR || c == LB_LF || c == LB_NL || c == LB_SP ||
           c == LB_ZW || c == LB_CB || c == LB_GL;
}
static int is_hangul_jamo(int c)
{
    return c == LB_JL || c == LB_JV || c == LB_JT || c == LB_H2 || c == LB_H3;
}
/* LB28a's "(AK | [◌] | AS)". */
static int is_aksara(const struct chain *ch)
{
    return ch->cls == LB_AK || ch->cls == LB_AS || ch->cp == 0x25CC;
}
static int is_ak_or_dotted(const struct chain *ch)
{
    return ch->cls == LB_AK || ch->cp == 0x25CC;
}

/* Core.  `cls` is the per-character class already through LB1 (so the caller
 * can resolve AI/CJ differently per run); `out` gets n+1 entries; `nontail`,
 * if non-NULL, gets n+1 flags marking the positions whose decision came from a
 * NON-TAILORABLE rule (LB2..LB12).  CSS's `word-break: break-all` is allowed to
 * overrule everything else and nothing else, so that flag is exactly the line
 * between what a tailoring may touch and what it may not. */
static int lb_engine(const uint32_t *cp, const uint8_t *cls, int n,
                     unsigned char *out, unsigned char *nontail)
{
    struct chain *ch;
    int nc = 0, i, k;
    int bcls, bidx;                 /* last non-SP chain, and its index */
    int numseq = 0, numclose = 0;   /* LB25's NU (SY|IS)* and its CL/CP form  */
    int rirun = 0;                  /* LB30a's run of regional indicators     */

    for (i = 0; i <= n; i++) out[i] = LTX_BRK_PROHIBITED;
    if (nontail) for (i = 0; i <= n; i++) nontail[i] = 0;
    out[n] = LTX_BRK_MANDATORY;                        /* LB3: ! eot */
    if (nontail) nontail[n] = 1;
    if (n <= 0) return n + 1;
    out[0] = LTX_BRK_PROHIBITED;                       /* LB2: sot × */
    if (nontail) nontail[0] = 1;

    ch = (struct chain *)malloc((size_t)n * sizeof *ch);
    if (!ch) return n + 1;          /* degrade to "no break opportunities":
                                     * wrong, but never a wrong CHARACTER */

    /* LB9 + LB10.  A CM or ZWJ glues to the preceding character unless that
     * character is one of the six that cannot carry a combining sequence, in
     * which case LB10 makes it a lone AL with U+0041's other properties --
     * including East_Asian_Width=Na and Extended_Pictographic=N, which LB19a
     * and LB30b both read, so the flags must be cleared and not inherited. */
    for (i = 0; i < n; i++) {
        int c = cls[i];
        int glue = (c == LB_CM || c == LB_ZWJ) && i > 0 &&
                   !in_set5(cls[i - 1], LB_BK, LB_CR, LB_LF, LB_NL, LB_SP) &&
                   cls[i - 1] != LB_ZW;
        if (glue) {
            ch[nc - 1].end_zwj = (c == LB_ZWJ);
            continue;
        }
        ch[nc].start   = i;
        ch[nc].cp      = cp[i];
        ch[nc].end_zwj = (c == LB_ZWJ);
        if (c == LB_CM || c == LB_ZWJ) { ch[nc].cls = LB_AL; ch[nc].flg = 0; }
        else                           { ch[nc].cls = (uint8_t)c;
                                         ch[nc].flg = (uint8_t)lb_flags(cp[i]); }
        nc++;
    }

    bcls = -1; bidx = -1;
    for (k = 1; k < nc; k++) {
        const struct chain *A = &ch[k - 1], *B = &ch[k];
        int a = A->cls, b = B->cls;
        unsigned af = A->flg, bf = B->flg;
        int pos = B->start;
        int r = LTX_BRK_ALLOWED, nt = 0;
        int nx  = (k + 1 < nc) ? ch[k + 1].cls : -1;    /* one chain lookahead */
        int nx2 = (k + 2 < nc) ? ch[k + 2].cls : -1;

        /* State that describes the text BEHIND this position, advanced over
         * chain k-1 before any rule is tested.  Doing it afterwards is an
         * off-by-one that is invisible on ordinary prose and wrong on every
         * number: LB25's "NU (SY|IS)* × PO" would see the sequence ending one
         * chain too early, so "0%" would break between the digit and the sign
         * while "a.2 3" would refuse to break where it must. */
        if (a != LB_SP) { bcls = a; bidx = k - 1; }
        if (a == LB_RI) rirun++; else rirun = 0;
        {
            int prev_num = numseq;
            if (a == LB_NU)                               numseq = 1;
            else if ((a == LB_SY || a == LB_IS) && numseq) numseq = 1;
            else                                          numseq = 0;
            numclose = ((a == LB_CL || a == LB_CP) && prev_num) ? 1 : 0;
        }

        /* ---- non-tailorable (UAX #14 §6.1) ---- */
        if (a == LB_BK)                       { r = LTX_BRK_MANDATORY; nt = 1; }
        else if (a == LB_CR && b == LB_LF)    { r = LTX_BRK_PROHIBITED; nt = 1; }
        else if (a == LB_CR || a == LB_LF || a == LB_NL)
                                              { r = LTX_BRK_MANDATORY; nt = 1; }
        else if (b == LB_BK || b == LB_CR || b == LB_LF || b == LB_NL)
                                              { r = LTX_BRK_PROHIBITED; nt = 1; }
        else if (b == LB_SP || b == LB_ZW)    { r = LTX_BRK_PROHIBITED; nt = 1; }
        else if (bcls == LB_ZW)               { r = LTX_BRK_ALLOWED; nt = 1; }
        else if (A->end_zwj)                  { r = LTX_BRK_PROHIBITED; nt = 1; }
        else if (b == LB_WJ || a == LB_WJ)    { r = LTX_BRK_PROHIBITED; nt = 1; }
        else if (a == LB_GL)                  { r = LTX_BRK_PROHIBITED; nt = 1; }
        /* ---- tailorable (UAX #14 §6.2) ---- */
        else if (b == LB_GL && a != LB_SP && a != LB_BA && a != LB_HY)
                                                r = LTX_BRK_PROHIBITED;   /* 12a */
        else if (b == LB_CL || b == LB_CP || b == LB_EX || b == LB_SY)
                                                r = LTX_BRK_PROHIBITED;   /* 13  */
        else if (bcls == LB_OP)                 r = LTX_BRK_PROHIBITED;   /* 14  */
        /* LB15a: (sot|BK|CR|LF|NL|OP|QU|GL|SP|ZW) [Pi&QU] SP* × */
        else if (bcls == LB_QU && (ch[bidx].flg & LBF_PI) &&
                 (bidx == 0 || lb15a_lead(ch[bidx - 1].cls)))
                                                r = LTX_BRK_PROHIBITED;
        /* LB15b: × [Pf&QU] (SP|GL|WJ|CL|QU|CP|EX|IS|SY|BK|CR|LF|NL|ZW|eot) */
        else if (b == LB_QU && (bf & LBF_PF) && (nx < 0 || lb15b_follow(nx)))
                                                r = LTX_BRK_PROHIBITED;
        else if (a == LB_SP && b == LB_IS && nx == LB_NU)
                                                r = LTX_BRK_ALLOWED;      /* 15c */
        else if (b == LB_IS)                    r = LTX_BRK_PROHIBITED;   /* 15d */
        else if ((bcls == LB_CL || bcls == LB_CP) && b == LB_NS)
                                                r = LTX_BRK_PROHIBITED;   /* 16  */
        else if (bcls == LB_B2 && b == LB_B2)   r = LTX_BRK_PROHIBITED;   /* 17  */
        else if (a == LB_SP)                    r = LTX_BRK_ALLOWED;      /* 18  */
        else if (b == LB_QU && !(bf & LBF_PI))  r = LTX_BRK_PROHIBITED;   /* 19  */
        else if (a == LB_QU && !(af & LBF_PF))  r = LTX_BRK_PROHIBITED;
        /* LB19a: unless surrounded by East Asian characters, glue both sides */
        else if (b == LB_QU && !(af & LBF_EA))  r = LTX_BRK_PROHIBITED;
        else if (b == LB_QU && (nx < 0 || !(ch[k + 1].flg & LBF_EA)))
                                                r = LTX_BRK_PROHIBITED;
        else if (a == LB_QU && !(bf & LBF_EA))  r = LTX_BRK_PROHIBITED;
        else if (a == LB_QU && (k < 2 || !(ch[k - 2].flg & LBF_EA)))
                                                r = LTX_BRK_PROHIBITED;
        else if (b == LB_CB || a == LB_CB)      r = LTX_BRK_ALLOWED;      /* 20  */
        /* LB20a: (sot|BK|CR|LF|NL|SP|ZW|CB|GL) (HY | U+2010) × AL */
        else if ((a == LB_HY || A->cp == 0x2010) && b == LB_AL &&
                 (k < 2 || lb20a_lead(ch[k - 2].cls)))
                                                r = LTX_BRK_PROHIBITED;
        else if (b == LB_BA || b == LB_HY || b == LB_NS || a == LB_BB)
                                                r = LTX_BRK_PROHIBITED;   /* 21  */
        /* LB21a: HL (HY | [BA - EastAsian]) × [^HL] */
        else if (k >= 2 && ch[k - 2].cls == LB_HL && b != LB_HL &&
                 (a == LB_HY || (a == LB_BA && !(af & LBF_EA))))
                                                r = LTX_BRK_PROHIBITED;
        else if (a == LB_SY && b == LB_HL)      r = LTX_BRK_PROHIBITED;   /* 21b */
        else if (b == LB_IN)                    r = LTX_BRK_PROHIBITED;   /* 22  */
        else if ((a == LB_AL || a == LB_HL) && b == LB_NU)
                                                r = LTX_BRK_PROHIBITED;   /* 23  */
        else if (a == LB_NU && (b == LB_AL || b == LB_HL))
                                                r = LTX_BRK_PROHIBITED;
        else if (a == LB_PR && (b == LB_ID || b == LB_EB || b == LB_EM))
                                                r = LTX_BRK_PROHIBITED;   /* 23a */
        else if ((a == LB_ID || a == LB_EB || a == LB_EM) && b == LB_PO)
                                                r = LTX_BRK_PROHIBITED;
        else if ((a == LB_PR || a == LB_PO) && (b == LB_AL || b == LB_HL))
                                                r = LTX_BRK_PROHIBITED;   /* 24  */
        else if ((a == LB_AL || a == LB_HL) && (b == LB_PR || b == LB_PO))
                                                r = LTX_BRK_PROHIBITED;
        /* LB25, in the order the annex lists its fifteen alternatives. */
        else if (numclose && (b == LB_PO || b == LB_PR))
                                                r = LTX_BRK_PROHIBITED;
        else if (numseq && (b == LB_PO || b == LB_PR || b == LB_NU))
                                                r = LTX_BRK_PROHIBITED;
        else if ((a == LB_PO || a == LB_PR) && b == LB_OP &&
                 (nx == LB_NU || (nx == LB_IS && nx2 == LB_NU)))
                                                r = LTX_BRK_PROHIBITED;
        else if ((a == LB_PO || a == LB_PR) && b == LB_NU)
                                                r = LTX_BRK_PROHIBITED;
        else if (a == LB_HY && b == LB_NU)      r = LTX_BRK_PROHIBITED;
        else if (a == LB_IS && b == LB_NU)      r = LTX_BRK_PROHIBITED;
        else if (a == LB_JL && (b == LB_JL || b == LB_JV || b == LB_H2 ||
                                b == LB_H3))    r = LTX_BRK_PROHIBITED;   /* 26  */
        else if ((a == LB_JV || a == LB_H2) && (b == LB_JV || b == LB_JT))
                                                r = LTX_BRK_PROHIBITED;
        else if ((a == LB_JT || a == LB_H3) && b == LB_JT)
                                                r = LTX_BRK_PROHIBITED;
        else if (is_hangul_jamo(a) && b == LB_PO)
                                                r = LTX_BRK_PROHIBITED;   /* 27  */
        else if (a == LB_PR && is_hangul_jamo(b))
                                                r = LTX_BRK_PROHIBITED;
        else if ((a == LB_AL || a == LB_HL) && (b == LB_AL || b == LB_HL))
                                                r = LTX_BRK_PROHIBITED;   /* 28  */
        /* LB28a: the Brahmic orthographic syllable. */
        else if (a == LB_AP && (is_aksara(B) || b == LB_AS))
                                                r = LTX_BRK_PROHIBITED;
        else if (is_aksara(A) && (b == LB_VF || b == LB_VI))
                                                r = LTX_BRK_PROHIBITED;
        else if (k >= 2 && is_aksara(&ch[k - 2]) && a == LB_VI &&
                 is_ak_or_dotted(B))            r = LTX_BRK_PROHIBITED;
        else if (is_aksara(A) && is_aksara(B) && nx == LB_VF)
                                                r = LTX_BRK_PROHIBITED;
        else if (a == LB_IS && (b == LB_AL || b == LB_HL))
                                                r = LTX_BRK_PROHIBITED;   /* 29  */
        else if ((a == LB_AL || a == LB_HL || a == LB_NU) && b == LB_OP &&
                 !(bf & LBF_EA))                r = LTX_BRK_PROHIBITED;   /* 30  */
        else if (a == LB_CP && !(af & LBF_EA) &&
                 (b == LB_AL || b == LB_HL || b == LB_NU))
                                                r = LTX_BRK_PROHIBITED;
        else if (a == LB_RI && b == LB_RI && (rirun & 1))
                                                r = LTX_BRK_PROHIBITED;   /* 30a */
        else if (a == LB_EB && b == LB_EM)      r = LTX_BRK_PROHIBITED;   /* 30b */
        else if ((af & LBF_EXTPICT_CN) && b == LB_EM)
                                                r = LTX_BRK_PROHIBITED;
        else                                    r = LTX_BRK_ALLOWED;      /* 31  */

        out[pos] = (unsigned char)r;
        if (nontail) nontail[pos] = (unsigned char)nt;
    }

    free(ch);
    return n + 1;
}

/* ------------------------------------------- CSS tailorings on top -------- */

/* word-break / line-break / hyphens, applied AFTER the annex's own rules.
 * These are per-position because a page can change them mid-paragraph, and
 * they only ever move a decision in one direction each, so applying them as a
 * post-pass cannot reorder the annex's rule precedence. */
static void lb_tailor(const uint32_t *cp, const uint8_t *cls, int n,
                      const struct ltx_lbopt *o, const unsigned char *nontail,
                      unsigned char *out)
{
    int i;
    if (!o) return;

    if (o->hyphens_none) {
        /* SOFT HYPHEN is class BA, so LB21 gives it a break after.  `hyphens:
         * none` says it must not: the character is invisible, so a line broken
         * there shows nothing at all and reads as a missing space. */
        for (i = 0; i < n; i++)
            if (cp[i] == 0x00AD && out[i + 1] == LTX_BRK_ALLOWED)
                out[i + 1] = LTX_BRK_PROHIBITED;
    }

    if (o->line_break == LTX_LB_ANYWHERE) {
        for (i = 1; i < n; i++)
            if (out[i] == LTX_BRK_PROHIBITED &&
                !(cls[i] == LB_CM || cls[i] == LB_ZWJ) &&
                !(cls[i - 1] == LB_CR && cls[i] == LB_LF))
                out[i] = LTX_BRK_ALLOWED;
        return;
    }

    if (o->word_break == LTX_WB_BREAK_ALL) {
        /* Every prohibited position becomes an opportunity EXCEPT the ones a
         * non-tailorable rule decided.  That is not a shortcut -- §6.1 is
         * exactly the set of rules a conformant tailoring may not touch, so
         * the flag the engine already records is the correct predicate. */
        for (i = 1; i < n; i++)
            if (out[i] == LTX_BRK_PROHIBITED && !nontail[i])
                out[i] = LTX_BRK_ALLOWED;
        return;
    }

    if (o->word_break == LTX_WB_KEEP_ALL) {
        /* No break within a run of characters that are not separated by a
         * space or an explicit break character.  For Latin text this changes
         * nothing (its opportunities are all at spaces and hyphens anyway);
         * for CJK it turns off the between-ideographs breaking that IS the
         * point of the rest of this file, which is what keep-all is for. */
        for (i = 1; i < n; i++) {
            int pa;
            if (out[i] != LTX_BRK_ALLOWED) continue;
            pa = cls[i - 1];
            if (pa == LB_SP || pa == LB_BA || pa == LB_HY || pa == LB_ZW ||
                pa == LB_B2 || pa == LB_CB)
                continue;
            out[i] = LTX_BRK_PROHIBITED;
        }
    }
}

/* ------------------------------------------------- public break API ------- */

#ifdef CSSTEXT_BREAK_ON_SPACE_ONLY
/* THE NEGATIVE CONTROL.  Not a broken build -- a plausible one.  This is a
 * competent space-splitter: it breaks after a run of spaces, honours the hard
 * line break characters, and refuses to break at the start or inside a
 * multi-byte character.  Everything an English-language test would ask of it,
 * it does.  It has no idea that Chinese exists. */
static int lb_naive(const uint32_t *cp, int n, unsigned char *out)
{
    int i;
    for (i = 0; i <= n; i++) out[i] = LTX_BRK_PROHIBITED;
    out[n] = LTX_BRK_MANDATORY;
    for (i = 1; i < n; i++) {
        if (cp[i - 1] == '\n' || cp[i - 1] == 0x0B || cp[i - 1] == 0x0C ||
            cp[i - 1] == 0x2028 || cp[i - 1] == 0x2029)
            out[i] = LTX_BRK_MANDATORY;
        else if (cp[i - 1] == '\r')
            out[i] = (cp[i] == '\n') ? LTX_BRK_PROHIBITED : LTX_BRK_MANDATORY;
        else if (cp[i - 1] == 0x20 && cp[i] != 0x20)
            out[i] = LTX_BRK_ALLOWED;
    }
    return n + 1;
}
#endif

int ltx_break_cp(const uint32_t *cp, int n, const struct ltx_lbopt *opt,
                 unsigned char *out)
{
    uint8_t *cls;
    unsigned char *nt;
    int i, rc;

#ifdef CSSTEXT_BREAK_ON_SPACE_ONLY
    (void)opt;
    return lb_naive(cp, n, out);
#else
    if (n <= 0) { out[0] = LTX_BRK_MANDATORY; return 1; }
    cls = (uint8_t *)malloc((size_t)n);
    nt  = (unsigned char *)malloc((size_t)n + 1);
    if (!cls || !nt) { free(cls); free(nt);
                       for (i = 0; i <= n; i++) out[i] = LTX_BRK_PROHIBITED;
                       out[n] = LTX_BRK_MANDATORY; return n + 1; }
    for (i = 0; i < n; i++) cls[i] = (uint8_t)lb_resolve(lb_raw(cp[i]), opt);
    rc = lb_engine(cp, cls, n, out, nt);
    lb_tailor(cp, cls, n, opt, nt, out);
    free(cls); free(nt);
    return rc;
#endif
}

int ltx_break_utf8(const char *s, int len, const struct ltx_lbopt *opt,
                   unsigned char *out)
{
    uint32_t *cp;
    int *off;
    unsigned char *b;
    int n = 0, i, j;

    for (i = 0; i <= len; i++) out[i] = LTX_BRK_PROHIBITED;
    out[len] = LTX_BRK_MANDATORY;
    if (len <= 0) return len + 1;

    cp  = (uint32_t *)malloc((size_t)len * sizeof *cp);
    off = (int *)malloc(((size_t)len + 1) * sizeof *off);
    b   = (unsigned char *)malloc((size_t)len + 2);
    if (!cp || !off || !b) { free(cp); free(off); free(b); return len + 1; }

    for (i = 0; i < len; ) {
        uint32_t c;
        int adv = u8_next(s, len, i, &c);
        off[n] = i; cp[n] = c; n++; i += adv;
    }
    off[n] = len;
    ltx_break_cp(cp, n, opt, b);
    for (j = 0; j <= n; j++) out[off[j]] = b[j];
    free(cp); free(off); free(b);
    return len + 1;
}

/* ------------------------------------------ white-space processing -------- */

void ltx_white_space(int ws, int *collapse, int *wrap)
{
    int c = LTX_WSC_COLLAPSE, w = LTX_WRAP_WRAP;
    switch (ws) {
    case LTX_WS_NORMAL:       c = LTX_WSC_COLLAPSE;        w = LTX_WRAP_WRAP;   break;
    case LTX_WS_PRE:          c = LTX_WSC_PRESERVE;        w = LTX_WRAP_NOWRAP; break;
    case LTX_WS_NOWRAP:       c = LTX_WSC_COLLAPSE;        w = LTX_WRAP_NOWRAP; break;
    case LTX_WS_PRE_WRAP:     c = LTX_WSC_PRESERVE;        w = LTX_WRAP_WRAP;   break;
    case LTX_WS_PRE_LINE:     c = LTX_WSC_PRESERVE_BREAKS; w = LTX_WRAP_WRAP;   break;
    case LTX_WS_BREAK_SPACES: c = LTX_WSC_BREAK_SPACES;    w = LTX_WRAP_WRAP;   break;
    default: break;
    }
    if (collapse) *collapse = c;
    if (wrap)     *wrap     = w;
}

static int ws_is_space(uint32_t c) { return c == 0x20 || c == 0x09; }
static int ws_is_break(uint32_t c) { return c == 0x0A || c == 0x0D; }

static int is_hangul_cp(uint32_t c)
{
    return (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F) ||
           (c >= 0xA960 && c <= 0xA97F) || (c >= 0xAC00 && c <= 0xD7FF);
}
static int is_ea_wide(uint32_t c) { return (lb_flags(c) & LBF_EA) != 0; }

/* Decide the fate of a pending segment break now that the next character is
 * known.  1 = emit a space, 0 = emit nothing. */
static int segbreak_becomes_space(const struct ltx_wsstate *st, uint32_t next)
{
    if (st->last_cp == 0x200B || next == 0x200B) return 0;   /* CSS Text §4.1.2 */
    if (st->ea_segbreak && st->last_cp && next &&
        is_ea_wide(st->last_cp) && is_ea_wide(next) &&
        !is_hangul_cp(st->last_cp) && !is_hangul_cp(next))
        return 0;
    return 1;
}

int ltx_collapse(const char *in, int len, int wsc, struct ltx_wsstate *state,
                 char *out, int outmax)
{
    struct ltx_wsstate local;
    int i = 0, o = 0;

    if (!state) { memset(&local, 0, sizeof local); state = &local; }

    if (wsc == LTX_WSC_PRESERVE || wsc == LTX_WSC_BREAK_SPACES) {
        /* Nothing collapses.  CRLF and a lone CR still normalise to LF -- a
         * segment break is a break however the file spelled it, and leaving a
         * CR in the text would later be measured as a missing glyph. */
        while (i < len) {
            char c = in[i];
            if (c == '\r') {
                if (o >= outmax) return -1;
                out[o++] = '\n';
                i += (i + 1 < len && in[i + 1] == '\n') ? 2 : 1;
                state->last_cp = 0x0A; state->started = 1;
                continue;
            }
            if (o >= outmax) return -1;
            out[o++] = c;
            i++;
            state->started = 1;
        }
        if (o > 0) {
            /* keep last_cp honest for a following run */
            int k = o - 1;
            while (k > 0 && ((unsigned char)out[k] & 0xC0) == 0x80) k--;
            u8_next(out, o, k, &state->last_cp);
        }
        return o;
    }

    while (i < len) {
        uint32_t c;
        int adv = u8_next(in, len, i, &c);

        if (ws_is_break(c)) {
            if (c == 0x0D && i + 1 < len && in[i + 1] == '\n') adv = 2;
            if (wsc == LTX_WSC_PRESERVE_BREAKS) {
                /* pre-line: the break survives; the collapsible spaces on
                 * either side of it do not.  `started` goes back to 0 because
                 * what follows is the start of a line, and a collapsible space
                 * at the start of a line is removed -- the same rule that
                 * drops the leading space of the paragraph. */
                if (o >= outmax) return -1;
                out[o++] = '\n';
                state->pending_space = 0;
                state->pending_break = 0;
                state->started = 0;
                state->last_cp = 0x0A;
            } else {
                /* collapse: a segment break absorbs the spaces on both sides
                 * and its own fate waits for the next character. */
                state->pending_space = 0;
                state->pending_break = 1;
            }
            i += adv;
            continue;
        }
        if (ws_is_space(c)) {
            if (wsc == LTX_WSC_PRESERVE_SPACES) {
                if (o >= outmax) return -1;
                out[o++] = ' ';
                state->started = 1;
                state->last_cp = 0x20;
            } else if (!state->pending_break) {
                state->pending_space = 1;
            }
            i += adv;
            continue;
        }

        /* A real character.  Flush whatever white space is owed. */
        if (state->pending_break) {
            if (state->started && segbreak_becomes_space(state, c)) {
                if (o >= outmax) return -1;
                out[o++] = ' ';
            }
            state->pending_break = 0;
            state->pending_space = 0;
        } else if (state->pending_space) {
            if (state->started) {
                if (o >= outmax) return -1;
                out[o++] = ' ';
            }
            state->pending_space = 0;
        }
        if (o + adv > outmax) return -1;
        memcpy(out + o, in + i, (size_t)adv);
        o += adv;
        i += adv;
        state->started = 1;
        state->last_cp = c;
    }
    return o;
}

int ltx_trim_end(const char *s, int len, int wsc)
{
    /* `break-spaces` is the one mode where a trailing space is real: it takes
     * its width, is not hung, and carries its own break opportunity.  In every
     * other mode a collapsible trailing space hangs off the end of the line and
     * is neither measured nor aligned. */
    if (wsc == LTX_WSC_BREAK_SPACES) return len;
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    return len;
}

/* ---------------------------------------------------- text-transform ------ */

static uint32_t casemap(const struct lb_casemap *tab, int n, uint32_t cp)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (tab[m].cp == cp) return tab[m].to;
        if (tab[m].cp < cp) lo = m + 1; else hi = m - 1;
    }
    return cp;
}

static const uint32_t *fullupper(uint32_t cp)
{
    int lo = 0, hi = (int)(sizeof lb_upper_full / sizeof lb_upper_full[0]) - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (lb_upper_full[m].cp == cp) return lb_upper_full[m].to;
        if (lb_upper_full[m].cp < cp) lo = m + 1; else hi = m - 1;
    }
    return NULL;
}

#define NELEM(a) ((int)(sizeof (a) / sizeof (a)[0]))

/* `text-transform: full-size-kana`: the small kana and the small Ainu katakana
 * become their full-size counterparts.  Short enough to state, and it is the
 * one text-transform value with no Unicode property behind it -- the mapping
 * is defined by CSS itself, so it belongs here and not in the generator. */
static uint32_t fullsize_kana(uint32_t c)
{
    /* Hiragana small -> large, then katakana, then the halfwidth forms. */
    switch (c) {
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049:
        return c + 1;
    case 0x3063: return 0x3064;
    case 0x3083: case 0x3085: case 0x3087: return c + 1;
    case 0x308E: return 0x308F;
    case 0x3095: return 0x304B;
    case 0x3096: return 0x3051;
    case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9:
        return c + 1;
    case 0x30C3: return 0x30C4;
    case 0x30E3: case 0x30E5: case 0x30E7: return c + 1;
    case 0x30EE: return 0x30EF;
    case 0x30F5: return 0x30AB;
    case 0x30F6: return 0x30B1;
    case 0x31F0: return 0x30AF;  /* small ku  */
    case 0x31F1: return 0x30B7;  /* small shi */
    case 0x31F2: return 0x30B9;  /* small su  */
    case 0x31F3: return 0x30C8;  /* small to  */
    case 0x31F4: return 0x30CC;  /* small nu  */
    case 0x31F5: return 0x30CF;  /* small ha  */
    case 0x31F6: return 0x30D2;  /* small hi  */
    case 0x31F7: return 0x30D5;  /* small fu  */
    case 0x31F8: return 0x30D8;  /* small he  */
    case 0x31F9: return 0x30DB;  /* small ho  */
    case 0x31FA: return 0x30E0;  /* small mu  */
    case 0x31FB: return 0x30E9;  /* small ra  */
    case 0x31FC: return 0x30EA;  /* small ri  */
    case 0x31FD: return 0x30EB;  /* small ru  */
    case 0x31FE: return 0x30EC;  /* small re  */
    case 0x31FF: return 0x30ED;  /* small ro  */
    default: return c;
    }
}

/* CSS's own definition of a "word" boundary for `capitalize` is UAX #29's word
 * segmentation, which is a second Unicode algorithm and a second table.  What
 * is implemented instead: a letter starts a word when the character before it
 * is neither a letter/number nor an apostrophe.  That gets "hello world" and
 * "o'clock" right and differs from a real browser on constructs like "1st"
 * (we capitalise the s, Chrome does not).  Said here rather than left to be
 * discovered. */
static int transform_is_wordchar(uint32_t c)
{
    int cls = lb_raw(c);
    if (c == 0x27 || c == 0x2019) return 1;              /* apostrophes */
    return cls == LB_AL || cls == LB_HL || cls == LB_NU || cls == LB_ID ||
           cls == LB_AI || cls == LB_CM || cls == LB_AK || cls == LB_AS;
}

int ltx_text_transform(const char *in, int len, int tt, int *at_word_start,
                       char *out, int outmax)
{
    int i = 0, o = 0, ws = at_word_start ? *at_word_start : 1;

    if (tt == LTX_TT_NONE) {
        if (len > outmax) return -1;
        memcpy(out, in, (size_t)len);
        return len;
    }
    while (i < len) {
        uint32_t c, m;
        int adv = u8_next(in, len, i, &c);
        const uint32_t *full = NULL;
        uint32_t seq[3];
        int nseq = 1;

        switch (tt) {
        case LTX_TT_UPPERCASE:
            full = fullupper(c);
            if (full) { nseq = 0;
                        while (nseq < 3 && full[nseq]) { seq[nseq] = full[nseq]; nseq++; } }
            else seq[0] = casemap(lb_upper, NELEM(lb_upper), c);
            break;
        case LTX_TT_LOWERCASE:
            /* Greek final sigma: Σ at the end of a word lowercases to ς, not
             * σ.  Conditioned on position, not on language, so unlike the
             * Turkish case it is ours to get right. */
            if (c == 0x03A3) {
                int j = i + adv;
                uint32_t nx = 0;
                if (j < len) u8_next(in, len, j, &nx);
                seq[0] = (j >= len || !transform_is_wordchar(nx)) ? 0x03C2 : 0x03C3;
            } else {
                seq[0] = casemap(lb_lower, NELEM(lb_lower), c);
            }
            break;
        case LTX_TT_CAPITALIZE:
            if (ws && transform_is_wordchar(c) && c != 0x27 && c != 0x2019) {
                m = casemap(lb_title, NELEM(lb_title), c);
                if (m == c) m = casemap(lb_upper, NELEM(lb_upper), c);
                seq[0] = m;
            } else {
                seq[0] = c;
            }
            break;
        case LTX_TT_FULL_WIDTH:
            if (c == 0x20)                    seq[0] = 0x3000;
            else if (c >= 0x21 && c <= 0x7E)  seq[0] = 0xFF01 + (c - 0x21);
            else                              seq[0] = c;
            break;
        case LTX_TT_FULL_SIZE_KANA:
            seq[0] = fullsize_kana(c);
            break;
        default:
            seq[0] = c;
            break;
        }

        {
            int k;
            for (k = 0; k < nseq; k++) {
                char tmp[4];
                int n = u8_encode(seq[k], tmp);
                if (o + n > outmax) return -1;
                memcpy(out + o, tmp, (size_t)n);
                o += n;
            }
        }
        ws = !transform_is_wordchar(c);
        i += adv;
    }
    if (at_word_start) *at_word_start = ws;
    return o;
}

/* --------------------------------------------------------- measuring ------ */

int ltx_measure_run(const struct ltx_env *env, const struct ltx_style *st,
                    const char *s, int len)
{
    int w, i, nchar = 0, nspace = 0;
    if (len <= 0) return 0;
    w = env && env->measure ? env->measure(env->ctx, s, len, st) : 0;
    if (!st) return w;
    if (st->letter_spacing || st->word_spacing) {
        for (i = 0; i < len; ) {
            uint32_t c;
            i += u8_next(s, len, i, &c);
            nchar++;
            if (c == 0x20) nspace++;
        }
        w += nchar * st->letter_spacing + nspace * st->word_spacing;
    }
    return w;
}

/* --------------------------------------------------- line building -------- */

struct rspan { int start, end; };

struct build {
    const struct ltx_run *runs;
    int nrun;
    const struct ltx_env *env;
    struct rspan *span;              /* per run, byte range in `text` */
    char *text;
    int len;
    unsigned char *brk;
    struct ltx_frag *frags; int nfrag, cfrag;
    struct ltx_line *lines; int nline, cline;
};

static int run_at(const struct build *b, int pos)
{
    int r;
    for (r = 0; r < b->nrun; r++)
        if (pos >= b->span[r].start && pos < b->span[r].end) return r;
    return b->nrun ? b->nrun - 1 : 0;
}

static int line_height_of(const struct ltx_style *st)
{
    int px = st && st->font_px > 0 ? st->font_px : 16;
    if (st && st->line_px > px) return st->line_px;
    return px * 5 / 4;
}

static int tab_width(const struct build *b, const struct ltx_style *st)
{
    int t;
    if (st->tab_px) t = st->tab_size;
    else {
        int sp = b->env->measure ? b->env->measure(b->env->ctx, " ", 1, st) : 8;
        t = (st->tab_size > 0 ? st->tab_size : 8) * (sp > 0 ? sp : 8);
    }
    return t > 0 ? t : 1;
}

/* Advance the pen across [from,to), honouring preserved tabs.  `x0` is the
 * line's left edge (tab stops are measured from there, not from the block). */
static int advance_span(const struct build *b, int from, int to, int x, int x0)
{
    int p = from;
    while (p < to) {
        int r = run_at(b, p);
        const struct ltx_style *st = b->runs[r].style;
        int end = b->span[r].end < to ? b->span[r].end : to;
        int seg = p;
        while (seg < end) {
            if (b->text[seg] == '\t' &&
                (st->wsc == LTX_WSC_PRESERVE || st->wsc == LTX_WSC_BREAK_SPACES ||
                 st->wsc == LTX_WSC_PRESERVE_SPACES)) {
                int tw;
                if (seg > p) x += ltx_measure_run(b->env, st, b->text + p, seg - p);
                tw = tab_width(b, st);
                x = x0 + ((x - x0) / tw + 1) * tw;
                p = seg + 1;
            }
            seg++;
        }
        if (end > p) x += ltx_measure_run(b->env, st, b->text + p, end - p);
        p = end;
        if (end == b->span[r].end && end < to) p = end;   /* next run */
        if (p == from && to > from) p = to;               /* never stall */
    }
    return x;
}

static int push_frag(struct build *b, const struct ltx_frag *f)
{
    if (b->nfrag == b->cfrag) {
        int nc = b->cfrag ? b->cfrag * 2 : 32;
        struct ltx_frag *nf = (struct ltx_frag *)realloc(b->frags,
                                                         (size_t)nc * sizeof *nf);
        if (!nf) return -1;
        b->frags = nf; b->cfrag = nc;
    }
    b->frags[b->nfrag++] = *f;
    return 0;
}

static int push_line(struct build *b, const struct ltx_line *l)
{
    if (b->nline == b->cline) {
        int nc = b->cline ? b->cline * 2 : 16;
        struct ltx_line *nl = (struct ltx_line *)realloc(b->lines,
                                                         (size_t)nc * sizeof *nl);
        if (!nl) return -1;
        b->lines = nl; b->cline = nc;
    }
    b->lines[b->nline++] = *l;
    return 0;
}

/* Emit [from,to) as one line at y, with `x0` its left edge.  Splits at run
 * boundaries, at preserved tabs, and -- when the line will be justified -- at
 * every space, because a justified gap has to be a gap BETWEEN fragments: the
 * painter draws a fragment as one string and cannot widen a space inside it. */
static int emit_line(struct build *b, int from, int to, int y, int x0,
                     int split_spaces, int *out_w, int *out_h)
{
    int p = from, x = x0, h = 0;
    int frag0 = b->nfrag;
    while (p < to) {
        int r = run_at(b, p);
        const struct ltx_style *st = b->runs[r].style;
        int end = b->span[r].end < to ? b->span[r].end : to;
        int lh = line_height_of(st);
        int preserve = (st->wsc == LTX_WSC_PRESERVE ||
                        st->wsc == LTX_WSC_BREAK_SPACES ||
                        st->wsc == LTX_WSC_PRESERVE_SPACES);
        int q = p;
        if (lh > h) h = lh;
        while (q < end) {
            int cut = q;
            while (cut < end) {
                if (preserve && b->text[cut] == '\t') break;
                if (split_spaces && b->text[cut] == ' ' && cut > q) break;
                cut++;
                if (split_spaces && b->text[cut - 1] == ' ') break;
            }
            if (cut > q) {
                struct ltx_frag f;
                memset(&f, 0, sizeof f);
                f.run = r; f.off = q; f.len = cut - q;
                f.x = x; f.y = y; f.h = lh; f.line = b->nline;
                f.user = b->runs[r].user;
                f.w = ltx_measure_run(b->env, st, b->text + q, cut - q);
                x += f.w;
                if (push_frag(b, &f) < 0) return -1;
            }
            if (cut < end && preserve && b->text[cut] == '\t') {
                int tw = tab_width(b, st);
                x = x0 + ((x - x0) / tw + 1) * tw;
                cut++;
            }
            q = cut;
        }
        p = end;
    }
    if (h == 0) h = line_height_of(b->nrun ? b->runs[0].style : NULL);
    *out_w = x - x0;
    *out_h = h;
    (void)frag0;
    return 0;
}

/* Shift a line's fragments, and -- for justify -- spread the slack. */
static void align_line(struct build *b, struct ltx_line *ln, int avail,
                       int last, int split_spaces)
{
    const struct ltx_env *e = b->env;
    int align = last ? -1 : e->align;
    int slack = avail - ln->w;
    int i, off = 0;

    if (last) {
        switch (e->align_last) {
        case LTX_ALAST_START:   align = LTX_ALIGN_START;   break;
        case LTX_ALAST_END:     align = LTX_ALIGN_END;     break;
        case LTX_ALAST_LEFT:    align = LTX_ALIGN_LEFT;    break;
        case LTX_ALAST_RIGHT:   align = LTX_ALIGN_RIGHT;   break;
        case LTX_ALAST_CENTER:  align = LTX_ALIGN_CENTER;  break;
        case LTX_ALAST_JUSTIFY: align = LTX_ALIGN_JUSTIFY; break;
        default:
            /* text-align-last: auto.  A justified block does NOT justify its
             * last line -- stretching two words across the measure is the
             * classic tell of a broken implementation -- so it falls back to
             * the start edge; every other alignment carries over. */
            align = (e->align == LTX_ALIGN_JUSTIFY) ? LTX_ALIGN_START : e->align;
            break;
        }
    }

    if (align == LTX_ALIGN_JUSTIFY && slack > 0 && ln->nfrag > 1 &&
        split_spaces && e->justify != LTX_TJ_NONE) {
        /* Inter-word: every gap between fragments is a space that grows.
         * Integer arithmetic distributes the remainder across the leading gaps
         * rather than dropping it, so the last fragment lands exactly on the
         * right margin -- a justified line that stops one pixel short on every
         * paragraph is visible. */
        int gaps = ln->nfrag - 1;
        for (i = 1; i < ln->nfrag; i++)
            b->frags[ln->frag0 + i].x += (int)((long)slack * i / gaps);
        ln->w = avail;
        return;
    }

    switch (align) {
    case LTX_ALIGN_START:   off = e->rtl ? slack : 0; break;
    case LTX_ALIGN_END:     off = e->rtl ? 0 : slack; break;
    case LTX_ALIGN_LEFT:    off = 0; break;
    case LTX_ALIGN_RIGHT:   off = slack; break;
    case LTX_ALIGN_CENTER:  off = slack / 2; break;
    default:                off = e->rtl ? slack : 0; break;
    }
    if (off <= 0) return;
    for (i = 0; i < ln->nfrag; i++) b->frags[ln->frag0 + i].x += off;
    ln->x += off;
}

/* Largest prefix of [from,to) that fits `avail` px starting at x, cut at a
 * UTF-8 character boundary.  Returns `from` if not even one character fits,
 * which the caller must treat as "put one character on anyway" -- a line with
 * zero characters does not terminate. */
static int fit_prefix(const struct build *b, int from, int to, int x, int x0,
                      int avail)
{
    int best = from, p = from;
    while (p < to) {
        int adv = 1;
        while (p + adv < to && ((unsigned char)b->text[p + adv] & 0xC0) == 0x80)
            adv++;
        if (advance_span(b, from, p + adv, x, x0) - x0 > avail) break;
        p += adv;
        best = p;
    }
    return best;
}

int ltx_layout_runs(const struct ltx_run *runs, int nrun,
                    const struct ltx_env *env, struct ltx_layout *out)
{
    struct build b;
    struct ltx_wsstate ws;
    int r, total = 0, cap, y = 0, pos = 0, lstart = 0, i;
    int word_start = 1;
    int split_spaces;
    int hard_next = 1;              /* the next line emitted is a "first" line */

    memset(out, 0, sizeof *out);
    memset(&b, 0, sizeof b);
    b.runs = runs; b.nrun = nrun; b.env = env;
    if (nrun <= 0) return 0;

    /* --- phase 1: transform + collapse every run into one buffer. */
    for (r = 0; r < nrun; r++) total += runs[r].len;
    cap = total * 3 + 4;            /* uppercase can grow the text (ß -> SS) */
    b.text = (char *)malloc((size_t)cap);
    b.span = (struct rspan *)malloc((size_t)nrun * sizeof *b.span);
    if (!b.text || !b.span) goto fail;

    memset(&ws, 0, sizeof ws);
    for (r = 0; r < nrun; r++) {
        const struct ltx_style *st = runs[r].style;
        char *tmp = NULL;
        const char *src = runs[r].text;
        int slen = runs[r].len, n;
        b.span[r].start = b.len;
        if (st->text_transform != LTX_TT_NONE && slen > 0) {
            tmp = (char *)malloc((size_t)slen * 3 + 4);
            if (!tmp) goto fail;
            n = ltx_text_transform(src, slen, st->text_transform, &word_start,
                                   tmp, slen * 3 + 4);
            if (n < 0) { free(tmp); goto fail; }
            src = tmp; slen = n;
        }
        n = ltx_collapse(src, slen, st->wsc, &ws, b.text + b.len, cap - b.len);
        free(tmp);
        if (n < 0) goto fail;
        b.len += n;
        b.span[r].end = b.len;
    }

    /* --- phase 2: break opportunities over the WHOLE inline context.  Not per
     * run: UAX #14 is context-sensitive across at least three characters, and
     * an element boundary is not a text boundary -- "<b>12</b>.5" must not
     * break before the decimal point. */
    b.brk = (unsigned char *)malloc((size_t)b.len + 2);
    if (!b.brk) goto fail;
    {
        uint32_t *cps = NULL;
        int *offs = NULL;
        uint8_t *cls = NULL;
        unsigned char *nt = NULL, *cb = NULL;
        int n = 0, j;
        if (b.len > 0) {
            cps  = (uint32_t *)malloc((size_t)b.len * sizeof *cps);
            offs = (int *)malloc(((size_t)b.len + 1) * sizeof *offs);
            cls  = (uint8_t *)malloc((size_t)b.len);
            nt   = (unsigned char *)malloc((size_t)b.len + 2);
            cb   = (unsigned char *)malloc((size_t)b.len + 2);
            if (!cps || !offs || !cls || !nt || !cb) {
                free(cps); free(offs); free(cls); free(nt); free(cb); goto fail;
            }
            for (i = 0; i < b.len; ) {
                uint32_t c;
                int adv = u8_next(b.text, b.len, i, &c);
                offs[n] = i; cps[n] = c; n++; i += adv;
            }
            offs[n] = b.len;
            for (j = 0; j < n; j++) {
                const struct ltx_style *st = runs[run_at(&b, offs[j])].style;
                struct ltx_lbopt o;
                memset(&o, 0, sizeof o);
                o.line_break = st->line_break;
                o.word_break = st->word_break;
                o.hyphens_none = (st->hyphens == LTX_HY_NONE);
                cls[j] = (uint8_t)lb_resolve(lb_raw(cps[j]), &o);
            }
#ifdef CSSTEXT_BREAK_ON_SPACE_ONLY
            /* The negative control has to reach the LINE BUILDER too, not just
             * the break API -- otherwise the layout assertions cannot tell the
             * two implementations apart, and the control silently stops
             * controlling for the thing that matters most. */
            memset(nt, 0, (size_t)n + 1);
            lb_naive(cps, n, cb);
#else
            lb_engine(cps, cls, n, cb, nt);
#endif
            /* Tailorings are per position, so they run once per run's style
             * over that run's positions only. */
            for (r = 0; r < nrun; r++) {
                const struct ltx_style *st = runs[r].style;
                struct ltx_lbopt o;
                int j0 = 0, j1 = 0;
                memset(&o, 0, sizeof o);
                o.line_break = st->line_break;
                o.word_break = st->word_break;
                o.hyphens_none = (st->hyphens == LTX_HY_NONE);
#ifdef CSSTEXT_BREAK_ON_SPACE_ONLY
                continue;
#endif
                if (!o.line_break && !o.word_break && !o.hyphens_none) continue;
                while (j0 < n && offs[j0] < b.span[r].start) j0++;
                j1 = j0;
                while (j1 < n && offs[j1] < b.span[r].end) j1++;
                if (j1 > j0)
                    lb_tailor(cps + j0, cls + j0, j1 - j0, &o, nt + j0, cb + j0);
            }
            for (i = 0; i <= b.len; i++) b.brk[i] = LTX_BRK_PROHIBITED;
            for (j = 0; j <= n; j++) b.brk[offs[j]] = cb[j];
            /* A preserved newline is a forced break wherever it survived
             * collapsing; UAX #14 already says so (LB4/LB5) and the class
             * table agrees, but `white-space: nowrap` must not turn it off. */
            free(cps); free(offs); free(cls); free(nt); free(cb);
        } else {
            b.brk[0] = LTX_BRK_MANDATORY;
        }
    }

    /* Suppress soft wraps inside `text-wrap: nowrap` runs.  Mandatory breaks
     * survive: `white-space: nowrap` still honours a <br> and a preserved \n,
     * which is why this filters ALLOWED and not everything. */
    for (r = 0; r < nrun; r++) {
        if (runs[r].style->wrap != LTX_WRAP_NOWRAP) continue;
        for (i = b.span[r].start; i < b.span[r].end; i++)
            if (b.brk[i] == LTX_BRK_ALLOWED) b.brk[i] = LTX_BRK_PROHIBITED;
        /* the opportunity at the run's END belongs to the next run's first
         * character, so it is not ours to remove */
    }

    split_spaces = (env->align == LTX_ALIGN_JUSTIFY ||
                    env->align_last == LTX_ALAST_JUSTIFY);

    /* --- phase 3: greedy line breaking. */
    while (pos <= b.len) {
        int indent = 0, avail, x0, cand = -1, last_line;
        int j, w, h;
        struct ltx_line ln;

        if (pos == b.len && lstart == b.len && b.nline > 0) break;

        /* text-indent applies to the first line of the block, to every line
         * under `each-line`, and to every line EXCEPT the first under
         * `hanging` (the two are exclusive in the grammar but not in the
         * struct, so both are read). */
        if (env->indent_hanging) indent = hard_next ? 0 : env->indent;
        else if (env->indent_each) indent = hard_next ? env->indent : 0;
        else indent = (b.nline == 0) ? env->indent : 0;

        avail = env->avail - indent;
        if (avail < 1) avail = 1;
        x0 = indent;

        j = lstart;
        last_line = 0;

        for (;;) {
            int nxt = j + 1;
            int segw;
            while (nxt <= b.len && b.brk[nxt] == LTX_BRK_PROHIBITED) nxt++;
            if (nxt > b.len) nxt = b.len;
            if (nxt <= j) { nxt = b.len; }

            {   /* width of [lstart,nxt) with the trailing hang removed */
                int r2 = run_at(&b, nxt > lstart ? nxt - 1 : lstart);
                int t = ltx_trim_end(b.text + lstart, nxt - lstart,
                                     runs[r2].style->wsc);
                segw = advance_span(&b, lstart, lstart + t, x0, x0) - x0;
            }
            if (segw > avail && j > lstart) { pos = j; break; }   /* wrap here */
            if (segw > avail && j == lstart) {
                /* One unbreakable segment is wider than the whole measure. */
                const struct ltx_style *st = runs[run_at(&b, lstart)].style;
                int emergency = (st->overflow_wrap == LTX_OW_ANYWHERE ||
                                 st->overflow_wrap == LTX_OW_BREAK_WORD ||
                                 st->word_break == LTX_WB_BREAK_ALL ||
                                 st->word_break == LTX_WB_BREAK_WORD ||
                                 st->line_break == LTX_LB_ANYWHERE);
                if (emergency) {
                    int cut = fit_prefix(&b, lstart, nxt, x0, x0, avail);
                    if (cut <= lstart) {
                        cut = lstart + 1;
                        while (cut < nxt &&
                               ((unsigned char)b.text[cut] & 0xC0) == 0x80) cut++;
                    }
                    pos = cut;
                    break;
                }
                /* No emergency allowed: the text overflows, which is what CSS
                 * asks for.  Keep going to the next opportunity. */
            }
            cand = nxt;
            j = nxt;
            if (nxt >= b.len) { pos = b.len; last_line = 1; break; }
            if (b.brk[nxt] == LTX_BRK_MANDATORY) { pos = nxt; break; }
        }
        (void)cand;

        {   /* trim the hang for measurement and alignment, keep it in the text */
            int r2 = run_at(&b, pos > lstart ? pos - 1 : lstart);
            int t = ltx_trim_end(b.text + lstart, pos - lstart,
                                 runs[r2].style->wsc);
            memset(&ln, 0, sizeof ln);
            ln.frag0 = b.nfrag;
            if (emit_line(&b, lstart, lstart + t, y, x0, split_spaces, &w, &h) < 0)
                goto fail;
            ln.nfrag = b.nfrag - ln.frag0;
            ln.x = x0; ln.y = y; ln.w = w; ln.h = h;
            ln.hard = (unsigned char)(last_line || pos >= b.len ||
                                      b.brk[pos] == LTX_BRK_MANDATORY);
            align_line(&b, &ln, avail, ln.hard, split_spaces);
            if (push_line(&b, &ln) < 0) goto fail;
            if (ln.w + x0 > out->width) out->width = ln.w + x0;
            y += h;
        }

        hard_next = (pos < b.len && b.brk[pos] == LTX_BRK_MANDATORY);
        /* A collapsible space that ended the line does not start the next one. */
        while (pos < b.len && b.text[pos] == '\n') pos++;
        if (pos >= b.len) { lstart = pos; break; }
        {
            const struct ltx_style *st = runs[run_at(&b, pos)].style;
            if (st->wsc == LTX_WSC_COLLAPSE || st->wsc == LTX_WSC_PRESERVE_BREAKS)
                while (pos < b.len && b.text[pos] == ' ') pos++;
        }
        lstart = pos;
    }

    out->text = b.text; out->text_len = b.len;
    out->frags = b.frags; out->nfrag = b.nfrag;
    out->lines = b.lines; out->nline = b.nline;
    out->height = y;
    free(b.span); free(b.brk);
    return 0;

fail:
    free(b.text); free(b.span); free(b.brk); free(b.frags); free(b.lines);
    memset(out, 0, sizeof *out);
    return -1;
}

void ltx_layout_free(struct ltx_layout *l)
{
    if (!l) return;
    free(l->text); free(l->frags); free(l->lines);
    memset(l, 0, sizeof *l);
}
