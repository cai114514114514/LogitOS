/* Script property lookup, run segmentation and Arabic cursive joining.
 *
 * See script.h for the contract. Three separable things live here because they
 * are the three questions that can be answered without opening a font:
 *
 *   script_of / script_joining_type   two-stage trie reads, exactly like
 *                                     bidi_class() next door
 *   script_runs                       the segmentation a shaper needs: one
 *                                     script, one direction, one font per run
 *   script_arabic_joining             the isol/fina/medi/init state machine
 *
 * No allocation, no globals, no libc.
 */

#include "script.h"
#include "fontrd.h"

#include "script_data.inc"

/* --------------------------------------------------------------- lookups -- */

int script_of(uint32_t cp)
{
    if (cp > 0x10FFFF) return SC_UNKNOWN;
    return uscript_s2[(unsigned)uscript_s1[cp >> 8] * 256u + (cp & 255u)];
}

int script_joining_type(uint32_t cp)
{
    if (cp > 0x10FFFF) return JOIN_U;
    return ujoin_s2[(unsigned)ujoin_s1[cp >> 8] * 256u + (cp & 255u)];
}

int script_is_rtl(int script)
{
    switch (script) {
    case SC_ARABIC: case SC_HEBREW: case SC_SYRIAC:
    case SC_THAANA: case SC_NKO:
        return 1;
    default:
        return 0;
    }
}

int script_is_cursive(int script)
{
    /* The scripts whose glyphs change shape with their neighbours and whose
     * fonts carry init/medi/fina/isol. Mongolian is listed because HarfBuzz
     * routes it through the same shaper; we have no Mongolian font to check it
     * against, so treat that as untested rather than supported. */
    switch (script) {
    case SC_ARABIC: case SC_SYRIAC: case SC_NKO: case SC_MONGOLIAN:
        return 1;
    default:
        return 0;
    }
}

uint32_t script_ot_tag(int script)
{
    switch (script) {
    case SC_LATIN:      return FONT_TAG('l','a','t','n');
    case SC_GREEK:      return FONT_TAG('g','r','e','k');
    case SC_CYRILLIC:   return FONT_TAG('c','y','r','l');
    case SC_ARABIC:     return FONT_TAG('a','r','a','b');
    case SC_HEBREW:     return FONT_TAG('h','e','b','r');
    case SC_SYRIAC:     return FONT_TAG('s','y','r','c');
    case SC_THAANA:     return FONT_TAG('t','h','a','a');
    case SC_NKO:        return FONT_TAG('n','k','o',' ');
    case SC_DEVANAGARI: return FONT_TAG('d','e','v','a');
    case SC_BENGALI:    return FONT_TAG('b','e','n','g');
    case SC_GURMUKHI:   return FONT_TAG('g','u','r','u');
    case SC_GUJARATI:   return FONT_TAG('g','u','j','r');
    case SC_ORIYA:      return FONT_TAG('o','r','y','a');
    case SC_TAMIL:      return FONT_TAG('t','a','m','l');
    case SC_TELUGU:     return FONT_TAG('t','e','l','u');
    case SC_KANNADA:    return FONT_TAG('k','n','d','a');
    case SC_MALAYALAM:  return FONT_TAG('m','l','y','m');
    case SC_SINHALA:    return FONT_TAG('s','i','n','h');
    case SC_THAI:       return FONT_TAG('t','h','a','i');
    case SC_LAO:        return FONT_TAG('l','a','o',' ');
    case SC_TIBETAN:    return FONT_TAG('t','i','b','t');
    case SC_MYANMAR:    return FONT_TAG('m','y','m','r');
    case SC_KHMER:      return FONT_TAG('k','h','m','r');
    case SC_HANGUL:     return FONT_TAG('h','a','n','g');
    case SC_HAN:        return FONT_TAG('h','a','n','i');
    case SC_HIRAGANA:
    case SC_KATAKANA:   return FONT_TAG('k','a','n','a');
    case SC_ARMENIAN:   return FONT_TAG('a','r','m','n');
    case SC_GEORGIAN:   return FONT_TAG('g','e','o','r');
    case SC_ETHIOPIC:   return FONT_TAG('e','t','h','i');
    case SC_MONGOLIAN:  return FONT_TAG('m','o','n','g');
    default:            return FONT_TAG('D','F','L','T');
    }
}

