#include "otlayout.h"
#include "fontrd.h"

/* OpenType Layout table access. See otlayout.h for the contract; this file is
 * pure binary reading with every offset bounds-checked through fontrd.h.
 *
 * Two habits run through it:
 *  - Counts that come out of the font are clamped to what the blob can actually
 *    hold before they are used as loop bounds, so a crafted count cannot make
 *    us walk off the end even though fr_* would return zeroes if it did.
 *  - "Returns the true count, writes at most cap" everywhere, so a caller can
 *    size a buffer with one call and never be handed a silently short list. */

static struct fr FR(const struct otl_table *t) { struct fr b = { t->data, t->len }; return b; }

/* How many `stride`-byte records can start at `off` inside the blob. */
static uint32_t fit(const struct fr *b, uint32_t off, uint32_t stride)
{
    if (!b->d || off >= b->len || !stride) return 0;
    return (b->len - off) / stride;
}

static uint32_t clampn(uint32_t n, uint32_t max) { return n > max ? max : n; }

int otl_open(const uint8_t *data, uint32_t len, uint32_t table_off, int is_gpos,
             struct otl_table *t)
{
    struct fr b = { data, len };
    if (!data || !table_off || !fr_ok(&b, table_off, 10)) return -1;
    t->data = data; t->len = len; t->off = table_off; t->is_gpos = is_gpos;
    t->major = (uint16_t)fr_u16(&b, table_off);
    t->minor = (uint16_t)fr_u16(&b, table_off + 2);
    if (t->major != 1) return -1;
    t->scripts  = fr_off16(&b, table_off, table_off + 4);
    t->features = fr_off16(&b, table_off, table_off + 6);
    t->lookups  = fr_off16(&b, table_off, table_off + 8);
    t->feature_vars = 0;
    if (t->minor >= 1 && fr_ok(&b, table_off + 10, 4))
        t->feature_vars = fr_off32(&b, table_off, table_off + 10);
    /* A GSUB/GPOS with no lookup list is inert but not corrupt (some fonts ship
     * an empty GPOS); only a table whose lists are all bogus is rejected. */
    if (!t->scripts && !t->features && !t->lookups) return -1;
    return 0;
}

/* ------------------------------------------------------------- script list -- */

int otl_script_count(const struct otl_table *t)
{
    struct fr b = FR(t);
    if (!t->scripts) return 0;
    uint32_t n = fr_u16(&b, t->scripts);
    return (int)clampn(n, fit(&b, t->scripts + 2, 6));
}

uint32_t otl_script_tag(const struct otl_table *t, int si)
{
    struct fr b = FR(t);
    if (si < 0 || si >= otl_script_count(t)) return 0;
    return fr_u32(&b, t->scripts + 2 + (uint32_t)si * 6);
}

int otl_find_script(const struct otl_table *t, uint32_t tag)
{
    int n = otl_script_count(t);
    for (int i = 0; i < n; i++) if (otl_script_tag(t, i) == tag) return i;
    return -1;
}

/* Absolute offset of Script table `si`. */
static uint32_t script_off(const struct otl_table *t, int si)
{
    struct fr b = FR(t);
    if (si < 0 || si >= otl_script_count(t)) return 0;
    return fr_off16(&b, t->scripts, t->scripts + 2 + (uint32_t)si * 6 + 4);
}

int otl_langsys_count(const struct otl_table *t, int si)
{
    struct fr b = FR(t);
    uint32_t s = script_off(t, si);
    if (!s || !fr_ok(&b, s, 4)) return 0;
    uint32_t n = fr_u16(&b, s + 2);
    return (int)clampn(n, fit(&b, s + 4, 6));
}

uint32_t otl_langsys_tag(const struct otl_table *t, int si, int li)
{
    struct fr b = FR(t);
    uint32_t s = script_off(t, si);
    if (!s || li < 0 || li >= otl_langsys_count(t, si)) return 0;
    return fr_u32(&b, s + 4 + (uint32_t)li * 6);
}

int otl_find_langsys(const struct otl_table *t, int si, uint32_t tag)
{
    int n = otl_langsys_count(t, si);
    for (int i = 0; i < n; i++) if (otl_langsys_tag(t, si, i) == tag) return i;
    return -1;
}

/* Absolute offset of a LangSys table; li < 0 selects DefaultLangSys. */
static uint32_t langsys_off(const struct otl_table *t, int si, int li)
{
    struct fr b = FR(t);
    uint32_t s = script_off(t, si);
    if (!s) return 0;
    if (li < 0) return fr_off16(&b, s, s);              /* defaultLangSysOffset */
    if (li >= otl_langsys_count(t, si)) return 0;
    return fr_off16(&b, s, s + 4 + (uint32_t)li * 6 + 4);
}

int otl_langsys_features(const struct otl_table *t, int si, int li,
                         int *required, uint16_t *out, int cap)
{
    struct fr b = FR(t);
    uint32_t l = langsys_off(t, si, li);
    if (required) *required = -1;
    if (!l || !fr_ok(&b, l, 6)) return -1;
    uint32_t req = fr_u16(&b, l + 2);
    if (required) *required = (req == 0xFFFF) ? -1 : (int)req;
    uint32_t n = fr_u16(&b, l + 4);
    n = clampn(n, fit(&b, l + 6, 2));
    for (uint32_t i = 0; i < n && (int)i < cap; i++)
        out[i] = (uint16_t)fr_u16(&b, l + 6 + i * 2);
    return (int)n;
}

/* ------------------------------------------------------------ feature list -- */

int otl_feature_count(const struct otl_table *t)
{
    struct fr b = FR(t);
    if (!t->features) return 0;
    uint32_t n = fr_u16(&b, t->features);
    return (int)clampn(n, fit(&b, t->features + 2, 6));
}

