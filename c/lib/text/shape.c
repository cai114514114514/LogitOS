/* Text shaping: GSUB + GPOS over c/lib/text/otlayout.c.
 *
 * See shape.h for the contract. The structure below follows the order the work
 * actually happens in, which is also the order HarfBuzz does it -- deliberately,
 * because tests/unit/shape_test.c compares our output against HarfBuzz's glyph
 * by glyph, and the only way to survive that is to make the same decisions in
 * the same order:
 *
 *   plan_build()      which features, in which stages, with which mask bits
 *   sh_map()          code points -> glyph ids (mirroring first, for RTL)
 *   gsub_stage()      apply one stage's lookups, sorted by lookup index
 *   gpos_apply()      one stage, then attachment propagation, then mark zeroing
 *   shape_run()       ties it together and reverses an RTL run at the end
 *
 * Two things are worth stating because they look like accidents and are not:
 *
 *  - A lookup is applied to the whole buffer before the next lookup starts, and
 *    within a stage lookups run in increasing LOOKUP INDEX order, not feature
 *    order. That is the OpenType model and it is why 'liga' and 'ccmp' can
 *    interleave.
 *  - Each glyph carries a mask. Global features have bit 0; the seven Arabic
 *    joining features get a bit each, and script.c's state machine decides which
 *    bit each letter gets. That single mechanism is the whole of contextual
 *    form selection -- there is no if-Arabic branch in the lookup application.
 *
 * No allocation, no globals, no libc.
 */

#include "shape.h"
#include "script.h"
#include "otlayout.h"
#include "bidi.h"
#include "fontrd.h"

/* ------------------------------------------------------------------ limits -- */

#define SH_MAX_LOOKUPS   256      /* per table; DejaVu uses ~120 */
#define SH_MAX_STAGES    13
#define SH_MAX_FEATS     160      /* features a langsys may list */
#define SH_NEST          6        /* contextual recursion depth (HarfBuzz: 6) */
#define SH_CTX           OTL_CTX_MAX

/* --------------------------------------------------------------- the plan -- */

struct sh_lk { uint16_t idx; uint32_t mask; };

struct sh_plan {
    struct otl_table gsub, gpos;
    struct otl_gdef  gdef;
    struct otl_kern  kern;
    int has_gsub, has_gpos, has_gdef, has_gdef_classes, has_kern;
    int use_legacy_kern;                 /* only when the font has no GPOS */
    int cursive;

    struct sh_lk gsub_lk[SH_MAX_LOOKUPS];
    int gsub_stage_end[SH_MAX_STAGES];   /* gsub_lk[.. end) belongs to stage <= i */
    int n_gsub_lk, n_stage;

    struct sh_lk gpos_lk[SH_MAX_LOOKUPS];
    int n_gpos_lk;

    uint32_t global_mask;
    uint32_t form_mask[AJ_NFORM];        /* AJ_ISOL..AJ_INIT */
    uint32_t rtlm_mask;
};

/* One entry of the feature plan: what to enable, in which GSUB stage, and
 * whether every glyph gets it (global) or only glyphs the shaper marks. */
struct sh_feat { uint32_t tag; uint8_t stage; uint8_t global; };

#define TAG FONT_TAG

/* The seven Arabic joining features, in AJ_* order. The order is the spec's and
 * each gets its own stage, so 'fina' can see what 'isol' produced. */
static const uint32_t arabic_feats[AJ_NFORM] = {
    TAG('i','s','o','l'), TAG('f','i','n','a'), TAG('f','i','n','2'),
    TAG('f','i','n','3'), TAG('m','e','d','i'), TAG('m','e','d','2'),
    TAG('i','n','i','t'),
};

/* The features every run gets, in the stage they land in for the plain shaper.
 * Stage 0 is 'rvrn' alone (a pause follows it); everything else is stage 1.
 * This is HarfBuzz's common_features + horizontal_features, minus the ones no
 * font has ever shipped ('Harf', 'Buzz', 'trak') and minus the ones that need
 * machinery we do not have (automatic fractions: 'frac'/'numr'/'dnom'). */
static const struct sh_feat plain_feats[] = {
    { TAG('r','v','r','n'), 0, 1 },
    { TAG('a','b','v','m'), 1, 1 },
    { TAG('b','l','w','m'), 1, 1 },
    { TAG('c','c','m','p'), 1, 1 },
    { TAG('l','o','c','l'), 1, 1 },
    { TAG('m','a','r','k'), 1, 1 },
    { TAG('m','k','m','k'), 1, 1 },
    { TAG('r','l','i','g'), 1, 1 },
    { TAG('c','a','l','t'), 1, 1 },
    { TAG('c','l','i','g'), 1, 1 },
    { TAG('c','u','r','s'), 1, 1 },
    { TAG('d','i','s','t'), 1, 1 },
    { TAG('k','e','r','n'), 1, 1 },
    { TAG('l','i','g','a'), 1, 1 },
    { TAG('r','c','l','t'), 1, 1 },
};

/* The cursive (Arabic-family) plan. Each joining feature sits alone in its own
 * stage; the tags that also appear in plain_feats take the EARLIEST stage they
 * are named in, which is what HarfBuzz's feature merge does. */
static const struct sh_feat cursive_feats[] = {
    { TAG('r','v','r','n'),  0, 1 },
    { TAG('s','t','c','h'),  1, 1 },
    { TAG('c','c','m','p'),  2, 1 },
    { TAG('l','o','c','l'),  2, 1 },
    { TAG('i','s','o','l'),  3, 0 },
    { TAG('f','i','n','a'),  4, 0 },
    { TAG('f','i','n','2'),  5, 0 },
    { TAG('f','i','n','3'),  6, 0 },
    { TAG('m','e','d','i'),  7, 0 },
    { TAG('m','e','d','2'),  8, 0 },
    { TAG('i','n','i','t'),  9, 0 },
    { TAG('r','c','l','t'), 10, 1 },
    { TAG('c','a','l','t'), 10, 1 },
    { TAG('l','i','g','a'), 11, 1 },
    { TAG('c','l','i','g'), 11, 1 },
    { TAG('m','s','e','t'), 11, 1 },
    { TAG('a','b','v','m'), 11, 1 },
    { TAG('b','l','w','m'), 11, 1 },
    { TAG('m','a','r','k'), 11, 1 },
    { TAG('m','k','m','k'), 11, 1 },
    { TAG('r','l','i','g'), 11, 1 },
    { TAG('c','u','r','s'), 11, 1 },
    { TAG('d','i','s','t'), 11, 1 },
    { TAG('k','e','r','n'), 11, 1 },
};

/* ------------------------------------------------------- feature lookups -- */

/* The (script, langsys) a table offers for this run, plus its feature list. */
struct sh_langsys {
    int si, li, required;
    uint16_t feat[SH_MAX_FEATS];
    uint32_t tag[SH_MAX_FEATS];
    int nfeat;
};