/* ------------------------------------------------------------------ runs -- */

static int real_script(int sc)
{
    return sc != SC_COMMON && sc != SC_INHERITED && sc != SC_UNKNOWN;
}

int script_runs(const uint32_t *cps, int n, const uint8_t *levels,
                int (*fontsel)(uint32_t cp, void *ctx), void *ctx,
                struct text_run *out, int cap)
{
    if (n <= 0) return 0;

    int nruns = 0;
    int start = 0;
    int sc = script_of(cps[0]);
    int lvl = levels ? levels[0] : 0;
    int fnt = fontsel ? fontsel(cps[0], ctx) : 0;
    if (!real_script(sc)) sc = SC_COMMON;   /* provisional; adopted below */

    for (int i = 1; i <= n; i++) {
        int brk = 0, csc = SC_COMMON, clvl = lvl, cfnt = fnt;

        if (i < n) {
            csc  = script_of(cps[i]);
            clvl = levels ? levels[i] : 0;
            cfnt = fontsel ? fontsel(cps[i], ctx) : 0;

            if (clvl != lvl || cfnt != fnt) {
                brk = 1;
            } else if (!real_script(csc)) {
                brk = 0;                     /* Common/Inherited extends */
            } else if (sc == SC_COMMON) {
                sc = csc;                    /* the run adopts its first real script */
            } else if (csc != sc) {
                brk = 1;
            }
        } else {
            brk = 1;                          /* end of text closes the run */
        }

        if (!brk) continue;

        if (nruns < cap) {
            out[nruns].start  = start;
            out[nruns].len    = i - start;
            out[nruns].script = sc;
            out[nruns].level  = lvl;
            /* Direction comes from the bidi level when there is one, because
             * bidi already decided it; without levels, from the script. */
            out[nruns].rtl    = levels ? (lvl & 1) : script_is_rtl(sc);
            out[nruns].font   = fnt;
        }
        nruns++;

        start = i;
        sc = real_script(csc) ? csc : SC_COMMON;
        lvl = clvl;
        fnt = cfnt;
    }
    return nruns;
}

/* --------------------------------------------------------- Arabic joining -- */

/* The joining classes the state machine indexes by. Joining_Type C (join
 * causing) behaves exactly as D, so it collapses; T (transparent) is handled
 * before the table is consulted. The two Syriac joining GROUPS need their own
 * columns because Alaph and Dalath/Rish take fin2/fin3/med2 rather than the
 * ordinary forms. */
enum { JC_U = 0, JC_L, JC_R, JC_D, JC_ALAPH, JC_DALATH_RISH, JC_NCLASS,
       JC_TRANSPARENT };

/* Syriac Alaph is U+0710; the Dalath/Rish joining group is these five. There
 * is no UCD file of joining groups in the tables we generate, and five literals
 * are cheaper and clearer than a sixth trie. */
static int syriac_group(uint32_t cp)
{
    if (cp == 0x0710) return JC_ALAPH;
    if (cp == 0x0715 || cp == 0x0716 || cp == 0x072A || cp == 0x072F ||
        cp == 0x074D)
        return JC_DALATH_RISH;
    return -1;
}

static int join_class(uint32_t cp)
{
    int jt = script_joining_type(cp);
    if (jt == JOIN_T) return JC_TRANSPARENT;
    if (jt == JOIN_U) return JC_U;
    if (jt == JOIN_L) return JC_L;
    if (jt == JOIN_R) {
        int g = syriac_group(cp);
        return g >= 0 ? g : JC_R;
    }
    /* D and C. */
    int g = syriac_group(cp);
    return g >= 0 ? g : JC_D;
}