uint32_t otl_feature_tag(const struct otl_table *t, int fi)
{
    struct fr b = FR(t);
    if (fi < 0 || fi >= otl_feature_count(t)) return 0;
    return fr_u32(&b, t->features + 2 + (uint32_t)fi * 6);
}

int otl_feature_lookups(const struct otl_table *t, int fi, uint16_t *out, int cap)
{
    struct fr b = FR(t);
    if (fi < 0 || fi >= otl_feature_count(t)) return -1;
    uint32_t f = fr_off16(&b, t->features, t->features + 2 + (uint32_t)fi * 6 + 4);
    if (!f || !fr_ok(&b, f, 4)) return -1;
    uint32_t n = fr_u16(&b, f + 2);
    n = clampn(n, fit(&b, f + 4, 2));
    for (uint32_t i = 0; i < n && (int)i < cap; i++)
        out[i] = (uint16_t)fr_u16(&b, f + 4 + i * 2);
    return (int)n;
}

/* ------------------------------------------------------------- lookup list -- */

int otl_lookup_count(const struct otl_table *t)
{
    struct fr b = FR(t);
    if (!t->lookups) return 0;
    uint32_t n = fr_u16(&b, t->lookups);
    return (int)clampn(n, fit(&b, t->lookups + 2, 2));
}

int otl_lookup_info(const struct otl_table *t, int li, struct otl_lookup *out)
{
    struct fr b = FR(t);
    if (li < 0 || li >= otl_lookup_count(t)) return -1;
    uint32_t l = fr_off16(&b, t->lookups, t->lookups + 2 + (uint32_t)li * 2);
    if (!l || !fr_ok(&b, l, 6)) return -1;
    out->off   = l;
    out->type  = (uint16_t)fr_u16(&b, l);
    out->flags = (uint16_t)fr_u16(&b, l + 2);
    uint32_t n = fr_u16(&b, l + 4);
    n = clampn(n, fit(&b, l + 6, 2));
    out->nsub = (int)n;
    out->mark_filter_set = 0;
    if (out->flags & OTL_LF_USE_MARK_FILTER_SET)
        out->mark_filter_set = (uint16_t)fr_u16(&b, l + 6 + n * 2);
    return 0;
}

uint32_t otl_subtable(const struct otl_table *t, const struct otl_lookup *l, int i)
{
    struct fr b = FR(t);
    if (!l || i < 0 || i >= l->nsub) return 0;
    return fr_off16(&b, l->off, l->off + 6 + (uint32_t)i * 2);
}

int otl_subtable_type(const struct otl_table *t, const struct otl_lookup *l, int i,
                      int *real_type, uint32_t *real_off)
{
    struct fr b = FR(t);
    uint32_t sub = otl_subtable(t, l, i);
    if (!sub) return -1;
    int ext = t->is_gpos ? OTL_GPOS_EXTENSION : OTL_GSUB_EXTENSION;
    int type = l->type;
    if (type == ext) {
        if (!fr_ok(&b, sub, 8) || fr_u16(&b, sub) != 1) return -1;
        type = (int)fr_u16(&b, sub + 2);
        if (type == ext) return -1;                 /* chained extensions: refused */
        sub = fr_off32(&b, sub, sub + 4);
        if (!sub) return -1;
    }
    *real_type = type; *real_off = sub;
    return 0;
}

/* ------------------------------------------- coverage and class definition -- */

int otl_coverage_count(const struct otl_table *t, uint32_t cov)
{
    struct fr b = FR(t);
    if (!cov || !fr_ok(&b, cov, 4)) return -1;
    uint32_t fmt = fr_u16(&b, cov), n = fr_u16(&b, cov + 2);
    if (fmt == 1) return (int)clampn(n, fit(&b, cov + 4, 2));
    if (fmt == 2) {
        n = clampn(n, fit(&b, cov + 4, 6));
        int total = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t r = cov + 4 + i * 6;
            uint32_t s = fr_u16(&b, r), e = fr_u16(&b, r + 2);
            if (e >= s) total += (int)(e - s + 1);
        }
        return total;
    }
    return -1;
}

int otl_coverage_index(const struct otl_table *t, uint32_t cov, uint16_t gid)
{
    struct fr b = FR(t);
    if (!cov || !fr_ok(&b, cov, 4)) return -1;
    uint32_t fmt = fr_u16(&b, cov), n = fr_u16(&b, cov + 2);
    if (fmt == 1) {
        n = clampn(n, fit(&b, cov + 4, 2));
        /* the array is sorted, so binary search -- coverage lookup is the single
         * hottest call in shaping and some coverages list thousands of glyphs. */
        uint32_t lo = 0, hi = n;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            uint32_t g = fr_u16(&b, cov + 4 + mid * 2);
            if (g == gid) return (int)mid;
            if (g < gid) lo = mid + 1; else hi = mid;
        }
        return -1;
    }
    if (fmt == 2) {
        n = clampn(n, fit(&b, cov + 4, 6));
        uint32_t lo = 0, hi = n;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            uint32_t r = cov + 4 + mid * 6;
            uint32_t s = fr_u16(&b, r), e = fr_u16(&b, r + 2);
            if (gid < s) { hi = mid; continue; }
            if (gid > e) { lo = mid + 1; continue; }
            return (int)(fr_u16(&b, r + 4) + (gid - s));
        }
        return -1;
    }
    return -1;
}

