/* The Unicode Bidirectional Algorithm (UAX #9).
 *
 * Structure follows the spec's own phases so that a reader with the document
 * open can find each rule:
 *
 *    P2/P3   para_level()             paragraph direction
 *    X1-X8   explicit()               embedding/override/isolate levels
 *    X9      compaction               formatting characters set aside
 *    BD9     match_isolates()         isolate initiator <-> PDI pairing
 *    BD13    the sequence loop        isolating run sequences
 *    X10     sos_eos()                sequence boundary directions
 *    W1-W7   weak()
 *    BD16/N0 brackets()
 *    N1/N2   neutrals()
 *    I1/I2   implicit()
 *    L1/L2   bidi_l1_line / bidi_reorder
 *
 * Two class arrays are kept on purpose. `cls` is the ORIGINAL Bidi_Class and
 * decides structure: which characters X9 removes, which are isolate
 * initiators, what L1 treats as whitespace. `rcls` is the working class that
 * X6's directional override and then W/N rewrite. Collapsing them looks
 * tempting and is wrong: X5a resets an RLI's *type* to L or R under an
 * override, but the character is still structurally an isolate initiator for
 * BD9/BD13, and an NSM that W1 has already rewritten must still be recognised
 * as "originally NSM" by N0's trailing-mark clause.
 *
 * No allocation and no globals: every array lives in the caller's scratch.
 */

#include "bidi.h"

#include "bidi_data.inc"

/* --------------------------------------------------------------- lookups -- */

int bidi_class(uint32_t cp)
{
    if (cp > 0x10FFFF) return BIDI_L;
    return bidi_cls_s2[(unsigned)bidi_cls_s1[cp >> 8] * 256u + (cp & 255u)];
}