/* The state table. Rows are states, columns are join classes; each cell says
 * what form the PREVIOUS joining character takes, what form THIS one takes,
 * and the next state. AJ_NONE as a prev_action means "leave the previous
 * character alone".
 *
 * States:
 *   0 prev was U, not willing to join
 *   1 prev was R, or an Alaph in isolated form, not willing to join
 *   2 prev was D or L in isolated form, willing to join
 *   3 prev was D in final form, willing to join
 *   4 prev was a final Alaph, not willing to join
 *   5 prev was a fin2/fin3 Alaph, not willing to join
 *   6 prev was Dalath/Rish, not willing to join
 */
struct aj_ent { uint8_t prev, curr, next; };
static const struct aj_ent aj_table[7][JC_NCLASS] = {
 /*        U                     L                     R                     D                     ALAPH                 DALATH_RISH        */
 {{AJ_NONE,AJ_NONE,0},{AJ_NONE,AJ_ISOL,2},{AJ_NONE,AJ_ISOL,1},{AJ_NONE,AJ_ISOL,2},{AJ_NONE,AJ_ISOL,1},{AJ_NONE,AJ_ISOL,6}},
 {{AJ_NONE,AJ_NONE,0},{AJ_NONE,AJ_ISOL,2},{AJ_NONE,AJ_ISOL,1},{AJ_NONE,AJ_ISOL,2},{AJ_NONE,AJ_FIN2,5},{AJ_NONE,AJ_ISOL,6}},
 {{AJ_NONE,AJ_NONE,0},{AJ_NONE,AJ_ISOL,2},{AJ_INIT,AJ_FINA,1},{AJ_INIT,AJ_FINA,3},{AJ_INIT,AJ_FINA,4},{AJ_INIT,AJ_FINA,6}},
 {{AJ_NONE,AJ_NONE,0},{AJ_NONE,AJ_ISOL,2},{AJ_MEDI,AJ_FINA,1},{AJ_MEDI,AJ_FINA,3},{AJ_MEDI,AJ_FINA,4},{AJ_MEDI,AJ_FINA,6}},
 {{AJ_NONE,AJ_NONE,0},{AJ_NONE,AJ_ISOL,2},{AJ_MED2,AJ_ISOL,1},{AJ_MED2,AJ_ISOL,2},{AJ_MED2,AJ_FIN2,5},{AJ_MED2,AJ_ISOL,6}},
 {{AJ_NONE,AJ_NONE,0},{AJ_NONE,AJ_ISOL,2},{AJ_ISOL,AJ_ISOL,1},{AJ_ISOL,AJ_ISOL,2},{AJ_ISOL,AJ_FIN2,5},{AJ_ISOL,AJ_ISOL,6}},
 {{AJ_NONE,AJ_NONE,0},{AJ_NONE,AJ_ISOL,2},{AJ_NONE,AJ_ISOL,1},{AJ_NONE,AJ_ISOL,2},{AJ_NONE,AJ_FIN3,5},{AJ_NONE,AJ_ISOL,6}},
};

void script_arabic_joining(const uint32_t *cps, int n, uint8_t *forms)
{
    int prev = -1, state = 0;

    for (int i = 0; i < n; i++) {
        int jc = join_class(cps[i]);
        if (jc == JC_TRANSPARENT) {
            /* A mark does not participate and does not break the join: it is
             * skipped entirely, so the letters on either side still see each
             * other. This is the single rule that makes vowelled Arabic join. */
            forms[i] = AJ_NONE;
            continue;
        }
        const struct aj_ent *e = &aj_table[state][jc];
        if (e->prev != AJ_NONE && prev >= 0)
            forms[prev] = e->prev;
        forms[i] = e->curr;
        prev = i;
        state = e->next;
    }
}