int otl_coverage_glyph(const struct otl_table *t, uint32_t cov, int i)
{
    struct fr b = FR(t);
    if (!cov || i < 0 || !fr_ok(&b, cov, 4)) return -1;
    uint32_t fmt = fr_u16(&b, cov), n = fr_u16(&b, cov + 2);
    if (fmt == 1) {
        n = clampn(n, fit(&b, cov + 4, 2));
        if ((uint32_t)i >= n) return -1;
        return (int)fr_u16(&b, cov + 4 + (uint32_t)i * 2);
    }
    if (fmt == 2) {
        n = clampn(n, fit(&b, cov + 4, 6));
        for (uint32_t k = 0; k < n; k++) {
            uint32_t r = cov + 4 + k * 6;
            uint32_t s = fr_u16(&b, r), e = fr_u16(&b, r + 2), sc = fr_u16(&b, r + 4);
            if (e < s) continue;
            if ((uint32_t)i >= sc && (uint32_t)i <= sc + (e - s))
                return (int)(s + ((uint32_t)i - sc));
        }
        return -1;
    }
    return -1;
}

int otl_class_of(const struct otl_table *t, uint32_t cd, uint16_t gid)
{
    struct fr b = FR(t);
    if (!cd || !fr_ok(&b, cd, 4)) return 0;
    uint32_t fmt = fr_u16(&b, cd);
    if (fmt == 1) {
        uint32_t start = fr_u16(&b, cd + 2), n = fr_u16(&b, cd + 4);
        n = clampn(n, fit(&b, cd + 6, 2));
        if (gid < start || gid >= start + n) return 0;
        return (int)fr_u16(&b, cd + 6 + ((uint32_t)gid - start) * 2);
    }
    if (fmt == 2) {
        uint32_t n = fr_u16(&b, cd + 2);
        n = clampn(n, fit(&b, cd + 4, 6));
        uint32_t lo = 0, hi = n;
        while (lo < hi) {                           /* ranges are sorted */
            uint32_t mid = lo + (hi - lo) / 2, r = cd + 4 + mid * 6;
            uint32_t s = fr_u16(&b, r), e = fr_u16(&b, r + 2);
            if (gid < s) { hi = mid; continue; }
            if (gid > e) { lo = mid + 1; continue; }
            return (int)fr_u16(&b, r + 4);
        }
        return 0;
    }
    return 0;
}

/* ------------------------------------------------------------- GSUB reads -- */

int otl_gsub_single(const struct otl_table *t, uint32_t sub, uint16_t gid)
{
    struct fr b = FR(t);
    if (!sub || !fr_ok(&b, sub, 6)) return -1;
    uint32_t fmt = fr_u16(&b, sub);
    uint32_t cov = fr_off16(&b, sub, sub + 2);
    int ci = otl_coverage_index(t, cov, gid);
    if (ci < 0) return -1;
    if (fmt == 1) return (int)(uint16_t)(gid + (int)fr_s16(&b, sub + 4));
    if (fmt == 2) {
        uint32_t n = fr_u16(&b, sub + 4);
        n = clampn(n, fit(&b, sub + 6, 2));
        if ((uint32_t)ci >= n) return -1;
        return (int)fr_u16(&b, sub + 6 + (uint32_t)ci * 2);
    }
    return -1;
}

int otl_gsub_multiple(const struct otl_table *t, uint32_t sub, uint16_t gid,
                      uint16_t *out, int cap)
{
    struct fr b = FR(t);
    if (!sub || !fr_ok(&b, sub, 6) || fr_u16(&b, sub) != 1) return -1;
    uint32_t cov = fr_off16(&b, sub, sub + 2);
    int ci = otl_coverage_index(t, cov, gid);
    if (ci < 0) return -1;
    uint32_t n = fr_u16(&b, sub + 4);
    n = clampn(n, fit(&b, sub + 6, 2));
    if ((uint32_t)ci >= n) return -1;
    uint32_t seq = fr_off16(&b, sub, sub + 6 + (uint32_t)ci * 2);
    if (!seq || !fr_ok(&b, seq, 2)) return -1;
    uint32_t cnt = fr_u16(&b, seq);
    cnt = clampn(cnt, fit(&b, seq + 2, 2));
    for (uint32_t i = 0; i < cnt && (int)i < cap; i++)
        out[i] = (uint16_t)fr_u16(&b, seq + 2 + i * 2);
    return (int)cnt;
}

int otl_gsub_alternate(const struct otl_table *t, uint32_t sub, uint16_t gid,
                       int alt, uint16_t *out)
{
    struct fr b = FR(t);
    if (!sub || !fr_ok(&b, sub, 6) || fr_u16(&b, sub) != 1) return -1;
    uint32_t cov = fr_off16(&b, sub, sub + 2);
    int ci = otl_coverage_index(t, cov, gid);
    if (ci < 0) return -1;
    uint32_t n = fr_u16(&b, sub + 4);
    n = clampn(n, fit(&b, sub + 6, 2));
    if ((uint32_t)ci >= n) return -1;
    uint32_t set = fr_off16(&b, sub, sub + 6 + (uint32_t)ci * 2);
    if (!set || !fr_ok(&b, set, 2)) return -1;
    uint32_t cnt = fr_u16(&b, set);
    cnt = clampn(cnt, fit(&b, set + 2, 2));
    if (out && cnt) {
        if (alt < 0) alt = 0;
        if ((uint32_t)alt >= cnt) alt = (int)cnt - 1;
        *out = (uint16_t)fr_u16(&b, set + 2 + (uint32_t)alt * 2);
    }
    return (int)cnt;
}