static int sel_script(const struct otl_table *t, uint32_t want)
{
    int si = otl_find_script(t, want);
    if (si >= 0) return si;
    /* The fallback chain every shaper uses: 'DFLT', then 'dflt' (a typo on the
     * Microsoft site that a generation of fonts copied), then 'latn' (fonts
     * that put their features there and hoped). */
    if ((si = otl_find_script(t, TAG('D','F','L','T'))) >= 0) return si;
    if ((si = otl_find_script(t, TAG('d','f','l','t'))) >= 0) return si;
    return otl_find_script(t, TAG('l','a','t','n'));
}

static void langsys_open(const struct otl_table *t, uint32_t script_tag,
                         struct sh_langsys *ls)
{
    ls->si = ls->li = -1; ls->required = -1; ls->nfeat = 0;
    ls->si = sel_script(t, script_tag);
    if (ls->si < 0) return;
    int n = otl_langsys_features(t, ls->si, -1, &ls->required, ls->feat, SH_MAX_FEATS);
    if (n < 0) n = 0;
    if (n > SH_MAX_FEATS) n = SH_MAX_FEATS;
    ls->nfeat = n;
    for (int i = 0; i < n; i++) ls->tag[i] = otl_feature_tag(t, ls->feat[i]);
}

static int langsys_find(const struct sh_langsys *ls, uint32_t tag)
{
    for (int i = 0; i < ls->nfeat; i++)
        if (ls->tag[i] == tag) return ls->feat[i];
    return -1;
}

static void lk_add(struct sh_lk *arr, int *n, int base, int cap,
                   uint16_t idx, uint32_t mask)
{
    for (int i = base; i < *n; i++)
        if (arr[i].idx == idx) { arr[i].mask |= mask; return; }
    if (*n >= cap) return;
    arr[*n].idx = idx; arr[*n].mask = mask; (*n)++;
}

static void lk_sort(struct sh_lk *arr, int lo, int hi)
{
    for (int i = lo + 1; i < hi; i++) {
        struct sh_lk k = arr[i];
        int j = i - 1;
        while (j >= lo && arr[j].idx > k.idx) { arr[j + 1] = arr[j]; j--; }
        arr[j + 1] = k;
    }
}

/* Collect a feature's lookups into `arr`. */
static void feat_lookups(const struct otl_table *t, int fi, uint32_t mask,
                         struct sh_lk *arr, int *n, int base, int cap)
{
    uint16_t lk[64];
    int c = otl_feature_lookups(t, fi, lk, 64);
    if (c < 0) return;
    if (c > 64) c = 64;
    for (int i = 0; i < c; i++) lk_add(arr, n, base, cap, lk[i], mask);
}

static void plan_build(struct sh_plan *p, const struct ttf_font *f, int script)
{
    const struct sh_feat *fl;
    int nfl, nstage;

    for (int i = 0; i < AJ_NFORM; i++) p->form_mask[i] = 0;
    p->global_mask = 1u;
    p->rtlm_mask = 0;
    p->n_gsub_lk = p->n_gpos_lk = 0;
    p->cursive = script_is_cursive(script);

    p->has_gsub = otl_open(f->data, (uint32_t)f->len, f->off_gsub, 0, &p->gsub) == 0;
    p->has_gpos = otl_open(f->data, (uint32_t)f->len, f->off_gpos, 1, &p->gpos) == 0;
    p->has_gdef = otl_gdef_open(f->data, (uint32_t)f->len, f->off_gdef, &p->gdef) == 0;
    p->has_gdef_classes = p->has_gdef && p->gdef.glyph_class != 0;
    p->has_kern = otl_kern_open(f->data, (uint32_t)f->len, f->off_kern, &p->kern) == 0;
    /* The legacy `kern` table is a FALLBACK, not an addition: a font with GPOS
     * has already said where its kerning lives, and applying both double-kerns
     * every pair. This is the same gate HarfBuzz uses. */
    p->use_legacy_kern = p->has_kern && !(p->has_gpos && otl_lookup_count(&p->gpos) > 0);

    if (p->cursive) { fl = cursive_feats; nfl = (int)(sizeof cursive_feats / sizeof cursive_feats[0]); nstage = 12; }
    else            { fl = plain_feats;   nfl = (int)(sizeof plain_feats   / sizeof plain_feats[0]);   nstage = 2;  }
    p->n_stage = nstage;

    /* Give each non-global feature a mask bit. */
    uint32_t bit = 2u;
    if (p->cursive)
        for (int i = 0; i < AJ_NFORM; i++) { p->form_mask[i] = bit; bit <<= 1; }
    p->rtlm_mask = bit;

    uint32_t stag = script_ot_tag(script);

    if (p->has_gsub) {
        struct sh_langsys ls;
        langsys_open(&p->gsub, stag, &ls);
        for (int s = 0; s < nstage; s++) {
            int base = p->n_gsub_lk;
            for (int i = 0; i < nfl; i++) {
                if (fl[i].stage != s) continue;
                int fi = langsys_find(&ls, fl[i].tag);
                if (fi < 0) continue;
                uint32_t m = p->global_mask;
                if (!fl[i].global) {
                    m = 0;
                    for (int a = 0; a < AJ_NFORM; a++)
                        if (arabic_feats[a] == fl[i].tag) { m = p->form_mask[a]; break; }
                    if (!m) continue;
                }
                feat_lookups(&p->gsub, fi, m, p->gsub_lk, &p->n_gsub_lk, base, SH_MAX_LOOKUPS);
            }
            /* The langsys's required feature is unconditional and lands in the
             * last stage. */
            if (s == nstage - 1 && ls.required >= 0)
                feat_lookups(&p->gsub, ls.required, p->global_mask,
                             p->gsub_lk, &p->n_gsub_lk, base, SH_MAX_LOOKUPS);
            lk_sort(p->gsub_lk, base, p->n_gsub_lk);
            p->gsub_stage_end[s] = p->n_gsub_lk;
        }
    } else {
        for (int s = 0; s < nstage; s++) p->gsub_stage_end[s] = 0;
    }

    /* GPOS has no pauses, so it is one stage: every feature's lookups merged
     * and sorted by lookup index. */
    if (p->has_gpos) {
        struct sh_langsys ls;
        langsys_open(&p->gpos, stag, &ls);
        for (int i = 0; i < nfl; i++) {
            int fi = langsys_find(&ls, fl[i].tag);
            if (fi < 0) continue;
            uint32_t m = p->global_mask;
            if (!fl[i].global) {
                m = 0;
                for (int a = 0; a < AJ_NFORM; a++)
                    if (arabic_feats[a] == fl[i].tag) { m = p->form_mask[a]; break; }
                if (!m) continue;
            }
            feat_lookups(&p->gpos, fi, m, p->gpos_lk, &p->n_gpos_lk, 0, SH_MAX_LOOKUPS);
        }
        if (ls.required >= 0)
            feat_lookups(&p->gpos, ls.required, p->global_mask,
                         p->gpos_lk, &p->n_gpos_lk, 0, SH_MAX_LOOKUPS);
        lk_sort(p->gpos_lk, 0, p->n_gpos_lk);
    }
}