uint32_t bidi_mirror_cp(uint32_t cp)
{
    int lo = 0, hi = (int)(sizeof bidi_mirror / sizeof bidi_mirror[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (bidi_mirror[mid][0] == cp) return bidi_mirror[mid][1];
        if (bidi_mirror[mid][0] < cp) lo = mid + 1; else hi = mid - 1;
    }
    return cp;
}

/* BD16 compares brackets under canonical equivalence, which matters for
 * exactly this one pair (the generator already canonicalised the `pair`
 * field, so only the character itself needs folding here). */
static uint32_t canon_bracket(uint32_t cp)
{
    if (cp == 0x2329) return 0x3008;
    if (cp == 0x232A) return 0x3009;
    return cp;
}

/* Returns 0 = not a bracket, 1 = opening, 2 = closing; *pair gets the partner
 * (canonicalised). */
static int bracket_of(uint32_t cp, uint32_t *pair)
{
    int lo = 0, hi = (int)(sizeof bidi_brackets / sizeof bidi_brackets[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (bidi_brackets[mid].cp == cp) {
            *pair = bidi_brackets[mid].pair;
            return bidi_brackets[mid].close ? 2 : 1;
        }
        if (bidi_brackets[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* ------------------------------------------------------------- predicates -- */

static int is_removed(int c)      /* X9 */
{
    return c == BIDI_RLE || c == BIDI_LRE || c == BIDI_RLO || c == BIDI_LRO ||
           c == BIDI_PDF || c == BIDI_BN;
}
static int is_isolate_init(int c) /* BD8 */
{
    return c == BIDI_LRI || c == BIDI_RLI || c == BIDI_FSI;
}
/* BD! "NI": neutral or isolate formatting character. */
static int is_ni(int c)
{
    return c == BIDI_B || c == BIDI_S || c == BIDI_WS || c == BIDI_ON ||
           c == BIDI_FSI || c == BIDI_LRI || c == BIDI_RLI || c == BIDI_PDI;
}
/* The strong direction a class contributes to N0/N1/N2, where EN and AN count
 * as R. Returns -1 for "contributes nothing" -- again not 0, because BIDI_L is
 * 0 and `if (dir)` would quietly drop every left-to-right context. */
static int strong_dir(int c)
{
    if (c == BIDI_L) return BIDI_L;
    if (c == BIDI_R || c == BIDI_EN || c == BIDI_AN) return BIDI_R;
    return -1;
}

/* ------------------------------------------------------------ scratch map -- */

/* One flat block, carved up once. Everything is sized by n, so the caller can
 * size a static buffer from bidi_scratch_size(). */
struct scratch {
    uint8_t *cls;       /* n : original Bidi_Class */
    uint8_t *rcls;      /* n : working class (override + W/N rewrites) */
    uint8_t *elev;      /* n : the X1-X8 embedding levels, kept intact */
    int32_t *mpdi;      /* n : matching PDI of an isolate initiator, else -1 */
    int32_t *minit;     /* n : matching initiator of a PDI, else -1 */
    int32_t *idx;       /* n : positions surviving X9 */
    int32_t *seq;       /* n : the current isolating run sequence's positions */
    int32_t *rstart;    /* n : level run starts (in compacted coordinates) */
    int32_t *rend;      /* n : level run ends, exclusive */
};

#define SCRATCH_PER_CHAR (3 + 6 * 4)

int bidi_scratch_size(int n)
{
    if (n < 0) return 0;
    return n * SCRATCH_PER_CHAR + 16;   /* +16: alignment slack */
}

static int carve(struct scratch *s, void *mem, int len, int n)
{
    unsigned char *p = (unsigned char *)mem;
    unsigned long a = (unsigned long)p & 3u;
    if (len < bidi_scratch_size(n)) return -1;
    if (a) { p += 4 - a; }
    s->mpdi   = (int32_t *)(void *)p;         p += (unsigned)n * 4;
    s->minit  = (int32_t *)(void *)p;         p += (unsigned)n * 4;
    s->idx    = (int32_t *)(void *)p;         p += (unsigned)n * 4;
    s->seq    = (int32_t *)(void *)p;         p += (unsigned)n * 4;
    s->rstart = (int32_t *)(void *)p;         p += (unsigned)n * 4;
    s->rend   = (int32_t *)(void *)p;         p += (unsigned)n * 4;
    s->cls    = (uint8_t *)p;                 p += (unsigned)n;
    s->rcls   = (uint8_t *)p;                 p += (unsigned)n;
    s->elev   = (uint8_t *)p;
    return 0;
}

/* --------------------------------------------------------- BD9: matching -- */

/* Pair each isolate initiator with its PDI over `m` entries whose classes are
 * read through `at` (an index array, or NULL for identity). Unmatched entries
 * get -1 in minit and `m` in mpdi, which is what X5c and X10 want to see. */
static void match_isolates(const uint8_t *cls, const int32_t *at, int m,
                           int32_t *mpdi, int32_t *minit, int32_t *stack)
{
    int sp = 0;
    for (int i = 0; i < m; i++) {
        int p = at ? at[i] : i;
        int c = cls[p];
        mpdi[i] = -1; minit[i] = -1;
        if (is_isolate_init(c)) { mpdi[i] = m; stack[sp++] = i; }
        else if (c == BIDI_PDI && sp > 0) { sp--; mpdi[stack[sp]] = i; minit[i] = stack[sp]; }
    }
}

/* ------------------------------------------------------------- P2/P3, X5c -- */

/* First strong character in [from,to) decides the level: R/AL -> 1 else 0.
 * Isolate initiators cause their whole isolate to be skipped, per P2. */
static int para_level_of(const uint8_t *cls, int from, int to,
                         const int32_t *mpdi)
{
    for (int i = from; i < to; i++) {
        int c = cls[i];
        if (is_isolate_init(c)) {
            int j = mpdi[i];
            i = (j >= to || j < 0) ? to : j;    /* resume at the matching PDI */
            continue;
        }
        if (c == BIDI_L) return 0;
        if (c == BIDI_R || c == BIDI_AL) return 1;
    }
    return 0;
}

int bidi_paragraph_level(const uint32_t *cps, int n)
{
    /* Standalone version: no scratch, so isolates are skipped with a counter
     * instead of a precomputed matching table. Same answer. */
    int depth = 0;
    for (int i = 0; i < n; i++) {
        int c = bidi_class(cps[i]);
        if (is_isolate_init(c)) { depth++; continue; }
        if (c == BIDI_PDI) { if (depth > 0) depth--; continue; }
        if (depth) continue;
        if (c == BIDI_L) return 0;
        if (c == BIDI_R || c == BIDI_AL) return 1;
    }
    return 0;
}

int bidi_is_trivial(const uint32_t *cps, int n)
{
    for (int i = 0; i < n; i++) {
        if (cps[i] < 0x80) continue;            /* ASCII is never RTL */
        switch (bidi_class(cps[i])) {
        case BIDI_L: case BIDI_EN: case BIDI_ES: case BIDI_ET:
        case BIDI_CS: case BIDI_WS: case BIDI_ON: case BIDI_B: case BIDI_S:
            break;
        default: return 0;
        }
    }
    return 1;
}

/* -------------------------------------------------------------- X1 to X8 -- */

/* `override` is a Bidi_Class or OVR_NEUTRAL. It cannot be "0 means neutral":
 * BIDI_L *is* 0, so an LRO's override would silently never apply -- which
 * costs 33k BidiTest cases and looks like an algorithm bug, not a typo. */
#define OVR_NEUTRAL 0xFF
struct dstat { uint8_t level, override, isolate; };

static void explicit_levels(const uint8_t *cls, int n, int para, uint8_t *levels,
                            uint8_t *rcls, const int32_t *mpdi)
{
    struct dstat st[BIDI_MAX_DEPTH + 2];
    int sp = 0;
    int overflow_iso = 0, overflow_emb = 0, valid_iso = 0;

    st[0].level = (uint8_t)para; st[0].override = OVR_NEUTRAL; st[0].isolate = 0;

    for (int i = 0; i < n; i++) {
        int c = cls[i];
        rcls[i] = (uint8_t)c;
        switch (c) {
        case BIDI_RLE: case BIDI_LRE: case BIDI_RLO: case BIDI_LRO: {
            /* X2-X5. The initiator itself is removed by X9; give it the
             * current level so a retaining renderer does not see a hole. */
            levels[i] = st[sp].level;
            int rtl = (c == BIDI_RLE || c == BIDI_RLO);
            int nl = rtl ? ((st[sp].level + 1) | 1) : ((st[sp].level + 2) & ~1);
            if (nl <= BIDI_MAX_DEPTH && !overflow_iso && !overflow_emb) {
                sp++;
                st[sp].level = (uint8_t)nl;
                st[sp].override = (uint8_t)(c == BIDI_LRO ? BIDI_L :
                                            c == BIDI_RLO ? BIDI_R : OVR_NEUTRAL);
                st[sp].isolate = 0;
            } else if (!overflow_iso) {
                overflow_emb++;
            }
            break;
        }
        case BIDI_RLI: case BIDI_LRI: case BIDI_FSI: {
            /* X5a-X5c. FSI first resolves to RLI or LRI by inspecting its own
             * contents (P2/P3 applied between it and its matching PDI). */
            int rtl;
            if (c == BIDI_FSI) {
                int end = mpdi[i];
                if (end < 0 || end > n) end = n;
                rtl = para_level_of(cls, i + 1, end, mpdi);
            } else {
                rtl = (c == BIDI_RLI);
            }
            levels[i] = st[sp].level;
            if (st[sp].override != OVR_NEUTRAL) rcls[i] = st[sp].override;
            int nl = rtl ? ((st[sp].level + 1) | 1) : ((st[sp].level + 2) & ~1);
            if (nl <= BIDI_MAX_DEPTH && !overflow_iso && !overflow_emb) {
                valid_iso++;
                sp++;
                st[sp].level = (uint8_t)nl;
                st[sp].override = OVR_NEUTRAL;
                st[sp].isolate = 1;
            } else {
                overflow_iso++;
            }
            break;
        }
        case BIDI_PDI:
            /* X6a. */
            if (overflow_iso > 0) {
                overflow_iso--;
            } else if (valid_iso > 0) {
                overflow_emb = 0;
                while (!st[sp].isolate) sp--;
                sp--;
                valid_iso--;
            }
            levels[i] = st[sp].level;
            if (st[sp].override != OVR_NEUTRAL) rcls[i] = st[sp].override;
            break;
        case BIDI_PDF:
            /* X7. The level reported is the one after popping, so the PDF sits
             * with the text that follows it rather than splitting a run. */
            if (overflow_iso > 0) {
                /* nothing */
            } else if (overflow_emb > 0) {
                overflow_emb--;
            } else if (!st[sp].isolate && sp > 0) {
                sp--;
            }
            levels[i] = st[sp].level;
            break;
        case BIDI_B:
            /* X8. We handle one paragraph at a time, so B is terminal. */
            levels[i] = (uint8_t)para;
            break;
        case BIDI_BN:
            levels[i] = st[sp].level;
            break;
        default:
            /* X6. */
            levels[i] = st[sp].level;
            if (st[sp].override != OVR_NEUTRAL) rcls[i] = st[sp].override;
            break;
        }
    }
}

/* ----------------------------------------------------------------- W1-W7 -- */

static void weak(uint8_t *rcls, const int32_t *seq, int m, int sos)
{
    int i;

    /* W1: NSM takes the type of the previous character; after an isolate
     * initiator or PDI it becomes ON, not the initiator's type. */
    {
        int prev = sos;
        for (i = 0; i < m; i++) {
            int c = rcls[seq[i]];
            if (c == BIDI_NSM) rcls[seq[i]] = (uint8_t)prev;
            else prev = (is_isolate_init(c) || c == BIDI_PDI) ? BIDI_ON : c;
        }
    }

    /* W2: EN becomes AN when the last strong type before it was AL. */
    {
        int strong = sos;
        for (i = 0; i < m; i++) {
            int c = rcls[seq[i]];
            if (c == BIDI_L || c == BIDI_R || c == BIDI_AL) strong = c;
            else if (c == BIDI_EN && strong == BIDI_AL) rcls[seq[i]] = BIDI_AN;
        }
    }

    /* W3: AL becomes R. */
    for (i = 0; i < m; i++)
        if (rcls[seq[i]] == BIDI_AL) rcls[seq[i]] = BIDI_R;

    /* W4: a single ES between two ENs, or a single CS between two numbers of
     * the same type, becomes that number type. */
    for (i = 1; i + 1 < m; i++) {
        int c = rcls[seq[i]], a = rcls[seq[i - 1]], b = rcls[seq[i + 1]];
        if (c == BIDI_ES && a == BIDI_EN && b == BIDI_EN) rcls[seq[i]] = BIDI_EN;
        else if (c == BIDI_CS && a == b && (a == BIDI_EN || a == BIDI_AN))
            rcls[seq[i]] = (uint8_t)a;
    }

    /* W5: a run of ETs adjacent to an EN becomes EN. */
    for (i = 0; i < m; i++) {
        if (rcls[seq[i]] != BIDI_ET) continue;
        int j = i;
        while (j < m && rcls[seq[j]] == BIDI_ET) j++;
        int before = (i > 0) && rcls[seq[i - 1]] == BIDI_EN;
        int after  = (j < m) && rcls[seq[j]] == BIDI_EN;
        if (before || after)
            for (int k = i; k < j; k++) rcls[seq[k]] = BIDI_EN;
        i = j - 1;
    }

    /* W6: leftover separators and terminators become ON. */
    for (i = 0; i < m; i++) {
        int c = rcls[seq[i]];
        if (c == BIDI_ET || c == BIDI_ES || c == BIDI_CS) rcls[seq[i]] = BIDI_ON;
    }

    /* W7: EN becomes L when the last strong type before it was L. */
    {
        int strong = sos;
        for (i = 0; i < m; i++) {
            int c = rcls[seq[i]];
            if (c == BIDI_L || c == BIDI_R) strong = c;
            else if (c == BIDI_EN && strong == BIDI_L) rcls[seq[i]] = BIDI_L;
        }
    }
}

/* ------------------------------------------------------------- BD16 / N0 -- */

#define BRACKET_STACK 63    /* the fixed capacity BD16 specifies */

static void brackets(const uint32_t *cps, const uint8_t *cls, uint8_t *rcls,
                     const int32_t *seq, int m, int level, int sos)
{
    /* BD16: pair up brackets whose current class is still ON. */
    struct { uint32_t expect; int pos; } stk[BRACKET_STACK];
    int open_at[BRACKET_STACK], close_at[BRACKET_STACK];
    int sp = 0, npair = 0;

    for (int i = 0; i < m && npair < BRACKET_STACK; i++) {
        if (rcls[seq[i]] != BIDI_ON) continue;
        uint32_t pair;
        int kind = bracket_of(cps[seq[i]], &pair);
        if (kind == 1) {
            if (sp == BRACKET_STACK) break;   /* BD16: stop, keep what we have */
            stk[sp].expect = pair;
            stk[sp].pos = i;
            sp++;
        } else if (kind == 2) {
            uint32_t me = canon_bracket(cps[seq[i]]);
            for (int k = sp - 1; k >= 0; k--) {
                if (stk[k].expect != me) continue;
                open_at[npair] = stk[k].pos;
                close_at[npair] = i;
                npair++;
                sp = k;                       /* discard the skipped openers */
                break;
            }
        }
    }

    /* Sort by opening position: N0 processes pairs in logical order of their
     * opening brackets, and BD16's stack discipline does not produce them in
     * that order when pairs nest. Insertion sort; npair <= 63. */
    for (int i = 1; i < npair; i++) {
        int o = open_at[i], c = close_at[i], j = i - 1;
        while (j >= 0 && open_at[j] > o) {
            open_at[j + 1] = open_at[j]; close_at[j + 1] = close_at[j]; j--;
        }
        open_at[j + 1] = o; close_at[j + 1] = c;
    }

    int e = (level & 1) ? BIDI_R : BIDI_L;
    int o = (level & 1) ? BIDI_L : BIDI_R;

    for (int p = 0; p < npair; p++) {
        int ob = open_at[p], cb = close_at[p];
        int found_e = 0, found_o = 0;
        for (int k = ob + 1; k < cb; k++) {
            int d = strong_dir(rcls[seq[k]]);
            if (d < 0) continue;
            if (d == e) { found_e = 1; break; }
            found_o = 1;
        }
        int set = -1;
        if (found_e) {
            set = e;                                  /* N0 b */
        } else if (found_o) {
            int ctx = sos;                            /* N0 c */
            for (int k = ob - 1; k >= 0; k--) {
                int d = strong_dir(rcls[seq[k]]);
                if (d >= 0) { ctx = d; break; }
            }
            set = (ctx == o) ? o : e;                 /* c.1 : c.2 */
        }
        if (set < 0) continue;                        /* N0 d: leave alone */
        rcls[seq[ob]] = (uint8_t)set;
        rcls[seq[cb]] = (uint8_t)set;
        /* Marks that were NSM before W1 follow the bracket they attach to. */
        for (int k = ob + 1; k < m && cls[seq[k]] == BIDI_NSM; k++)
            rcls[seq[k]] = (uint8_t)set;
        for (int k = cb + 1; k < m && cls[seq[k]] == BIDI_NSM; k++)
            rcls[seq[k]] = (uint8_t)set;
    }
}

/* ----------------------------------------------------------------- N1, N2 -- */

static void neutrals(uint8_t *rcls, const int32_t *seq, int m, int level,
                     int sos, int eos)
{
    int e = (level & 1) ? BIDI_R : BIDI_L;
    for (int i = 0; i < m; i++) {
        if (!is_ni(rcls[seq[i]])) continue;
        int j = i;
        while (j < m && is_ni(rcls[seq[j]])) j++;
        int before = (i > 0) ? strong_dir(rcls[seq[i - 1]]) : sos;
        int after  = (j < m) ? strong_dir(rcls[seq[j]]) : eos;
        int set = (before >= 0 && before == after) ? before : e;   /* N1 : N2 */
        for (int k = i; k < j; k++) rcls[seq[k]] = (uint8_t)set;
        i = j - 1;
    }
}

/* ----------------------------------------------------------------- I1, I2 -- */

static void implicit(const uint8_t *rcls, const int32_t *seq, int m, int level,
                     uint8_t *levels)
{
    for (int i = 0; i < m; i++) {
        int c = rcls[seq[i]];
        int l = level;
        if (level & 1) {                       /* I2 */
            if (c == BIDI_L || c == BIDI_EN || c == BIDI_AN) l = level + 1;
        } else {                               /* I1 */
            if (c == BIDI_R) l = level + 1;
            else if (c == BIDI_EN || c == BIDI_AN) l = level + 2;
        }
        levels[seq[i]] = (uint8_t)l;
    }
}

/* --------------------------------------------------------------------- L1 -- */

void bidi_l1_line(const uint32_t *cps, int n, int para_level, uint8_t *levels)
{
    /* L1 works on ORIGINAL types. Section 5.2 folds the X9-removed formatting
     * characters in with whitespace, so a trailing "WS RLE WS" resets whole. */
    int reset_from = 0;     /* start of the current resettable tail */
    int resettable = 1;
    for (int i = 0; i < n; i++) {
        int c = bidi_class(cps[i]);
        if (c == BIDI_B || c == BIDI_S) {
            levels[i] = (uint8_t)para_level;
            for (int k = reset_from; k < i; k++) levels[k] = (uint8_t)para_level;
            reset_from = i + 1;
            resettable = 1;
        } else if (c == BIDI_WS || is_isolate_init(c) || c == BIDI_PDI ||
                   is_removed(c)) {
            if (!resettable) { reset_from = i; resettable = 1; }
        } else {
            resettable = 0;
            reset_from = i + 1;
        }
    }
    if (resettable)
        for (int k = reset_from; k < n; k++) levels[k] = (uint8_t)para_level;
}

/* --------------------------------------------------------------------- L2 -- */

void bidi_reorder(const uint8_t *levels, int n, int *order)
{
    int i;
    int highest = 0, lowest_odd = BIDI_MAX_DEPTH + 2;

    for (i = 0; i < n; i++) order[i] = i;
    for (i = 0; i < n; i++) {
        int l = levels[i];
        if (l > highest) highest = l;
        if ((l & 1) && l < lowest_odd) lowest_odd = l;
    }
    for (int lev = highest; lev >= lowest_odd; lev--) {
        for (i = 0; i < n; i++) {
            if (levels[order[i]] < lev) continue;
            int j = i;
            while (j < n && levels[order[j]] >= lev) j++;
            for (int a = i, b = j - 1; a < b; a++, b--) {
                int t = order[a]; order[a] = order[b]; order[b] = t;
            }
            i = j - 1;
        }
    }
}

/* ---------------------------------------------------------- the whole run -- */

int bidi_resolve(const uint32_t *cps, int n, int dir, uint8_t *levels,
                 void *scratch, int scratchlen)
{
    struct scratch s;
    int i;

    if (n < 0) return -1;
    if (n == 0) return dir == BIDI_DIR_RTL ? 1 : 0;
    if (carve(&s, scratch, scratchlen, n) != 0) return -1;

    for (i = 0; i < n; i++) s.cls[i] = (uint8_t)bidi_class(cps[i]);

    /* BD9 over the full text -- X5c needs it before anything is removed. */
    match_isolates(s.cls, 0, n, s.mpdi, s.minit, s.rstart);

    int para = (dir == BIDI_DIR_LTR) ? 0 :
               (dir == BIDI_DIR_RTL) ? 1 :
               para_level_of(s.cls, 0, n, s.mpdi);

    /* X10 compares a sequence's level with its NEIGHBOURS' embedding levels.
     * Those must be the levels the explicit phase produced: if sos/eos read
     * `levels` instead, an earlier sequence's I1/I2 result would have already
     * overwritten the neighbour and every second run would come out wrong. */
    explicit_levels(s.cls, n, para, s.elev, s.rcls, s.mpdi);
    for (i = 0; i < n; i++) levels[i] = s.elev[i];

    /* X9: set the formatting characters aside. Everything downstream works in
     * these compacted coordinates, which is why sos/eos and W/N never have to
     * special-case a BN sitting between two real characters. */
    int m = 0;
    for (i = 0; i < n; i++)
        if (!is_removed(s.cls[i])) s.idx[m++] = i;

    if (m == 0) {
        for (i = 0; i < n; i++) levels[i] = (uint8_t)para;
        return para;
    }

    /* BD9 again, now in compacted coordinates, for BD13 and X10. */
    match_isolates(s.cls, s.idx, m, s.mpdi, s.minit, s.rend);

    /* Level runs over the compacted text. */
    int nrun = 0;
    for (i = 0; i < m; ) {
        int j = i + 1;
        while (j < m && s.elev[s.idx[j]] == s.elev[s.idx[i]]) j++;
        s.rstart[nrun] = i; s.rend[nrun] = j; nrun++;
        i = j;
    }

    /* BD13: stitch level runs into isolating run sequences. A run that starts
     * with a PDI matching an earlier initiator is a continuation, never a
     * start. */
    for (int r = 0; r < nrun; r++) {
        int first = s.rstart[r];
        if (s.cls[s.idx[first]] == BIDI_PDI && s.minit[first] >= 0) continue;

        int mlen = 0;
        int cur = r;
        for (;;) {
            for (i = s.rstart[cur]; i < s.rend[cur]; i++) s.seq[mlen++] = s.idx[i];
            int last = s.rend[cur] - 1;
            if (!is_isolate_init(s.cls[s.idx[last]])) break;
            int pdi = s.mpdi[last];
            if (pdi < 0 || pdi >= m) break;         /* no matching PDI */
            /* Find the run that starts at `pdi`. Runs are sorted by start. */
            int lo = cur + 1, hi = nrun - 1, found = -1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (s.rstart[mid] == pdi) { found = mid; break; }
                if (s.rstart[mid] < pdi) lo = mid + 1; else hi = mid - 1;
            }
            if (found < 0) break;
            cur = found;
        }

        int level = s.elev[s.seq[0]];

        /* X10: sos from the character before the sequence, eos from the one
         * after -- except that a sequence ending in an unmatched isolate
         * initiator is bounded by the paragraph, not by whatever follows. */
        int prev_level = para, next_level = para;
        {
            int p = s.rstart[r] - 1;
            if (p >= 0) prev_level = s.elev[s.idx[p]];
        }
        {
            int lastrun_end = s.rend[cur];
            int lastch = s.seq[mlen - 1];
            if (is_isolate_init(s.cls[lastch]) &&
                (s.mpdi[lastrun_end - 1] < 0 || s.mpdi[lastrun_end - 1] >= m)) {
                next_level = para;
            } else if (lastrun_end < m) {
                next_level = s.elev[s.idx[lastrun_end]];
            }
        }
        int hs = prev_level > level ? prev_level : level;
        int he = next_level > level ? next_level : level;
        int sos = (hs & 1) ? BIDI_R : BIDI_L;
        int eos = (he & 1) ? BIDI_R : BIDI_L;

        weak(s.rcls, s.seq, mlen, sos);
        brackets(cps, s.cls, s.rcls, s.seq, mlen, level, sos);
        neutrals(s.rcls, s.seq, mlen, level, sos, eos);
        implicit(s.rcls, s.seq, mlen, level, levels);
    }

    /* Section 5.2: a removed character takes the level of what precedes it. */
    {
        int last = para;
        int k = 0;
        for (i = 0; i < n; i++) {
            if (k < m && s.idx[k] == i) { last = levels[i]; k++; }
            else levels[i] = (uint8_t)last;
        }
    }

    bidi_l1_line(cps, n, para, levels);
    return para;
}