int otl_gsub_ligature(const struct otl_table *t, uint32_t sub, uint16_t first,
                      const uint16_t *rest, int nrest, int *consumed)
{
    struct fr b = FR(t);
    if (!sub || !fr_ok(&b, sub, 6) || fr_u16(&b, sub) != 1) return -1;
    uint32_t cov = fr_off16(&b, sub, sub + 2);
    int ci = otl_coverage_index(t, cov, first);
    if (ci < 0) return -1;
    uint32_t n = fr_u16(&b, sub + 4);
    n = clampn(n, fit(&b, sub + 6, 2));
    if ((uint32_t)ci >= n) return -1;
    uint32_t set = fr_off16(&b, sub, sub + 6 + (uint32_t)ci * 2);
    if (!set || !fr_ok(&b, set, 2)) return -1;
    uint32_t cnt = fr_u16(&b, set);
    cnt = clampn(cnt, fit(&b, set + 2, 2));

    int best = -1, bestlen = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t lig = fr_off16(&b, set, set + 2 + i * 2);
        if (!lig || !fr_ok(&b, lig, 4)) continue;
        uint32_t glyph = fr_u16(&b, lig);
        uint32_t comps = fr_u16(&b, lig + 2);        /* includes the first glyph */
        if (comps < 1) continue;
        uint32_t need = comps - 1;
        if (need > (uint32_t)(nrest < 0 ? 0 : nrest)) continue;
        if (!fr_ok(&b, lig + 4, need * 2)) continue;
        uint32_t k;
        for (k = 0; k < need; k++)
            if (rest[k] != (uint16_t)fr_u16(&b, lig + 4 + k * 2)) break;
        if (k != need) continue;
        /* The spec orders LigatureSets longest-first, but not every font obeys;
         * pick the longest match explicitly so a sloppy font cannot make "ffi"
         * come out as "ff" + "i". */
        if ((int)comps > bestlen) { bestlen = (int)comps; best = (int)glyph; }
    }
    if (best < 0) return -1;
    if (consumed) *consumed = bestlen;
    return best;
}

int otl_gsub_reverse(const struct otl_table *t, uint32_t sub, uint16_t gid)
{
    struct fr b = FR(t);
    if (!sub || !fr_ok(&b, sub, 4) || fr_u16(&b, sub) != 1) return -1;
    uint32_t cov = fr_off16(&b, sub, sub + 2);
    int ci = otl_coverage_index(t, cov, gid);
    if (ci < 0) return -1;
    uint32_t p = sub + 4;
    uint32_t nb = fr_u16(&b, p); p += 2;
    if (nb > fit(&b, p, 2)) return -1;
    p += nb * 2;
    uint32_t na = fr_u16(&b, p); p += 2;
    if (na > fit(&b, p, 2)) return -1;
    p += na * 2;
    uint32_t ng = fr_u16(&b, p); p += 2;
    ng = clampn(ng, fit(&b, p, 2));
    if ((uint32_t)ci >= ng) return -1;
    return (int)fr_u16(&b, p + (uint32_t)ci * 2);
}

int otl_revchain(const struct otl_table *t, uint32_t sub, struct otl_revchain *out)
{
    struct fr b = FR(t);
    if (!sub || !fr_ok(&b, sub, 4) || fr_u16(&b, sub) != 1) return -1;
    out->coverage = fr_off16(&b, sub, sub + 2);
    uint32_t p = sub + 4;
    uint32_t nb = fr_u16(&b, p); p += 2;
    if (nb > fit(&b, p, 2) || nb > OTL_CTX_MAX) return -1;
    out->nback = (int)nb;
    for (uint32_t i = 0; i < nb; i++) out->back[i] = fr_off16(&b, sub, p + i * 2);
    p += nb * 2;
    uint32_t na = fr_u16(&b, p); p += 2;
    if (na > fit(&b, p, 2) || na > OTL_CTX_MAX) return -1;
    out->nahead = (int)na;
    for (uint32_t i = 0; i < na; i++) out->ahead[i] = fr_off16(&b, sub, p + i * 2);
    return 0;
}

/* ------------------------------------------------------------- GPOS reads -- */

/* Size in bytes of a ValueRecord with this format. */
static uint32_t vr_size(uint32_t f)
{
    uint32_t n = 0;
    for (int i = 0; i < 8; i++) if (f & (1u << i)) n += 2;
    return n;
}

/* Read a ValueRecord at `p` with format `f`. Consumes vr_size(f) bytes. */
static void vr_read(const struct fr *b, uint32_t p, uint32_t f, struct otl_value *v)
{
    v->x_placement = v->y_placement = v->x_advance = v->y_advance = 0;
    v->has_device = 0;
    if (f & 0x0001) { v->x_placement = fr_s16(b, p); p += 2; }
    if (f & 0x0002) { v->y_placement = fr_s16(b, p); p += 2; }
    if (f & 0x0004) { v->x_advance   = fr_s16(b, p); p += 2; }
    if (f & 0x0008) { v->y_advance   = fr_s16(b, p); p += 2; }
    /* Device / VariationIndex offsets: recorded as "present", not applied. */
    for (int i = 4; i < 8; i++) if (f & (1u << i)) { if (fr_u16(b, p)) v->has_device = 1; p += 2; }
}

int otl_gpos_single(const struct otl_table *t, uint32_t sub, uint16_t gid,
                    struct otl_value *v)
{
    struct fr b = FR(t);
    if (!sub || !fr_ok(&b, sub, 6)) return -1;
    uint32_t fmt = fr_u16(&b, sub);
    uint32_t cov = fr_off16(&b, sub, sub + 2);
    int ci = otl_coverage_index(t, cov, gid);
    if (ci < 0) return -1;
    uint32_t vf = fr_u16(&b, sub + 4);
    if (fmt == 1) { vr_read(&b, sub + 6, vf, v); return 0; }
    if (fmt == 2) {
        uint32_t n = fr_u16(&b, sub + 6), sz = vr_size(vf);
        if (!sz) { vr_read(&b, sub + 8, vf, v); return 0; }
        n = clampn(n, fit(&b, sub + 8, sz));
        if ((uint32_t)ci >= n) return -1;
        vr_read(&b, sub + 8 + (uint32_t)ci * sz, vf, v);
        return 0;
    }
    return -1;
}

