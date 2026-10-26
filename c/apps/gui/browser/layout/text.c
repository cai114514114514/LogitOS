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

#include "layout/text.h"
/* The UAX #14 class lookup and pair-rule engine live in the shared core (also
 * used by c/lib/text/linebreak.c).  Included, not linked: this file is on dozens
 * of host link lines and must not grow a dependency. */
#include "../../../../lib/text/linebreak_core.inc"

_Static_assert(LBC_BRK_PROHIBITED == LTX_BRK_PROHIBITED &&
               LBC_BRK_ALLOWED == LTX_BRK_ALLOWED &&
               LBC_BRK_MANDATORY == LTX_BRK_MANDATORY,
               "core and text.h break values must agree");

/* LB1's tailorable half, from the CSS options. */
static int lb_resolve(int c, const struct ltx_lbopt *o)
{
    return lb_resolve_cls(c, o && o->ai_is_id,
                          o && o->line_break == LTX_LB_LOOSE);
}

/* ------------------------------------------------------------ utf-8 ------- */


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


int ltx_class(uint32_t cp) { return lb_raw(cp); }

const char *ltx_class_name(int cls)
{
    if (cls < 0 || cls >= LB_COUNT) return "??";
    return lb_class_names[cls];
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
    struct chain *ch;
    int i, rc;

#ifdef CSSTEXT_BREAK_ON_SPACE_ONLY
    (void)opt;
    return lb_naive(cp, n, out);
#else
    if (n <= 0) { out[0] = LTX_BRK_MANDATORY; return 1; }
    cls = (uint8_t *)malloc((size_t)n);
    nt  = (unsigned char *)malloc((size_t)n + 1);
    ch  = (struct chain *)malloc((size_t)n * sizeof *ch);
    if (!cls || !nt || !ch) { free(cls); free(nt); free(ch);
                       for (i = 0; i <= n; i++) out[i] = LTX_BRK_PROHIBITED;
                       out[n] = LTX_BRK_MANDATORY; return n + 1; }
    for (i = 0; i < n; i++) cls[i] = (uint8_t)lb_resolve(lb_raw(cp[i]), opt);
    rc = lb_engine(cp, cls, n, ch, out, nt);
    lb_tailor(cp, cls, n, opt, nt, out);
    free(cls); free(nt); free(ch);
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

/* An untransformed inline still participates in capitalization boundaries;
 * resetting at each span incorrectly capitalizes the middle of a DOM word. */
void ltx_transform_state(const char *in,int len,int *at_word_start)
{
    /* Only the final scalar determines the state; do not rescan long default
     * text merely because a later sibling might request capitalization. */
    if(len>0&&at_word_start){int i=len-1;uint32_t cp;
        while(i>0&&((unsigned char)in[i]&0xc0)==0x80)i--;
        u8_next(in,len,i,&cp);*at_word_start=!transform_is_wordchar(cp);}
}
int ltx_text_transform(const char *in, int len, int tt, int *at_word_start,
                       char *out, int outmax)
{
    int i = 0, o = 0, ws = at_word_start ? *at_word_start : 1;

    if (tt == LTX_TT_NONE) {
        if (len > outmax) return -1;
        memcpy(out, in, (size_t)len);
        ltx_transform_state(in,len,at_word_start);
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

/* A nonzero letter spacing disables cross-character ligatures in the
 * current GUI contract: each scalar is drawn separately. Word-spacing alone
 * preserves shaped words. Measurement MUST use the same segments as paint;
 * measuring full prefixes then drawing isolated glyphs keeps kerning in only
 * one half of the contract, and costs O(n*n) on long preformatted lines. This
 * shared segmentation is linear; grapheme/complex-script cluster spacing
 * needs the font API to return shaped clusters and remains outside this seam. */
int ltx_spacing_next(const char *s,int len,int pos,int letter_spacing)
{
    int q=pos+1;
    if(letter_spacing){while(q<len&&((unsigned char)s[q]&0xc0)==0x80)q++;}
    else if(s[pos]!=' '){while(q<len&&s[q]!=' ')q++;}
    return q;
}
int ltx_measure_run(const struct ltx_env *env, const struct ltx_style *st,
                    const char *s, int len)
{
    if(len<=0)return 0;
    if(!st||(!st->letter_spacing&&!st->word_spacing))
        return env&&env->measure?env->measure(env->ctx,s,len,st):0;
    int w=0;
    for(int p=0;p<len;){int q=ltx_spacing_next(s,len,p,st->letter_spacing);
        if(env&&env->measure)w+=env->measure(env->ctx,s+p,q-p,st);
        w+=st->letter_spacing;if(s[p]==' ')w+=st->word_spacing;p=q;}
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
    /* Formerly clamped below-em line heights to normal, unlike CSS. */
    if (st && (st->has_line_px || st->line_px > 0)) return st->line_px;
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
 * the expansion opportunities, because a justified gap has to be a gap BETWEEN
 * fragments: the painter draws a fragment as one string and cannot widen a
 * space inside it.
 *
 * `split` is SPLIT_NONE, SPLIT_WORD (after each run of spaces, so a fragment
 * carries its own trailing space and every gap between fragments is exactly
 * one inter-word opportunity) or SPLIT_CHAR (at every break opportunity, which
 * is how a Chinese paragraph justifies at all -- it has no spaces to stretch,
 * and inter-word justification of CJK is a no-op that leaves a ragged right
 * edge on a block that asked for a flush one). */
enum { SPLIT_NONE = 0, SPLIT_WORD = 1, SPLIT_CHAR = 2 };

static int emit_line(struct build *b, int from, int to, int y, int x0,
                     int split, int *out_w, int *out_h)
{
    int p = from, x = x0, h = 0;
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
            int cut;
            /* A tab is a position, not a glyph.  It has to be consumed here,
             * BEFORE a fragment starts, or a tab at the start of one -- which
             * is every indented line of every <pre> on the web, and every line
             * after a break inside one -- ends up inside the fragment's text
             * and gets handed to the font, which draws a missing-glyph box. */
            if (preserve && b->text[q] == '\t') {
                int tw = tab_width(b, st);
                x = x0 + ((x - x0) / tw + 1) * tw;
                q++;
                continue;
            }
            cut = q;
            while (cut < end) {
                if (preserve && b->text[cut] == '\t') break;
                if (cut > q && split == SPLIT_CHAR &&
                    b->brk[cut] == LTX_BRK_ALLOWED) break;
                if (cut > q && split == SPLIT_WORD &&
                    b->text[cut - 1] == ' ' && b->text[cut] != ' ') break;
                cut++;
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
            q = cut;    /* a tab stops the fragment; the top of the loop
                         * consumes it on the next pass */
        }
        p = end;
    }
    if (h == 0) h = line_height_of(b->nrun ? b->runs[0].style : NULL);
    *out_w = x - x0;
    *out_h = h;
    return 0;
}

/* Shift a line's fragments, and -- for justify -- spread the slack. */
static void align_line(struct build *b, struct ltx_line *ln, int avail,
                       int last, int split)
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
        split != SPLIT_NONE && e->justify != LTX_TJ_NONE) {
        /* Every gap between fragments is one expansion opportunity.
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
    int justify_any;
    int word_start = 1;

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
        if(st->text_transform==LTX_TT_NONE)ltx_transform_state(src,slen,&word_start);
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
        struct chain *chn = NULL;
        int n = 0, j;
        if (b.len > 0) {
            cps  = (uint32_t *)malloc((size_t)b.len * sizeof *cps);
            offs = (int *)malloc(((size_t)b.len + 1) * sizeof *offs);
            cls  = (uint8_t *)malloc((size_t)b.len);
            nt   = (unsigned char *)malloc((size_t)b.len + 2);
            cb   = (unsigned char *)malloc((size_t)b.len + 2);
            chn  = (struct chain *)malloc((size_t)b.len * sizeof *chn);
            if (!cps || !offs || !cls || !nt || !cb || !chn) {
                free(cps); free(offs); free(cls); free(nt); free(cb); free(chn);
                goto fail;
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
            lb_engine(cps, cls, n, chn, cb, nt);
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
            free(cps); free(offs); free(cls); free(nt); free(cb); free(chn);
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

    /* `white-space: break-spaces` keeps every preserved space, gives each one
     * its own break opportunity, and does NOT hang them.  UAX #14's LB7 (× SP)
     * forbids breaking BEFORE a space, which is right for every other mode and
     * wrong for this one -- break-spaces exists precisely so that a run of
     * spaces can be split across lines instead of overflowing. */
    for (r = 0; r < nrun; r++) {
        if (runs[r].style->wsc != LTX_WSC_BREAK_SPACES) continue;
        for (i = b.span[r].start; i < b.span[r].end; i++)
            if (i > 0 && b.text[i] == ' ' && b.brk[i] == LTX_BRK_PROHIBITED)
                b.brk[i] = LTX_BRK_ALLOWED;
    }

    justify_any = (env->align == LTX_ALIGN_JUSTIFY ||
                   env->align_last == LTX_ALAST_JUSTIFY);

    /* --- phase 3: greedy line breaking. */
    while (pos <= b.len) {
        int indent = 0, avail, x0, last_line;
        int j, w, h, curx, split;
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
        curx = x0;                  /* the pen after [lstart, j) */
        j = lstart;
        last_line = 0;

        for (;;) {
            int nxt = j + 1;
            int segw, te;
            while (nxt <= b.len && b.brk[nxt] == LTX_BRK_PROHIBITED) nxt++;
            if (nxt > b.len) nxt = b.len;
            if (nxt <= j) { nxt = b.len; }

            {   /* Width of [lstart,nxt) with the trailing hang removed,
                 * measured FORWARD from the pen rather than re-measured from
                 * the start of the line: this loop visits every opportunity, so
                 * re-measuring the whole line at each one is quadratic in the
                 * paragraph -- which a 40 KB <pre> notices and a two-word test
                 * case never would. */
                int r2 = run_at(&b, nxt > lstart ? nxt - 1 : lstart);
                te = lstart + ltx_trim_end(b.text + lstart, nxt - lstart,
                                           runs[r2].style->wsc);
                if (te >= j) segw = advance_span(&b, j, te, curx, x0) - x0;
                else         segw = advance_span(&b, lstart, te, x0, x0) - x0;
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
            curx = advance_span(&b, j, nxt, curx, x0);
            j = nxt;
            if (nxt >= b.len) { pos = b.len; last_line = 1; break; }
            if (b.brk[nxt] == LTX_BRK_MANDATORY) { pos = nxt; break; }
        }

        {   /* trim the hang for measurement and alignment, keep it in the text */
            int r2 = run_at(&b, pos > lstart ? pos - 1 : lstart);
            int t = ltx_trim_end(b.text + lstart, pos - lstart,
                                 runs[r2].style->wsc);
            /* Which expansion opportunities this line has.  `text-justify:
             * auto` prefers spaces and falls back to characters when the line
             * has none, which is the CJK case and the reason the fallback
             * exists at all. */
            split = SPLIT_NONE;
            if (justify_any) {
                if (env->justify == LTX_TJ_INTER_CHARACTER) split = SPLIT_CHAR;
                else if (env->justify == LTX_TJ_NONE)       split = SPLIT_NONE;
                else {
                    int q;
                    split = SPLIT_CHAR;
                    for (q = lstart; q < lstart + t; q++)
                        if (b.text[q] == ' ') { split = SPLIT_WORD; break; }
                    if (env->justify == LTX_TJ_INTER_WORD) split = SPLIT_WORD;
                }
            }
            memset(&ln, 0, sizeof ln);
            ln.frag0 = b.nfrag;
            if (emit_line(&b, lstart, lstart + t, y, x0, split, &w, &h) < 0)
                goto fail;
            ln.nfrag = b.nfrag - ln.frag0;
            ln.x = x0; ln.y = y; ln.w = w; ln.h = h;
            ln.hard = (unsigned char)(last_line || pos >= b.len ||
                                      b.brk[pos] == LTX_BRK_MANDATORY);
            /* A line broken at a SOFT HYPHEN must show a hyphen: the character
             * itself is invisible, so without this the word just falls apart. */
            ln.hyphen = (unsigned char)(!ln.hard && pos >= 2 &&
                                        (unsigned char)b.text[pos - 2] == 0xC2 &&
                                        (unsigned char)b.text[pos - 1] == 0xAD);
            align_line(&b, &ln, avail, ln.hard, split);
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