/* --------------------------------------------------------------- context -- */

struct sh_ctx {
    const struct ttf_font *f;
    const struct sh_plan  *p;
    const struct otl_table *t;           /* the table being applied */
    struct shape_glyph *g;
    int n, cap;
    int idx;
    uint32_t mask;                       /* the current lookup's mask */
    uint32_t props;                      /* the current lookup's flags */
    int nest;
    int is_gpos;
    int rtl;
    int lig_serial;
    int overflow;
};

static uint32_t gdef_props(const struct sh_plan *p, uint16_t gid)
{
    if (!p->has_gdef_classes) return 0;
    int k = otl_gdef_class(&p->gdef, gid);
    switch (k) {
    case OTL_GC_BASE:     return SH_P_BASE;
    case OTL_GC_LIGATURE: return SH_P_LIGATURE;
    case OTL_GC_MARK: {
        int a = otl_gdef_mark_attach(&p->gdef, gid);
        return SH_P_MARK | ((uint32_t)(a & 0xFF) << 8);
    }
    default: return 0;
    }
}

/* Default_Ignorable_Code_Point, the subset that actually occurs in text. These
 * glyphs take part in shaping (a ZWNJ has to break a join) and are hidden
 * afterwards, exactly as HarfBuzz does it. */
static int is_ignorable(uint32_t cp)
{
    switch (cp >> 8) {
    case 0x00: return cp == 0x00AD;
    case 0x03: return cp == 0x034F;
    case 0x06: return cp == 0x061C;
    case 0x17: return cp >= 0x17B4 && cp <= 0x17B5;
    case 0x18: return cp >= 0x180B && cp <= 0x180E;
    case 0x20: return (cp >= 0x200B && cp <= 0x200F) ||
                      (cp >= 0x202A && cp <= 0x202E) ||
                      (cp >= 0x2060 && cp <= 0x206F);
    case 0xFE: return (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF;
    case 0xFF: return cp >= 0xFFF0 && cp <= 0xFFF8;
    default:
        if (cp >= 0x1BCA0 && cp <= 0x1BCA3) return 1;
        if (cp >= 0x1D173 && cp <= 0x1D17A) return 1;
        if (cp >= 0xE0000 && cp <= 0xE0FFF) return 1;
        return 0;
    }
}

/* Does this glyph pass the current lookup's LookupFlag filter? */
static int prop_ok(struct sh_ctx *c, int i)
{
    uint32_t gp = c->g[i].props;
    if (gp & c->props & (SH_P_BASE | SH_P_LIGATURE | SH_P_MARK)) return 0;
    if (gp & SH_P_MARK) {
        if (c->props & OTL_LF_USE_MARK_FILTER_SET)
            return c->p->has_gdef &&
                   otl_gdef_mark_set_covers(&c->p->gdef, (int)(c->props >> 16),
                                            c->g[i].gid) == 1;
        if (c->props & OTL_LF_MARK_ATTACH_TYPE)
            return (c->props & OTL_LF_MARK_ATTACH_TYPE) == (gp & OTL_LF_MARK_ATTACH_TYPE);
    }
    return 1;
}

/* 0 = do not skip, 1 = skip, 2 = skip unless it matches (a default ignorable). */
static int skip_of(struct sh_ctx *c, int i)
{
    if (!prop_ok(c, i)) return 1;
    if (c->g[i].props & SH_P_IGNORABLE) return 2;
    return 0;
}

/* Next position at or after i+1 whose glyph satisfies `want` and carries the
 * mask; -1 if the first non-skippable glyph fails. `want` of -1 accepts any
 * glyph (used when the caller only needs the next real glyph). */
struct sh_want { int kind; uint16_t v; uint32_t cov; uint32_t cd; };
enum { W_ANY = 0, W_GLYPH, W_CLASS, W_COV };

static int want_ok(struct sh_ctx *c, const struct sh_want *w, uint16_t gid)
{
    switch (w->kind) {
    case W_GLYPH: return gid == w->v;
    case W_CLASS: return otl_class_of(c->t, w->cd, gid) == (int)w->v;
    case W_COV:   return otl_coverage_index(c->t, w->cov, gid) >= 0;
    default:      return 1;
    }
}

static int iter_next(struct sh_ctx *c, int i, const struct sh_want *w, int use_mask)
{
    for (i++; i < c->n; i++) {
        int sk = skip_of(c, i);
        if (sk == 1) continue;
        int m = want_ok(c, w, c->g[i].gid) &&
                (!use_mask || (c->g[i].mask & c->mask));
        if (m) return i;
        if (sk == 0) return -1;
    }
    return -1;
}

static int iter_prev(struct sh_ctx *c, int i, const struct sh_want *w, int use_mask)
{
    for (i--; i >= 0; i--) {
        int sk = skip_of(c, i);
        if (sk == 1) continue;
        int m = want_ok(c, w, c->g[i].gid) &&
                (!use_mask || (c->g[i].mask & c->mask));
        if (m) return i;
        if (sk == 0) return -1;
    }
    return -1;
}

/* ---------------------------------------------------------- buffer edits -- */

static void buf_set_glyph(struct sh_ctx *c, int i, uint16_t gid, uint32_t extra)
{
    uint32_t keep = c->g[i].props & (SH_P_SUBSTITUTED | SH_P_LIGATED | SH_P_MULTIPLIED |
                                     SH_P_IGNORABLE);
    keep |= SH_P_SUBSTITUTED | extra;
    if (extra & SH_P_LIGATED) keep &= ~SH_P_MULTIPLIED;
    c->g[i].gid = gid;
    c->g[i].props = keep | gdef_props(c->p, gid);
}

static void buf_delete(struct sh_ctx *c, int i)
{
    for (int j = i; j + 1 < c->n; j++) c->g[j] = c->g[j + 1];
    c->n--;
}

static int buf_insert(struct sh_ctx *c, int i, int count)
{
    if (c->n + count > c->cap) { c->overflow = 1; return -1; }
    for (int j = c->n - 1; j >= i; j--) c->g[j + count] = c->g[j];
    c->n += count;
    return 0;
}

/* ------------------------------------------------------------------ GSUB -- */

static int gsub_lookup_at(struct sh_ctx *c, int li);
static int gsub_recurse(struct sh_ctx *c, int li, int at);

/* Apply the contextual lookup records of a matched rule. Transcribed from the
 * OpenType model: each record names a position within the matched input and a
 * lookup to run there, and a recursed lookup that changes the buffer length
 * shifts every later position. */
static void ctx_records(struct sh_ctx *c, int count, int *pos, int nrec,
                        const struct otl_lookup_rec *rec, int match_end,
                        int (*run)(struct sh_ctx *, int, int))
{
    int end = c->n - match_end + count;

    for (int i = 0; i < nrec && !c->overflow; i++) {
        int idx = rec[i].seq;
        if (idx >= count) continue;
        if (pos[idx] >= c->n) continue;
        int before = c->n;
        int save = c->nest;
        c->nest--;
        if (c->nest >= 0) run(c, rec[i].lookup, pos[idx]);
        c->nest = save;
        int delta = c->n - before;
        if (!delta) continue;
        end += delta;
        if (end < pos[idx]) { end = pos[idx]; break; }
        int next = idx + 1;
        if (delta > 0) {
            if (delta + count > SH_CTX) break;
            for (int j = count - 1; j >= next; j--) pos[j + delta] = pos[j];
        } else {
            if (delta < next - count) delta = next - count;
            next -= delta;
            for (int j = next; j < count; j++) pos[j + delta] = pos[j];
        }
        next += delta;
        count += delta;
        for (int j = idx + 1; j < next; j++) pos[j] = pos[j - 1] + 1;
        for (; next < count; next++) pos[next] += delta;
    }
    if (end < 0) end = 0;
    if (end > c->n) end = c->n;
    c->idx = end;
}

/* Match one contextual rule at c->idx. Returns the input length on success and
 * fills pos[]/match_end, or 0. */
static int ctx_try_rule(struct sh_ctx *c, const struct otl_ctx_info *ci,
                        const struct otl_ctx_rule *r, int *pos, int *match_end)
{
    struct sh_want w;
    int at = c->idx;

    if (r->ninput < 1 || r->ninput > SH_CTX) return 0;

    /* Input: position 0 is the glyph we are standing on (already matched by
     * the subtable's coverage), 1..n-1 come from the rule. */
    pos[0] = at;
    int cur = at;
    for (int i = 1; i < r->ninput; i++) {
        if (r->fmt == 1)      { w.kind = W_GLYPH; w.v = r->input[i]; }
        else if (r->fmt == 2) { w.kind = W_CLASS; w.v = r->input[i]; w.cd = ci->cd_input; }
        else                  { w.kind = W_COV;   w.cov = r->covinput[i]; }
        cur = iter_next(c, cur, &w, 1);
        if (cur < 0) return 0;
        pos[i] = cur;
    }
    *match_end = cur + 1;

    /* Lookahead. */
    cur = *match_end - 1;
    for (int i = 0; i < r->nahead; i++) {
        if (r->fmt == 1)      { w.kind = W_GLYPH; w.v = r->ahead[i]; }
        else if (r->fmt == 2) { w.kind = W_CLASS; w.v = r->ahead[i]; w.cd = ci->cd_ahead; }
        else                  { w.kind = W_COV;   w.cov = r->covahead[i]; }
        cur = iter_next(c, cur, &w, 0);
        if (cur < 0) return 0;
    }

    /* Backtrack, nearest first, which is the order the font stores it in. */
    cur = at;
    for (int i = 0; i < r->nback; i++) {
        if (r->fmt == 1)      { w.kind = W_GLYPH; w.v = r->back[i]; }
        else if (r->fmt == 2) { w.kind = W_CLASS; w.v = r->back[i]; w.cd = ci->cd_back; }
        else                  { w.kind = W_COV;   w.cov = r->covback[i]; }
        cur = iter_prev(c, cur, &w, 0);
        if (cur < 0) return 0;
    }
    return r->ninput;
}

static int ctx_apply(struct sh_ctx *c, uint32_t sub, int chain,
                     int (*run)(struct sh_ctx *, int, int))
{
    struct otl_ctx_info ci;
    if (otl_ctx_open(c->t, sub, chain, &ci) != 0) return 0;

    uint16_t gid = c->g[c->idx].gid;
    int set;
    if (ci.fmt == 1) {
        set = otl_coverage_index(c->t, ci.coverage, gid);
        if (set < 0) return 0;
    } else if (ci.fmt == 2) {
        if (otl_coverage_index(c->t, ci.coverage, gid) < 0) return 0;
        set = otl_class_of(c->t, ci.cd_input, gid);
    } else {
        set = 0;
    }
    if (set < 0 || set >= ci.nsets) return 0;

    int nr = otl_ctx_rule_count(c->t, &ci, sub, set);
    if (nr <= 0) return 0;

    for (int i = 0; i < nr; i++) {
        struct otl_ctx_rule r;
        if (otl_ctx_rule(c->t, &ci, sub, set, i, &r) != 0) continue;
        if (ci.fmt == 3 && r.ninput >= 1 &&
            otl_coverage_index(c->t, r.covinput[0], gid) < 0) continue;
        int pos[SH_CTX], me = 0;
        int count = ctx_try_rule(c, &ci, &r, pos, &me);
        if (!count) continue;
        ctx_records(c, count, pos, r.nrec, r.rec, me, run);
        return 1;
    }
    return 0;
}

/* Assign ligature ids so that a mark knows which component of a ligature it
 * belongs to. Ids start at 1; 0 means "not part of a ligature". */
static int next_lig_id(struct sh_ctx *c)
{
    c->lig_serial = (c->lig_serial + 1) & 0x1F;
    if (!c->lig_serial) c->lig_serial = 1;
    return c->lig_serial;
}

static int gsub_subtable(struct sh_ctx *c, int type, uint32_t sub)
{
    int i = c->idx;
    uint16_t gid = c->g[i].gid;

    switch (type) {
    case OTL_GSUB_SINGLE: {
        int r = otl_gsub_single(c->t, sub, gid);
        if (r < 0) return 0;
        buf_set_glyph(c, i, (uint16_t)r, 0);
        c->idx = i + 1;
        return 1;
    }
    case OTL_GSUB_MULTIPLE: {
        uint16_t out[SH_CTX];
        int nn = otl_gsub_multiple(c->t, sub, gid, out, SH_CTX);
        if (nn < 0) return 0;
        if (nn > SH_CTX) return 0;
        if (nn == 0) { buf_delete(c, i); return 1; }
        if (nn > 1 && buf_insert(c, i + 1, nn - 1) != 0) return 0;
        int lig_id = nn > 1 ? next_lig_id(c) : 0;
        for (int k = 0; k < nn; k++) {
            if (k) c->g[i + k] = c->g[i];
            c->g[i + k].gid = out[k];
            c->g[i + k].props = (c->g[i].props & (SH_P_SUBSTITUTED | SH_P_IGNORABLE))
                                | SH_P_SUBSTITUTED
                                | (nn > 1 ? SH_P_MULTIPLIED : 0u)
                                | gdef_props(c->p, out[k]);
            c->g[i + k].lig_id = lig_id;
            c->g[i + k].lig_comp = nn > 1 ? k + 1 : 0;
        }
        c->idx = i + nn;
        return 1;
    }
    case OTL_GSUB_ALTERNATE: {
        uint16_t alt = 0;
        int cnt = otl_gsub_alternate(c->t, sub, gid, 0, &alt);
        if (cnt <= 0) return 0;
        buf_set_glyph(c, i, alt, 0);
        c->idx = i + 1;
        return 1;
    }
    case OTL_GSUB_LIGATURE: {
        /* Gather the following glyphs the lookup is allowed to see, keeping
         * their buffer positions: skipped glyphs (marks, under IgnoreMarks)
         * stay in the buffer between the components. */
        uint16_t rest[SH_CTX];
        int pos[SH_CTX];
        int nrest = 0, cur = i;
        struct sh_want any = { W_ANY, 0, 0, 0 };
        while (nrest < SH_CTX - 1) {
            int nx = iter_next(c, cur, &any, 1);
            if (nx < 0) break;
            rest[nrest] = c->g[nx].gid;
            pos[nrest + 1] = nx;
            nrest++; cur = nx;
        }
        int consumed = 0;
        int lig = otl_gsub_ligature(c->t, sub, gid, rest, nrest, &consumed);
        if (lig < 0 || consumed < 1) return 0;
        pos[0] = i;
        if (consumed == 1) { buf_set_glyph(c, i, (uint16_t)lig, SH_P_LIGATED); c->idx = i + 1; return 1; }

        /* Total component count, so a mark can be attached to the right part. */
        int total = 0;
        for (int k = 0; k < consumed; k++) {
            int lc = c->g[pos[k]].lig_id ? c->g[pos[k]].lig_comp : 0;
            (void)lc;
            total += c->g[pos[k]].lig_id && (c->g[pos[k]].props & SH_P_LIGATURE)
                     ? (c->g[pos[k]].lig_comp ? c->g[pos[k]].lig_comp : 1) : 1;
        }
        int lig_id = c->g[i].lig_id ? c->g[i].lig_id : next_lig_id(c);
        int cluster = c->g[i].cluster;
        for (int k = 1; k < consumed; k++)
            if (c->g[pos[k]].cluster < cluster) cluster = c->g[pos[k]].cluster;

        buf_set_glyph(c, i, (uint16_t)lig, SH_P_LIGATED);
        c->g[i].lig_id = lig_id;
        c->g[i].lig_comp = 0;
        c->g[i].cluster = cluster;

        /* Marks that sat between components become components of the ligature. */
        int comp_so_far = 1;
        for (int k = 1; k < consumed; k++) {
            for (int j = pos[k - 1] + 1; j < pos[k]; j++) {
                c->g[j].lig_id = lig_id;
                c->g[j].lig_comp = comp_so_far;
            }
            comp_so_far++;
        }
        (void)total;
        /* Delete the component glyphs, back to front so the indices hold. */
        for (int k = consumed - 1; k >= 1; k--) buf_delete(c, pos[k]);
        c->idx = i + 1;
        return 1;
    }
    case OTL_GSUB_CONTEXT: return ctx_apply(c, sub, 0, gsub_recurse);
    case OTL_GSUB_CHAIN:   return ctx_apply(c, sub, 1, gsub_recurse);
    case OTL_GSUB_REVERSE_CHAIN: {
        struct otl_revchain rc;
        if (otl_revchain(c->t, sub, &rc) != 0) return 0;
        if (otl_coverage_index(c->t, rc.coverage, gid) < 0) return 0;
        struct sh_want w;
        int cur = i;
        for (int k = 0; k < rc.nback; k++) {
            w.kind = W_COV; w.cov = rc.back[k];
            cur = iter_prev(c, cur, &w, 0);
            if (cur < 0) return 0;
        }
        cur = i;
        for (int k = 0; k < rc.nahead; k++) {
            w.kind = W_COV; w.cov = rc.ahead[k];
            cur = iter_next(c, cur, &w, 0);
            if (cur < 0) return 0;
        }
        int r = otl_gsub_reverse(c->t, sub, gid);
        if (r < 0) return 0;
        buf_set_glyph(c, i, (uint16_t)r, 0);
        return 1;
    }
    default: return 0;
    }
}

/* Apply lookup `li` at position `at`. Returns 1 if it applied. */
static int gsub_lookup_at(struct sh_ctx *c, int li)
{
    struct otl_lookup l;
    if (otl_lookup_info(c->t, li, &l) != 0) return 0;
    uint32_t save = c->props;
    c->props = (uint32_t)l.flags |
               ((l.flags & OTL_LF_USE_MARK_FILTER_SET) ? ((uint32_t)l.mark_filter_set << 16) : 0);
    int r = 0;
    for (int s = 0; s < l.nsub; s++) {
        int type; uint32_t off;
        if (otl_subtable_type(c->t, &l, s, &type, &off) != 0) continue;
        if (gsub_subtable(c, type, off)) { r = 1; break; }
    }
    c->props = save;
    return r;
}

/* Wrapper matching the ctx_records callback signature. */
static int gsub_recurse(struct sh_ctx *c, int li, int at)
{
    int save = c->idx;
    c->idx = at;
    int r = gsub_lookup_at(c, li);
    if (!r) c->idx = save;
    return r;
}

static void gsub_stage(struct sh_ctx *c, int lo, int hi)
{
    for (int k = lo; k < hi && !c->overflow; k++) {
        struct otl_lookup l;
        if (otl_lookup_info(c->t, c->p->gsub_lk[k].idx, &l) != 0) continue;
        c->mask = c->p->gsub_lk[k].mask;
        c->props = (uint32_t)l.flags |
                   ((l.flags & OTL_LF_USE_MARK_FILTER_SET) ? ((uint32_t)l.mark_filter_set << 16) : 0);

        int reverse = 0;
        for (int s = 0; s < l.nsub; s++) {
            int type; uint32_t off;
            if (otl_subtable_type(c->t, &l, s, &type, &off) == 0 &&
                type == OTL_GSUB_REVERSE_CHAIN) { reverse = 1; break; }
        }

        if (!reverse) {
            c->idx = 0;
            while (c->idx < c->n && !c->overflow) {
                int i = c->idx;
                if ((c->g[i].mask & c->mask) && prop_ok(c, i)) {
                    c->nest = SH_NEST;
                    if (gsub_lookup_at(c, c->p->gsub_lk[k].idx)) {
                        if (c->idx <= i) c->idx = i + 1;   /* never stall */
                        continue;
                    }
                }
                c->idx = i + 1;
            }
        } else {
            for (int i = c->n - 1; i >= 0 && !c->overflow; i--) {
                if (!(c->g[i].mask & c->mask) || !prop_ok(c, i)) continue;
                c->idx = i;
                c->nest = SH_NEST;
                gsub_lookup_at(c, c->p->gsub_lk[k].idx);
            }
        }
    }
}

/* ------------------------------------------------------------------ GPOS -- */

#define AT_MARK    1u
#define AT_CURSIVE 2u

static void val_apply(struct shape_glyph *g, const struct otl_value *v, int rtl)
{
    (void)rtl;
    g->x_offset  += v->x_placement;
    g->y_offset  += v->y_placement;
    g->x_advance += v->x_advance;
    g->y_advance += v->y_advance;
}

static int val_zero(const struct otl_value *v)
{
    return !v->x_placement && !v->y_placement && !v->x_advance && !v->y_advance &&
           !v->has_device;
}

static int gpos_lookup_at(struct sh_ctx *c, int li);

static int gpos_recurse(struct sh_ctx *c, int li, int at)
{
    int save = c->idx;
    c->idx = at;
    int r = gpos_lookup_at(c, li);
    c->idx = save;
    return r;
}

static int gpos_subtable(struct sh_ctx *c, int type, uint32_t sub)
{
    int i = c->idx;
    uint16_t gid = c->g[i].gid;
    struct sh_want any = { W_ANY, 0, 0, 0 };

    switch (type) {
    case OTL_GPOS_SINGLE: {
        struct otl_value v;
        if (otl_gpos_single(c->t, sub, gid, &v) != 0) return 0;
        val_apply(&c->g[i], &v, c->rtl);
        c->idx = i + 1;
        return 1;
    }
    case OTL_GPOS_PAIR: {
        int j = iter_next(c, i, &any, 1);
        if (j < 0) return 0;
        struct otl_value v1, v2;
        if (otl_gpos_pair(c->t, sub, gid, c->g[j].gid, &v1, &v2) != 0) return 0;
        val_apply(&c->g[i], &v1, c->rtl);
        val_apply(&c->g[j], &v2, c->rtl);
        /* If the second record is empty the font declared valueFormat2 = 0, and
         * the spec leaves the cursor ON the second glyph so it can start the
         * next pair -- which is what makes "AVA" kern twice. */
        c->idx = val_zero(&v2) ? j : j + 1;
        return 1;
    }
    case OTL_GPOS_CURSIVE: {
        struct otl_anchor exit_a, entry_a, dummy;
        int he = 0, hx = 0, hd = 0;
        if (otl_gpos_cursive(c->t, sub, gid, &dummy, &hd, &exit_a, &hx) != 0) return 0;
        if (!hx) return 0;
        int j = iter_next(c, i, &any, 1);
        if (j < 0) return 0;
        struct otl_anchor dummy2;
        int hd2 = 0;
        if (otl_gpos_cursive(c->t, sub, c->g[j].gid, &entry_a, &he, &dummy2, &hd2) != 0) return 0;
        if (!he) return 0;
        int d;
        if (!c->rtl) {
            c->g[i].x_advance = exit_a.x + c->g[i].x_offset;
            d = entry_a.x + c->g[j].x_offset;
            c->g[j].x_advance -= d;
            c->g[j].x_offset  -= d;
        } else {
            d = exit_a.x + c->g[i].x_offset;
            c->g[i].x_advance -= d;
            c->g[i].x_offset  -= d;
            c->g[j].x_advance = entry_a.x + c->g[j].x_offset;
        }
        if (c->props & OTL_LF_RIGHT_TO_LEFT) {
            c->g[i].y_offset = entry_a.y - exit_a.y;
            c->g[i].attach_type = AT_CURSIVE;
            c->g[i].attach_chain = j - i;
        } else {
            c->g[j].y_offset = exit_a.y - entry_a.y;
            c->g[j].attach_type = AT_CURSIVE;
            c->g[j].attach_chain = i - j;
        }
        c->idx = j;
        return 1;
    }
    case OTL_GPOS_MARK_BASE:
    case OTL_GPOS_MARK_LIG:
    case OTL_GPOS_MARK_MARK: {
        int j;
        if (type == OTL_GPOS_MARK_MARK) {
            /* Search back for a mark, honouring the lookup's own filters. */
            for (j = i - 1; j >= 0; j--) {
                if (!prop_ok(c, j)) continue;
                if (c->g[j].props & SH_P_IGNORABLE) continue;
                break;
            }
            if (j < 0) return 0;
            if (!(c->g[j].props & SH_P_MARK)) return 0;
            int id1 = c->g[i].lig_id, id2 = c->g[j].lig_id;
            int c1 = c->g[i].lig_comp, c2 = c->g[j].lig_comp;
            int good = 0;
            if (id1 == id2) good = (id1 == 0) || (c1 == c2);
            else good = (id1 > 0 && !c1) || (id2 > 0 && !c2);
            if (!good) return 0;
        } else {
            /* Search back for a non-mark, whatever the lookup's flags say. */
            uint32_t save = c->props;
            c->props = (c->props & ~(uint32_t)(SH_P_BASE | SH_P_LIGATURE | SH_P_MARK))
                       | SH_P_MARK;      /* i.e. IgnoreMarks */
            j = iter_prev(c, i, &any, 0);
            c->props = save;
            if (j < 0) return 0;
            if (!(c->g[j].props & SH_P_BASE) &&
                !(type == OTL_GPOS_MARK_LIG && (c->g[j].props & SH_P_LIGATURE)))
                return 0;
        }

        int comp = 0;
        if (type == OTL_GPOS_MARK_LIG) {
            int nc = otl_gpos_lig_components(c->t, sub, c->g[j].gid);
            if (nc < 0) return 0;
            comp = nc - 1;
            if (c->g[i].lig_id && c->g[i].lig_id == c->g[j].lig_id && c->g[i].lig_comp > 0)
                comp = c->g[i].lig_comp - 1;
            if (comp < 0) comp = 0;
            if (nc > 0 && comp >= nc) comp = nc - 1;
        }

        struct otl_anchor ma, ba;
        int mclass = 0;
        if (otl_gpos_mark(c->t, sub, type, gid, c->g[j].gid, comp, &ma, &ba, &mclass) != 0)
            return 0;
        c->g[i].x_offset = ba.x - ma.x;
        c->g[i].y_offset = ba.y - ma.y;
        c->g[i].attach_type = AT_MARK;
        c->g[i].attach_chain = j - i;
        c->idx = i + 1;
        return 1;
    }
    case OTL_GPOS_CONTEXT: return ctx_apply(c, sub, 0, gpos_recurse);
    case OTL_GPOS_CHAIN:   return ctx_apply(c, sub, 1, gpos_recurse);
    default: return 0;
    }
}

static int gpos_lookup_at(struct sh_ctx *c, int li)
{
    struct otl_lookup l;
    if (otl_lookup_info(c->t, li, &l) != 0) return 0;
    uint32_t save = c->props;
    c->props = (uint32_t)l.flags |
               ((l.flags & OTL_LF_USE_MARK_FILTER_SET) ? ((uint32_t)l.mark_filter_set << 16) : 0);
    int r = 0;
    for (int s = 0; s < l.nsub; s++) {
        int type; uint32_t off;
        if (otl_subtable_type(c->t, &l, s, &type, &off) != 0) continue;
        if (gpos_subtable(c, type, off)) { r = 1; break; }
    }
    c->props = save;
    return r;
}

static void propagate_attach(struct shape_glyph *g, int n, int i, int rtl, int depth)
{
    if (depth <= 0) return;
    int chain = g[i].attach_chain;
    uint32_t type = g[i].attach_type;
    if (!chain) return;
    g[i].attach_chain = 0;
    int j = i + chain;
    if (j < 0 || j >= n) return;
    propagate_attach(g, n, j, rtl, depth - 1);
    if (type & AT_CURSIVE) {
        g[i].y_offset += g[j].y_offset;
    } else if (type & AT_MARK) {
        g[i].x_offset += g[j].x_offset;
        g[i].y_offset += g[j].y_offset;
        if (!rtl) {
            for (int k = j; k < i; k++) { g[i].x_offset -= g[k].x_advance; g[i].y_offset -= g[k].y_advance; }
        } else {
            for (int k = j + 1; k < i + 1; k++) { g[i].x_offset += g[k].x_advance; g[i].y_offset += g[k].y_advance; }
        }
    }
}

/* ------------------------------------------------------------- shape_run -- */

static int glyph_of(const struct ttf_font *f, uint32_t cp)
{
    int g = ttf_glyph_id(f, cp);
    return g < 0 ? 0 : g;
}

int shape_run(const struct ttf_font *f, const uint32_t *cps, int n,
              int script, int rtl, struct shape_glyph *buf, int cap)
{
    if (!f || !cps || n < 0) return -1;
    if (n == 0) return 0;
    if (cap < n) return -1;
    int upem = f->units_per_em > 0 ? f->units_per_em : 1000;
    (void)upem;

#ifdef SHAPE_NEGATIVE_CONTROL
    /* The negative control: what the text layer did before this file existed.
     * One glyph per code point straight out of cmap, advances summed, no GSUB
     * and no GPOS -- so Arabic comes out as disconnected isolated letters and
     * every kerned pair is a pixel or two wide. The HarfBuzz differential has
     * to reject this. */
    for (int i = 0; i < n; i++) {
        buf[i].gid = (uint16_t)glyph_of(f, cps[i]);
        buf[i].cp = cps[i];
        buf[i].cluster = i;
        buf[i].x_advance = ttf_advance(f, buf[i].gid);
        buf[i].y_advance = buf[i].x_offset = buf[i].y_offset = 0;
        buf[i].mask = 1; buf[i].props = 0;
        buf[i].lig_id = buf[i].lig_comp = 0;
        buf[i].attach_chain = 0; buf[i].attach_type = 0;
    }
    if (rtl) {
        for (int a = 0, b = n - 1; a < b; a++, b--) {
            struct shape_glyph t = buf[a]; buf[a] = buf[b]; buf[b] = t;
        }
    }
    (void)script;
    return n;
#else
    struct sh_plan plan;
    plan_build(&plan, f, script);

    /* Joining forms, before anything touches the buffer: the state machine
     * reads code points, not glyphs. */
    uint8_t forms[512];
    int nform = n < 512 ? n : 512;
    if (plan.cursive) {
        script_arabic_joining(cps, nform, forms);
        for (int i = nform; i < n; i++) forms[0] = forms[0];   /* not reached */
    }

    /* Map code points to glyphs. RTL runs get mirrored characters first, which
     * is UAX #9's L4 done where the font can be consulted: use the mirrored
     * glyph if the font has it, and let the 'rtlm' feature handle it if not. */
    for (int i = 0; i < n; i++) {
        uint32_t cp = cps[i];
        uint32_t mask = plan.global_mask;
        int gid;
        if (rtl) {
            uint32_t m = bidi_mirror_cp(cp);
            if (m != cp) {
                int mg = glyph_of(f, m);
                if (mg) { cp = m; }
                else    { mask |= plan.rtlm_mask; }
            }
        }
        gid = glyph_of(f, cp);
        if (plan.cursive && i < nform && forms[i] < AJ_NFORM)
            mask |= plan.form_mask[forms[i]];

        buf[i].gid = (uint16_t)gid;
        buf[i]._pad = 0;
        buf[i].cp = cps[i];
        buf[i].cluster = i;
        buf[i].x_advance = buf[i].y_advance = 0;
        buf[i].x_offset = buf[i].y_offset = 0;
        buf[i].mask = mask;
        buf[i].props = gdef_props(&plan, (uint16_t)gid) |
                       (is_ignorable(cps[i]) ? SH_P_IGNORABLE : 0u);
        buf[i].lig_id = buf[i].lig_comp = 0;
        buf[i].attach_chain = 0;
        buf[i].attach_type = 0;
    }

    struct sh_ctx c;
    c.f = f; c.p = &plan; c.g = buf; c.n = n; c.cap = cap;
    c.idx = 0; c.mask = 1; c.props = 0; c.nest = SH_NEST;
    c.is_gpos = 0; c.rtl = rtl ? 1 : 0; c.lig_serial = 0; c.overflow = 0;

    if (plan.has_gsub) {
        c.t = &plan.gsub;
        int lo = 0;
        for (int s = 0; s < plan.n_stage; s++) {
            gsub_stage(&c, lo, plan.gsub_stage_end[s]);
            lo = plan.gsub_stage_end[s];
        }
    }
    if (c.overflow) return -1;

    /* Default advances, then GPOS on top. */
    for (int i = 0; i < c.n; i++) {
        c.g[i].x_advance = ttf_advance(f, c.g[i].gid);
        c.g[i].y_advance = 0;
    }

    if (plan.has_gpos && plan.n_gpos_lk > 0) {
        c.is_gpos = 1;
        c.t = &plan.gpos;
        for (int k = 0; k < plan.n_gpos_lk; k++) {
            struct otl_lookup l;
            if (otl_lookup_info(c.t, plan.gpos_lk[k].idx, &l) != 0) continue;
            c.mask = plan.gpos_lk[k].mask;
            c.idx = 0;
            while (c.idx < c.n) {
                int i = c.idx;
                c.props = (uint32_t)l.flags |
                          ((l.flags & OTL_LF_USE_MARK_FILTER_SET) ? ((uint32_t)l.mark_filter_set << 16) : 0);
                if ((c.g[i].mask & c.mask) && prop_ok(&c, i)) {
                    c.nest = SH_NEST;
                    if (gpos_lookup_at(&c, plan.gpos_lk[k].idx)) {
                        if (c.idx <= i) c.idx = i + 1;
                        continue;
                    }
                }
                c.idx = i + 1;
            }
        }
        for (int i = 0; i < c.n; i++) propagate_attach(c.g, c.n, i, c.rtl, 16);
    } else if (plan.use_legacy_kern) {
        /* Fonts that carry kerning only in the old `kern` table -- DejaVu's
         * ancestors, most Type-1 ports. Applied between adjacent non-mark
         * glyphs, which is all format 0 can express. */
        for (int i = 0; i + 1 < c.n; i++) {
            if (c.g[i].props & SH_P_MARK) continue;
            int j = i + 1;
            while (j < c.n && (c.g[j].props & SH_P_MARK)) j++;
            if (j >= c.n) break;
            c.g[i].x_advance += otl_kern_pair(&plan.kern, c.g[i].gid, c.g[j].gid);
        }
    }

    /* Marks carry no advance of their own once GPOS has placed them. */
    if (plan.has_gdef_classes)
        for (int i = 0; i < c.n; i++)
            if (c.g[i].props & SH_P_MARK) { c.g[i].x_advance = 0; c.g[i].y_advance = 0; }

    /* Hide the default ignorables that survived: they become the space glyph,
     * keeping whatever advance they had (normally zero). */
    int space = glyph_of(f, ' ');
    if (space)
        for (int i = 0; i < c.n; i++)
            if ((c.g[i].props & SH_P_IGNORABLE) && !(c.g[i].props & SH_P_SUBSTITUTED))
                c.g[i].gid = (uint16_t)space;

    if (rtl)
        for (int a = 0, b = c.n - 1; a < b; a++, b--) {
            struct shape_glyph t = c.g[a]; c.g[a] = c.g[b]; c.g[b] = t;
        }
    return c.n;
#endif
}

int shape_run_width(const struct ttf_font *f, const uint32_t *cps, int n,
                    int script, int rtl, struct shape_glyph *buf, int cap)
{
    int ng = shape_run(f, cps, n, script, rtl, buf, cap);
    if (ng < 0) return -1;
    int w = 0;
    for (int i = 0; i < ng; i++) w += buf[i].x_advance;
    return w;
}

/* ------------------------------------------------------------ whole lines -- */

#include "utf8.h"

static int sc_scale(int v, int px, int upem)
{
    if (upem <= 0) return 0;
    /* Round toward zero on both signs, which is what the old text.c did for
     * advances; keeping it identical means the shaping change cannot move a
     * pure-Latin layout by a pixel for a reason unrelated to shaping. */
    return (int)(((long)v * (long)px) / (long)upem);
}

/* Which font in the set has a glyph for this code point?  First match wins;
 * a code point no font covers goes to font 0 so it renders as .notdef there
 * rather than disappearing. */
static int font_for(const struct shape_font_set *fs, uint32_t cp)
{
    for (int i = 0; i < fs->n && i < SHAPE_MAX_FONTS; i++)
        if (fs->f[i] && ttf_glyph_id(fs->f[i], cp) > 0) return i;
    return 0;
}

struct fsel_ctx { const struct shape_font_set *fs; };
static int fsel(uint32_t cp, void *ud)
{
    return font_for(((struct fsel_ctx *)ud)->fs, cp);
}

int shape_line(const struct shape_font_set *fs, const char *utf8, int len,
               int px, int cell, int x0, const struct shape_emit *em,
               struct shape_scratch *sc)
{
    if (!fs || fs->n <= 0 || !fs->f[0] || !utf8 || len <= 0 || !sc) return x0;

    /* 1. Decode. */
    int n = 0;
    const char *p = utf8, *e = utf8 + len;
    while (p < e && n < sc->ncp_cap) {
        uint32_t cp;
        const char *q = utf8_next(p, &cp);
        if (q <= p) break;
        p = q;
        if (!cp) break;
        sc->cps[n++] = cp;
    }
    if (n == 0) return x0;

    int x = x0;

    /* The terminal's fixed grid: no shaping, one cell per code point. See the
     * note in shape.h -- a cell grid and a ligature cannot both be honoured. */
    if (cell > 0) {
        for (int i = 0; i < n; i++) {
            int fi = font_for(fs, sc->cps[i]);
            const struct ttf_font *f = fs->f[fi];
            int gid = ttf_glyph_id(f, sc->cps[i]);
            if (gid < 0) gid = 0;
            int adv = sc_scale(ttf_advance(f, gid), px, f->units_per_em);
            int w = (adv > cell * 3 / 2) ? cell * 2 : cell;
            if (em && em->glyph) em->glyph(em->ud, fi, gid, x, 0);
            x += w;
        }
        return x;
    }

    /* 2. Bidi. */
    int para = 0;
    if (bidi_is_trivial(sc->cps, n)) {
        for (int i = 0; i < n; i++) sc->levels[i] = 0;
    } else {
        para = bidi_resolve(sc->cps, n, BIDI_DIR_AUTO, sc->levels, sc->bidi, sc->bidi_cap);
        if (para < 0) for (int i = 0; i < n; i++) sc->levels[i] = 0;
    }

    /* 3. Segment into runs of one script, one direction, one font. */
    struct fsel_ctx fc = { fs };
    int nrun = script_runs(sc->cps, n, sc->levels, fsel, &fc, sc->runs, sc->nrun_cap);
    if (nrun > sc->nrun_cap) nrun = sc->nrun_cap;
    if (nrun <= 0) return x;

    /* 4. Visual order of the runs: reorder the characters, then take the runs
     * in the order their characters first appear. That reuses bidi_reorder
     * rather than reimplementing L2 over runs. */
    bidi_reorder(sc->levels, n, sc->order);

    for (int k = 0; k < nrun; k++) sc->runs[k].vis = n;
    for (int vi = 0; vi < n; vi++) {
        int ci = sc->order[vi];
        for (int k = 0; k < nrun; k++)
            if (ci >= sc->runs[k].start && ci < sc->runs[k].start + sc->runs[k].len) {
                if (vi < sc->runs[k].vis) sc->runs[k].vis = vi;
                break;
            }
    }

    /* 5. Shape and place, left to right. Two runs cannot share a first visual
     * position, so "the smallest vis greater than the last one" is a total
     * order over the runs. */
    int last = -1;
    for (int done = 0; done < nrun; done++) {
        int best = -1;
        for (int k = 0; k < nrun; k++)
            if (sc->runs[k].vis > last &&
                (best < 0 || sc->runs[k].vis < sc->runs[best].vis))
                best = k;
        if (best < 0) break;
        last = sc->runs[best].vis;

        const struct text_run *r = &sc->runs[best];
        const struct ttf_font *f = fs->f[r->font];
        if (!f) continue;
        int ng = shape_run(f, sc->cps + r->start, r->len, r->script, r->rtl,
                           sc->glyphs, sc->nglyph_cap);
        if (ng < 0) continue;

        int upem = f->units_per_em;
        for (int gi = 0; gi < ng; gi++) {
            if (em && em->glyph) {
                int xo = sc_scale(sc->glyphs[gi].x_offset, px, upem);
                int yo = sc_scale(sc->glyphs[gi].y_offset, px, upem);
                em->glyph(em->ud, r->font, sc->glyphs[gi].gid, x + xo, yo);
            }
            x += sc_scale(sc->glyphs[gi].x_advance, px, upem);
        }
    }
    return x;
}