int otl_gpos_pair(const struct otl_table *t, uint32_t sub, uint16_t g1, uint16_t g2,
                  struct otl_value *v1, struct otl_value *v2)
{
    struct fr b = FR(t);
    if (!sub || !fr_ok(&b, sub, 8)) return -1;
    uint32_t fmt = fr_u16(&b, sub);
    uint32_t cov = fr_off16(&b, sub, sub + 2);
    uint32_t f1 = fr_u16(&b, sub + 4), f2 = fr_u16(&b, sub + 6);
    int ci = otl_coverage_index(t, cov, g1);
    if (ci < 0) return -1;
    uint32_t s1 = vr_size(f1), s2 = vr_size(f2);

    if (fmt == 1) {
        uint32_t n = fr_u16(&b, sub + 8);
        n = clampn(n, fit(&b, sub + 10, 2));
        if ((uint32_t)ci >= n) return -1;
        uint32_t set = fr_off16(&b, sub, sub + 10 + (uint32_t)ci * 2);
        if (!set || !fr_ok(&b, set, 2)) return -1;
        uint32_t cnt = fr_u16(&b, set), rec = 2 + s1 + s2;
        cnt = clampn(cnt, fit(&b, set + 2, rec));
        /* PairValueRecords are sorted by secondGlyph. */
        uint32_t lo = 0, hi = cnt;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2, p = set + 2 + mid * rec;
            uint32_t sg = fr_u16(&b, p);
            if (sg == g2) {
                vr_read(&b, p + 2, f1, v1);
                vr_read(&b, p + 2 + s1, f2, v2);
                return 0;
            }
            if (sg < g2) lo = mid + 1; else hi = mid;
        }
        return -1;
    }
    if (fmt == 2) {
        uint32_t cd1 = fr_off16(&b, sub, sub + 8), cd2 = fr_off16(&b, sub, sub + 10);
        uint32_t n1 = fr_u16(&b, sub + 12), n2 = fr_u16(&b, sub + 14);
        int c1 = otl_class_of(t, cd1, g1), c2 = otl_class_of(t, cd2, g2);
        if (c1 < 0 || c2 < 0 || (uint32_t)c1 >= n1 || (uint32_t)c2 >= n2) return -1;
        uint32_t rec = s1 + s2;
        uint64_t idx = (uint64_t)(uint32_t)c1 * n2 + (uint32_t)c2;
        uint64_t p = (uint64_t)sub + 16 + idx * rec;
        if (rec && (p + rec > b.len)) return -1;
        vr_read(&b, (uint32_t)p, f1, v1);
        vr_read(&b, (uint32_t)p + s1, f2, v2);
        return 0;
    }
    return -1;
}

static int anchor_read(const struct fr *b, uint32_t a, struct otl_anchor *out)
{
    if (!a || !fr_ok(b, a, 6)) return -1;
    out->fmt = (int)fr_u16(b, a);
    out->x = fr_s16(b, a + 2);
    out->y = fr_s16(b, a + 4);
    out->point = (out->fmt == 2 && fr_ok(b, a + 6, 2)) ? (int)fr_u16(b, a + 6) : -1;
    return 0;
}

int otl_gpos_cursive(const struct otl_table *t, uint32_t sub, uint16_t gid,
                     struct otl_anchor *entry, int *has_entry,
                     struct otl_anchor *exit_, int *has_exit)
{
    struct fr b = FR(t);
    *has_entry = *has_exit = 0;
    if (!sub || !fr_ok(&b, sub, 6) || fr_u16(&b, sub) != 1) return -1;
    uint32_t cov = fr_off16(&b, sub, sub + 2);
    int ci = otl_coverage_index(t, cov, gid);
    if (ci < 0) return -1;
    uint32_t n = fr_u16(&b, sub + 4);
    n = clampn(n, fit(&b, sub + 6, 4));
    if ((uint32_t)ci >= n) return -1;
    uint32_t r = sub + 6 + (uint32_t)ci * 4;
    uint32_t ea = fr_off16(&b, sub, r), xa = fr_off16(&b, sub, r + 2);
    if (anchor_read(&b, ea, entry) == 0) *has_entry = 1;
    if (anchor_read(&b, xa, exit_) == 0) *has_exit = 1;
    return 0;
}

/* MarkArray: (class, anchor) per covered mark. */
static int mark_array(const struct fr *b, uint32_t ma, int ci,
                      struct otl_anchor *anchor, int *cls)
{
    if (!ma || !fr_ok(b, ma, 2) || ci < 0) return -1;
    uint32_t n = fr_u16(b, ma);
    n = clampn(n, fit(b, ma + 2, 4));
    if ((uint32_t)ci >= n) return -1;
    uint32_t r = ma + 2 + (uint32_t)ci * 4;
    *cls = (int)fr_u16(b, r);
    return anchor_read(b, fr_off16(b, ma, r + 2), anchor);
}

int otl_gpos_lig_components(const struct otl_table *t, uint32_t sub, uint16_t lig_gid)
{
    struct fr b = FR(t);
    if (!sub || !fr_ok(&b, sub, 12) || fr_u16(&b, sub) != 1) return -1;
    uint32_t bcov = fr_off16(&b, sub, sub + 4);
    int bi = otl_coverage_index(t, bcov, lig_gid);
    if (bi < 0) return -1;
    uint32_t la = fr_off16(&b, sub, sub + 10);
    if (!la || !fr_ok(&b, la, 2)) return -1;
    uint32_t n = fr_u16(&b, la);
    n = clampn(n, fit(&b, la + 2, 2));
    if ((uint32_t)bi >= n) return -1;
    uint32_t attach = fr_off16(&b, la, la + 2 + (uint32_t)bi * 2);
    if (!attach || !fr_ok(&b, attach, 2)) return -1;
    return (int)fr_u16(&b, attach);
}

