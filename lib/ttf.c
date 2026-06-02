#include "ttf.h"

/* From-scratch TrueType reader: table directory, head/hhea/maxp, cmap (formats
 * 4 and 12), hmtx advances, and glyph outlines (simple + composite). Big-endian
 * on the wire; we read with explicit byte ops so it works on any host. */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static int16_t  rs16(const uint8_t *p) { return (int16_t)rd16(p); }
static uint32_t rd32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static uint32_t tag4(const char *s)
{ return ((uint32_t)(uint8_t)s[0] << 24) | ((uint8_t)s[1] << 16) | ((uint8_t)s[2] << 8) | (uint8_t)s[3]; }

/* Find a table by tag; returns its offset (0 if absent). */
static uint32_t find_table(const uint8_t *d, int len, uint32_t tag)
{
    if (len < 12) return 0;
    int n = rd16(d + 4);
    const uint8_t *rec = d + 12;
    for (int i = 0; i < n; i++, rec += 16) {
        if (rec + 16 > d + len) break;
        if (rd32(rec) == tag) return rd32(rec + 8);
    }
    return 0;
}

/* Choose a Unicode cmap subtable: prefer (3,10) format 12, then (3,1) format 4,
 * then (0,*). Returns the subtable offset (absolute), or 0. */
static uint32_t pick_cmap(const uint8_t *d, int len, uint32_t cmap_off)
{
    if (!cmap_off || cmap_off + 4 > (uint32_t)len) return 0;
    const uint8_t *c = d + cmap_off;
    int n = rd16(c + 2);
    uint32_t best = 0; int best_score = -1;
    for (int i = 0; i < n; i++) {
        const uint8_t *e = c + 4 + i * 8;
        if (e + 8 > d + len) break;
        int plat = rd16(e), enc = rd16(e + 2);
        uint32_t sub = cmap_off + rd32(e + 4);
        if (sub + 2 > (uint32_t)len) continue;
        int fmt = rd16(d + sub);
        int score = -1;
        if (plat == 3 && enc == 10 && fmt == 12) score = 4;
        else if (plat == 0 && fmt == 12)         score = 3;
        else if (plat == 3 && enc == 1 && fmt == 4) score = 2;
        else if (plat == 0 && fmt == 4)          score = 1;
        if (score > best_score) { best_score = score; best = sub; }
    }
    return best;
}

int ttf_parse(const uint8_t *data, int len, struct ttf_font *f)
{
    if (len < 12) return -1;
    uint32_t ver = rd32(data);
    if (ver != 0x00010000 && ver != tag4("true") && ver != tag4("OTTO") && ver != tag4("ttcf"))
        return -1;
    if (ver == tag4("OTTO")) return -1;             /* CFF outlines unsupported */
    if (ver == tag4("ttcf")) return -1;             /* collection: extract a face first */

    f->data = data; f->len = len;
    f->off_head = find_table(data, len, tag4("head"));
    f->off_hhea = find_table(data, len, tag4("hhea"));
    f->off_maxp = find_table(data, len, tag4("maxp"));
    f->off_hmtx = find_table(data, len, tag4("hmtx"));
    f->off_loca = find_table(data, len, tag4("loca"));
    f->off_glyf = find_table(data, len, tag4("glyf"));
    f->off_cmap = find_table(data, len, tag4("cmap"));
    if (!f->off_head || !f->off_hhea || !f->off_maxp || !f->off_hmtx ||
        !f->off_loca || !f->off_glyf || !f->off_cmap) return -1;

    f->units_per_em = rd16(data + f->off_head + 18);
    f->loca_long    = rs16(data + f->off_head + 50);
    f->ascent   = rs16(data + f->off_hhea + 4);
    f->descent  = rs16(data + f->off_hhea + 6);
    f->line_gap = rs16(data + f->off_hhea + 8);
    f->num_hmetrics = rd16(data + f->off_hhea + 34);
    f->num_glyphs   = rd16(data + f->off_maxp + 4);
    f->cmap_sub = pick_cmap(data, len, f->off_cmap);
    if (!f->cmap_sub || !f->units_per_em) return -1;
    return 0;
}

int ttf_advance(const struct ttf_font *f, int gid)
{
    int i = gid < f->num_hmetrics ? gid : f->num_hmetrics - 1;
    return rd16(f->data + f->off_hmtx + i * 4);
}

/* cmap format 4 lookup (segment mapping). */
static int cmap4(const uint8_t *t, uint32_t cp)
{
    if (cp > 0xFFFF) return 0;
    int segX2 = rd16(t + 6);
    int seg = segX2 / 2;
    const uint8_t *endC = t + 14;
    const uint8_t *startC = endC + segX2 + 2;
    const uint8_t *idDelta = startC + segX2;
    const uint8_t *idRange = idDelta + segX2;
    for (int i = 0; i < seg; i++) {
        if (cp <= rd16(endC + i * 2)) {
            int start = rd16(startC + i * 2);
            if (cp < (uint32_t)start) return 0;
            int ro = rd16(idRange + i * 2);
            if (ro == 0) return (uint16_t)(cp + rs16(idDelta + i * 2));
            const uint8_t *gp = idRange + i * 2 + ro + (cp - start) * 2;
            int g = rd16(gp);
            return g ? (uint16_t)(g + rs16(idDelta + i * 2)) : 0;
        }
    }
    return 0;
}

/* cmap format 12 lookup (segmented coverage, full Unicode). */
static int cmap12(const uint8_t *t, uint32_t cp)
{
    uint32_t ngroups = rd32(t + 12);
    const uint8_t *g = t + 16;
    for (uint32_t i = 0; i < ngroups; i++, g += 12) {
        uint32_t s = rd32(g), e = rd32(g + 4);
        if (cp >= s && cp <= e) return rd32(g + 8) + (cp - s);
    }
    return 0;
}

int ttf_glyph_id(const struct ttf_font *f, uint32_t codepoint)
{
    const uint8_t *t = f->data + f->cmap_sub;
    int fmt = rd16(t);
    if (fmt == 4)  return cmap4(t, codepoint);
    if (fmt == 12) return cmap12(t, codepoint);
    return 0;
}