int otl_gpos_mark(const struct otl_table *t, uint32_t sub, int type,
                  uint16_t mark_gid, uint16_t base_gid, int lig_component,
                  struct otl_anchor *mark_anchor, struct otl_anchor *base_anchor,
                  int *mark_class)
{
    struct fr b = FR(t);
    if (!sub || !fr_ok(&b, sub, 12) || fr_u16(&b, sub) != 1) return -1;
    uint32_t mcov = fr_off16(&b, sub, sub + 2);
    uint32_t bcov = fr_off16(&b, sub, sub + 4);
    uint32_t nclass = fr_u16(&b, sub + 6);
    uint32_t marr = fr_off16(&b, sub, sub + 8);
    uint32_t barr = fr_off16(&b, sub, sub + 10);
    int mi = otl_coverage_index(t, mcov, mark_gid);
    int bi = otl_coverage_index(t, bcov, base_gid);
    if (mi < 0 || bi < 0 || !nclass) return -1;

    int cls = 0;
    if (mark_array(&b, marr, mi, mark_anchor, &cls) != 0) return -1;
    if (cls < 0 || (uint32_t)cls >= nclass) return -1;
    if (mark_class) *mark_class = cls;

    if (!barr || !fr_ok(&b, barr, 2)) return -1;
    uint32_t cnt = fr_u16(&b, barr);

    if (type == OTL_GPOS_MARK_LIG) {
        /* LigatureArray -> LigatureAttach -> ComponentRecord[class] */
        cnt = clampn(cnt, fit(&b, barr + 2, 2));
        if ((uint32_t)bi >= cnt) return -1;
        uint32_t attach = fr_off16(&b, barr, barr + 2 + (uint32_t)bi * 2);
        if (!attach || !fr_ok(&b, attach, 2)) return -1;
        uint32_t ncomp = fr_u16(&b, attach);
        ncomp = clampn(ncomp, fit(&b, attach + 2, nclass * 2));
        if (!ncomp) return -1;
        if (lig_component < 0) lig_component = 0;
        if ((uint32_t)lig_component >= ncomp) lig_component = (int)ncomp - 1;
        uint32_t r = attach + 2 + ((uint32_t)lig_component * nclass + (uint32_t)cls) * 2;
        return anchor_read(&b, fr_off16(&b, attach, r), base_anchor);
    }
    /* type 4 and type 6 share the BaseArray / Mark2Array shape: an anchor per
     * (glyph, class). */
    cnt = clampn(cnt, fit(&b, barr + 2, nclass * 2));
    if ((uint32_t)bi >= cnt) return -1;
    uint32_t r = barr + 2 + ((uint32_t)bi * nclass + (uint32_t)cls) * 2;
    return anchor_read(&b, fr_off16(&b, barr, r), base_anchor);
}

/* ---------------------------------------------------- contextual lookups -- */

int otl_ctx_open(const struct otl_table *t, uint32_t sub, int chain,
                 struct otl_ctx_info *ci)
{
    struct fr b = FR(t);
    if (!sub || !fr_ok(&b, sub, 4)) return -1;
    ci->chain = chain ? 1 : 0;
    ci->fmt = (int)fr_u16(&b, sub);
    ci->coverage = ci->cd_back = ci->cd_input = ci->cd_ahead = 0;
    ci->nsets = 0;
    if (ci->fmt == 1) {
        ci->coverage = fr_off16(&b, sub, sub + 2);
        uint32_t n = fr_u16(&b, sub + 4);
        ci->nsets = (int)clampn(n, fit(&b, sub + 6, 2));
        return 0;
    }
    if (ci->fmt == 2) {
        ci->coverage = fr_off16(&b, sub, sub + 2);
        if (chain) {
            ci->cd_back  = fr_off16(&b, sub, sub + 4);
            ci->cd_input = fr_off16(&b, sub, sub + 6);
            ci->cd_ahead = fr_off16(&b, sub, sub + 8);
            uint32_t n = fr_u16(&b, sub + 10);
            ci->nsets = (int)clampn(n, fit(&b, sub + 12, 2));
        } else {
            ci->cd_input = fr_off16(&b, sub, sub + 4);
            uint32_t n = fr_u16(&b, sub + 6);
            ci->nsets = (int)clampn(n, fit(&b, sub + 8, 2));
        }
        return 0;
    }
    if (ci->fmt == 3) { ci->nsets = 1; return 0; }
    return -1;
}

/* Offset of the RuleSet / ChainRuleSet array for fmt 1/2. */
static uint32_t ctx_setarr(const struct otl_ctx_info *ci, uint32_t sub)
{
    if (ci->fmt == 1) return sub + 6;
    if (ci->fmt == 2) return ci->chain ? sub + 12 : sub + 8;
    return 0;
}

int otl_ctx_rule_count(const struct otl_table *t, const struct otl_ctx_info *ci,
                       uint32_t sub, int set)
{
    struct fr b = FR(t);
    if (ci->fmt == 3) return (set == 0) ? 1 : 0;
    if (set < 0 || set >= ci->nsets) return -1;
    uint32_t arr = ctx_setarr(ci, sub);
    uint32_t rs = fr_off16(&b, sub, arr + (uint32_t)set * 2);
    if (!rs) return 0;                       /* an empty set is legal (NULL offset) */
    if (!fr_ok(&b, rs, 2)) return -1;
    uint32_t n = fr_u16(&b, rs);
    return (int)clampn(n, fit(&b, rs + 2, 2));
}

/* Read a uint16 array of `n` entries at p into out[], bounded by OTL_CTX_MAX. */
static int rd_arr(const struct fr *b, uint32_t p, uint32_t n, uint16_t *out)
{
    if (n > OTL_CTX_MAX) return -1;
    if (n && !fr_ok(b, p, n * 2)) return -1;
    for (uint32_t i = 0; i < n; i++) out[i] = (uint16_t)fr_u16(b, p + i * 2);
    return 0;
}

static int rd_covarr(const struct fr *b, uint32_t base, uint32_t p, uint32_t n,
                     uint32_t *out)
{
    if (n > OTL_CTX_MAX) return -1;
    if (n && !fr_ok(b, p, n * 2)) return -1;
    for (uint32_t i = 0; i < n; i++) out[i] = fr_off16(b, base, p + i * 2);
    return 0;
}

static int rd_recs(const struct fr *b, uint32_t p, uint32_t n, struct otl_ctx_rule *r)
{
    if (n > OTL_CTX_MAX) return -1;
    if (n && !fr_ok(b, p, n * 4)) return -1;
    for (uint32_t i = 0; i < n; i++) {
        r->rec[i].seq    = (uint16_t)fr_u16(b, p + i * 4);
        r->rec[i].lookup = (uint16_t)fr_u16(b, p + i * 4 + 2);
    }
    r->nrec = (int)n;
    return 0;
}

int otl_ctx_rule(const struct otl_table *t, const struct otl_ctx_info *ci,
                 uint32_t sub, int set, int rule, struct otl_ctx_rule *out)
{
    struct fr b = FR(t);
    out->fmt = ci->fmt; out->chain = ci->chain;
    out->nback = out->ninput = out->nahead = out->nrec = 0;

    if (ci->fmt == 3) {
        if (set != 0 || rule != 0) return -1;
        uint32_t p = sub + 2;
        if (ci->chain) {
            uint32_t nb = fr_u16(&b, p); p += 2;
            if (rd_covarr(&b, sub, p, nb, out->covback)) return -1;
            p += nb * 2;
            uint32_t ni = fr_u16(&b, p); p += 2;
            if (!ni || rd_covarr(&b, sub, p, ni, out->covinput)) return -1;
            p += ni * 2;
            uint32_t na = fr_u16(&b, p); p += 2;
            if (rd_covarr(&b, sub, p, na, out->covahead)) return -1;
            p += na * 2;
            uint32_t nr = fr_u16(&b, p); p += 2;
            if (rd_recs(&b, p, nr, out)) return -1;
            out->nback = (int)nb; out->ninput = (int)ni; out->nahead = (int)na;
        } else {
            uint32_t ni = fr_u16(&b, p); p += 2;
            uint32_t nr = fr_u16(&b, p); p += 2;
            if (!ni || rd_covarr(&b, sub, p, ni, out->covinput)) return -1;
            p += ni * 2;
            if (rd_recs(&b, p, nr, out)) return -1;
            out->ninput = (int)ni;
        }
        return 0;
    }

    int rc = otl_ctx_rule_count(t, ci, sub, set);
    if (rc < 0 || rule < 0 || rule >= rc) return -1;
    uint32_t arr = ctx_setarr(ci, sub);
    uint32_t rs = fr_off16(&b, sub, arr + (uint32_t)set * 2);
    uint32_t r = fr_off16(&b, rs, rs + 2 + (uint32_t)rule * 2);
    if (!r || !fr_ok(&b, r, 4)) return -1;

    uint32_t p = r;
    if (ci->chain) {
        uint32_t nb = fr_u16(&b, p); p += 2;
        if (rd_arr(&b, p, nb, out->back)) return -1;
        p += nb * 2;
        uint32_t ni = fr_u16(&b, p); p += 2;
        if (ni < 1 || ni > OTL_CTX_MAX) return -1;
        /* the rule's arrays omit the first glyph (it is in the coverage) */
        if (rd_arr(&b, p, ni - 1, out->input + 1)) return -1;
        p += (ni - 1) * 2;
        uint32_t na = fr_u16(&b, p); p += 2;
        if (rd_arr(&b, p, na, out->ahead)) return -1;
        p += na * 2;
        uint32_t nr = fr_u16(&b, p); p += 2;
        if (rd_recs(&b, p, nr, out)) return -1;
        out->nback = (int)nb; out->ninput = (int)ni; out->nahead = (int)na;
    } else {
        uint32_t ni = fr_u16(&b, p); p += 2;
        uint32_t nr = fr_u16(&b, p); p += 2;
        if (ni < 1 || ni > OTL_CTX_MAX) return -1;
        if (rd_arr(&b, p, ni - 1, out->input + 1)) return -1;
        p += (ni - 1) * 2;
        if (rd_recs(&b, p, nr, out)) return -1;
        out->ninput = (int)ni;
    }
    /* input[0] is implied: for fmt 1 it is the covered glyph, for fmt 2 the
     * covered glyph's input class. Only the caller knows which glyph matched,
     * so leave slot 0 as the sentinel 0xFFFF and let the shaper fill it. */
    out->input[0] = 0xFFFF;
    return 0;
}

/* ------------------------------------------------------------------ GDEF -- */

int otl_gdef_open(const uint8_t *data, uint32_t len, uint32_t off, struct otl_gdef *g)
{
    struct fr b = { data, len };
    if (!data || !off || !fr_ok(&b, off, 12)) return -1;
    g->data = data; g->len = len; g->off = off;
    g->major = (uint16_t)fr_u16(&b, off);
    g->minor = (uint16_t)fr_u16(&b, off + 2);
    if (g->major != 1) return -1;
    g->glyph_class       = fr_off16(&b, off, off + 4);
    g->attach_list       = fr_off16(&b, off, off + 6);
    g->lig_caret         = fr_off16(&b, off, off + 8);
    g->mark_attach_class = fr_off16(&b, off, off + 10);
    g->mark_glyph_sets   = (g->minor >= 2) ? fr_off16(&b, off, off + 12) : 0;
    return 0;
}

/* otl_class_of only needs data/len/off from the table, so borrow a GSUB-shaped
 * view of the GDEF blob rather than duplicating the ClassDef reader. */
static struct otl_table gdef_view(const struct otl_gdef *g)
{
    struct otl_table t;
    t.data = g->data; t.len = g->len; t.off = g->off;
    t.scripts = t.features = t.lookups = t.feature_vars = 0;
    t.major = 1; t.minor = 0; t.is_gpos = 0;
    return t;
}

int otl_gdef_class(const struct otl_gdef *g, uint16_t gid)
{
    struct otl_table t = gdef_view(g);
    return otl_class_of(&t, g->glyph_class, gid);
}

int otl_gdef_mark_attach(const struct otl_gdef *g, uint16_t gid)
{
    struct otl_table t = gdef_view(g);
    return otl_class_of(&t, g->mark_attach_class, gid);
}

int otl_gdef_mark_set_covers(const struct otl_gdef *g, int set, uint16_t gid)
{
    struct fr b = { g->data, g->len };
    struct otl_table t = gdef_view(g);
    uint32_t ms = g->mark_glyph_sets;
    if (!ms || !fr_ok(&b, ms, 4) || fr_u16(&b, ms) != 1 || set < 0) return -1;
    uint32_t n = fr_u16(&b, ms + 2);
    n = clampn(n, fit(&b, ms + 4, 4));
    if ((uint32_t)set >= n) return -1;
    uint32_t cov = fr_off32(&b, ms, ms + 4 + (uint32_t)set * 4);
    return otl_coverage_index(&t, cov, gid) >= 0 ? 1 : 0;
}

int otl_gdef_lig_carets(const struct otl_gdef *g, uint16_t gid, int *out, int cap)
{
    struct fr b = { g->data, g->len };
    struct otl_table t = gdef_view(g);
    uint32_t lc = g->lig_caret;
    if (!lc || !fr_ok(&b, lc, 4)) return 0;
    uint32_t cov = fr_off16(&b, lc, lc);
    int ci = otl_coverage_index(&t, cov, gid);
    if (ci < 0) return 0;
    uint32_t n = fr_u16(&b, lc + 2);
    n = clampn(n, fit(&b, lc + 4, 2));
    if ((uint32_t)ci >= n) return 0;
    uint32_t lg = fr_off16(&b, lc, lc + 4 + (uint32_t)ci * 2);
    if (!lg || !fr_ok(&b, lg, 2)) return 0;
    uint32_t cnt = fr_u16(&b, lg);
    cnt = clampn(cnt, fit(&b, lg + 2, 2));
    for (uint32_t i = 0; i < cnt && (int)i < cap; i++) {
        uint32_t cv = fr_off16(&b, lg, lg + 2 + i * 2);
        int fmt = cv ? (int)fr_u16(&b, cv) : 0;
        if (fmt == 1)      out[i] = fr_s16(&b, cv + 2);          /* coordinate */
        else if (fmt == 2) out[i] = -(int)fr_u16(&b, cv + 2) - 1; /* contour point */
        else if (fmt == 3) out[i] = fr_s16(&b, cv + 2);          /* device: value only */
        else               out[i] = 0;
    }
    return (int)cnt;
}

/* ------------------------------------------------------------------ kern -- */

int otl_kern_open(const uint8_t *data, uint32_t len, uint32_t off, struct otl_kern *k)
{
    struct fr b = { data, len };
    if (!data || !off || !fr_ok(&b, off, 4)) return -1;
    k->data = data; k->len = len; k->off = off;
    uint32_t v = fr_u16(&b, off);
    if (v == 0) {                                  /* TrueType kern 0 */
        k->apple = 0;
        k->ntables = (int)fr_u16(&b, off + 2);
    } else if (v == 1 && fr_u16(&b, off + 2) == 0) { /* Apple kern 1.0 */
        if (!fr_ok(&b, off, 8)) return -1;
        k->apple = 1;
        k->ntables = (int)fr_u32(&b, off + 4);
    } else return -1;
    if (k->ntables < 0 || k->ntables > 64) k->ntables = 0;   /* absurd count */
    return k->ntables ? 0 : -1;
}

int otl_kern_pair(const struct otl_kern *k, uint16_t left, uint16_t right)
{
    struct fr b = { k->data, k->len };
    uint32_t p = k->off + (k->apple ? 8u : 4u);
    int total = 0;
    uint32_t key = ((uint32_t)left << 16) | right;
    for (int i = 0; i < k->ntables; i++) {
        uint32_t length, coverage, fmt, body;
        if (k->apple) {
            if (!fr_ok(&b, p, 8)) break;
            length = fr_u32(&b, p);
            coverage = fr_u16(&b, p + 4);
            fmt = coverage & 0x00FF;                 /* Apple: format in the low byte */
            /* Apple coverage bit 15 = vertical, 14 = cross-stream, 13 = variation */
            if (coverage & 0xE000) { if (length < 8) break; p += length; continue; }
            body = p + 8;
        } else {
            if (!fr_ok(&b, p, 6)) break;
            length = fr_u16(&b, p + 2);
            coverage = fr_u16(&b, p + 4);
            fmt = (coverage >> 8) & 0xFF;
            /* bit 0 horizontal, 1 minimum, 2 cross-stream, 3 override */
            if (!(coverage & 1) || (coverage & 0x000E)) {
                if (length < 6) break;
                p += length;
                continue;
            }
            body = p + 6;
        }
        if (length < (k->apple ? 8u : 6u)) break;
        if (fmt == 0 && fr_ok(&b, body, 8)) {
            uint32_t npairs = fr_u16(&b, body);
            npairs = clampn(npairs, fit(&b, body + 8, 6));
            uint32_t lo = 0, hi = npairs;
            while (lo < hi) {                        /* pairs are sorted by key */
                uint32_t mid = lo + (hi - lo) / 2, r = body + 8 + mid * 6;
                uint32_t kk = fr_u32(&b, r);
                if (kk == key) { total += fr_s16(&b, r + 4); break; }
                if (kk < key) lo = mid + 1; else hi = mid;
            }
        }
        p += length;
    }
    return total;
}
