#include "fontcolor.h"
#include "fontrd.h"

/* COLR/CPAL layer records and the CBDT/CBLC + sbix bitmap strikes. See
 * fontcolor.h for what is drawn versus what is merely located. */

static struct fr FR(const struct ttf_font *f)
{ struct fr b = { f->data, (uint32_t)f->len }; return b; }

/* ------------------------------------------------------------ COLR / CPAL -- */

int colr_version(const struct ttf_font *f)
{
    struct fr b = FR(f);
    if (!f->off_colr || !fr_ok(&b, f->off_colr, 14)) return -1;
    return (int)fr_u16(&b, f->off_colr);
}

int colr_layers(const struct ttf_font *f, uint16_t gid, struct colr_layer *out, int cap)
{
    struct fr b = FR(f);
    uint32_t t = f->off_colr;
    if (!t || !fr_ok(&b, t, 14)) return -1;
    uint32_t nbase = fr_u16(&b, t + 2);
    uint32_t baserec = fr_off32(&b, t, t + 4);
    uint32_t layerrec = fr_off32(&b, t, t + 8);
    uint32_t nlayer = fr_u16(&b, t + 12);
    if (!baserec || !layerrec) return -1;
    /* clamp the two counts to what the blob can actually hold */
    uint32_t maxbase = (b.len - baserec) / 6, maxlayer = (b.len - layerrec) / 4;
    if (nbase > maxbase) nbase = maxbase;
    if (nlayer > maxlayer) nlayer = maxlayer;

    /* BaseGlyphRecords are sorted by glyph id. */
    uint32_t lo = 0, hi = nbase, found = 0xFFFFFFFFu;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t g = fr_u16(&b, baserec + mid * 6);
        if (g == gid) { found = mid; break; }
        if (g < gid) lo = mid + 1; else hi = mid;
    }
    if (found == 0xFFFFFFFFu) return -1;

    uint32_t first = fr_u16(&b, baserec + found * 6 + 2);
    uint32_t n     = fr_u16(&b, baserec + found * 6 + 4);
    if (first > nlayer) return -1;
    if (n > nlayer - first) n = nlayer - first;
    for (uint32_t i = 0; i < n && (int)i < cap; i++) {
        uint32_t r = layerrec + (first + i) * 4;
        out[i].gid = (uint16_t)fr_u16(&b, r);
        out[i].palette_index = (uint16_t)fr_u16(&b, r + 2);
    }
    return (int)n;
}

int cpal_palette_count(const struct ttf_font *f)
{
    struct fr b = FR(f);
    if (!f->off_cpal || !fr_ok(&b, f->off_cpal, 12)) return 0;
    return (int)fr_u16(&b, f->off_cpal + 4);
}

int cpal_entry_count(const struct ttf_font *f)
{
    struct fr b = FR(f);
    if (!f->off_cpal || !fr_ok(&b, f->off_cpal, 12)) return 0;
    return (int)fr_u16(&b, f->off_cpal + 2);
}

uint32_t cpal_color(const struct ttf_font *f, int pal, int index)
{
    struct fr b = FR(f);
    uint32_t t = f->off_cpal;
    if (!t || !fr_ok(&b, t, 12) || pal < 0 || index < 0) return 0;
    uint32_t nentries = fr_u16(&b, t + 2);
    uint32_t npal     = fr_u16(&b, t + 4);
    uint32_t nrec     = fr_u16(&b, t + 6);
    uint32_t recs     = fr_off32(&b, t, t + 8);
    if (!recs || (uint32_t)pal >= npal || (uint32_t)index >= nentries) return 0;
    uint32_t firstidx = fr_u16(&b, t + 12 + (uint32_t)pal * 2);
    uint64_t ri = (uint64_t)firstidx + (uint32_t)index;
    if (ri >= nrec) return 0;
    uint32_t r = recs + (uint32_t)ri * 4;
    if (!fr_ok(&b, r, 4)) return 0;
    /* ColorRecord is BGRA on the wire. */
    uint32_t bl = fr_u8(&b, r), gr = fr_u8(&b, r + 1);
    uint32_t re = fr_u8(&b, r + 2), al = fr_u8(&b, r + 3);
    return (al << 24) | (re << 16) | (gr << 8) | bl;
}

/* --------------------------------------------------------------- CBDT/CBLC -- */

/* imageFormat -> the container the glyph data is in. 17/18/19 are the colour
 * (PNG) formats; 1..9 are the monochrome/greyscale EBDT formats, which we do
 * not unpack -- reporting UNKNOWN keeps a caller from treating raw bits as PNG. */
static int cbdt_imgfmt(uint32_t f) { return (f >= 17 && f <= 19) ? FONTIMG_PNG : FONTIMG_UNKNOWN; }

/* One CBLC BitmapSize record. */
struct strike {
    uint32_t idxarray;         /* absolute */
    uint32_t nsub;
    uint32_t first, last;
    int ppem;
};

static int cblc_strike(const struct ttf_font *f, uint32_t i, struct strike *s)
{
    struct fr b = FR(f);
    uint32_t t = f->off_cblc;
    uint32_t rec = t + 8 + i * 48;
    if (!fr_ok(&b, rec, 48)) return -1;
    uint32_t rel = fr_u32(&b, rec);
    if ((uint64_t)t + rel >= b.len) return -1;
    s->idxarray = t + rel;
    s->nsub  = fr_u32(&b, rec + 8);
    s->first = fr_u16(&b, rec + 40);
    s->last  = fr_u16(&b, rec + 42);
    s->ppem  = (int)fr_u8(&b, rec + 45);         /* ppemY */
    if (!s->ppem) s->ppem = (int)fr_u8(&b, rec + 44);
    if (s->nsub > (b.len - s->idxarray) / 8) s->nsub = (b.len - s->idxarray) / 8;
    return 0;
}

/* Locate glyph `gid` inside one strike: image data range + the metrics the
 * IndexSubTable/glyph record carries. */
static int cblc_find(const struct ttf_font *f, const struct strike *s, uint16_t gid,
                     struct font_bitmap *out)
{
    struct fr b = FR(f);
    if (gid < s->first || gid > s->last) return -1;
    for (uint32_t i = 0; i < s->nsub; i++) {
        uint32_t e = s->idxarray + i * 8;
        uint32_t g0 = fr_u16(&b, e), g1 = fr_u16(&b, e + 2);
        if (gid < g0 || gid > g1) continue;
        uint32_t rel = fr_u32(&b, e + 4);
        uint64_t sub64 = (uint64_t)s->idxarray + rel;
        if (sub64 + 8 > b.len) return -1;
        uint32_t sub = (uint32_t)sub64;
        uint32_t ifmt = fr_u16(&b, sub), imgfmt = fr_u16(&b, sub + 2);
        uint64_t dbase = (uint64_t)f->off_cbdt + fr_u32(&b, sub + 4);
        if (dbase >= b.len) return -1;
        uint32_t k = (uint32_t)gid - g0;
        uint64_t doff = 0, dend = 0;

        if (ifmt == 1) {
            uint32_t arr = sub + 8;
            if (!fr_ok(&b, arr, (uint32_t)(g1 - g0 + 2) * 4)) return -1;
            doff = dbase + fr_u32(&b, arr + k * 4);
            dend = dbase + fr_u32(&b, arr + (k + 1) * 4);
        } else if (ifmt == 3) {
            uint32_t arr = sub + 8;
            if (!fr_ok(&b, arr, (uint32_t)(g1 - g0 + 2) * 2)) return -1;
            doff = dbase + fr_u16(&b, arr + k * 2);
            dend = dbase + fr_u16(&b, arr + (k + 1) * 2);
        } else if (ifmt == 2) {
            uint32_t isz = fr_u32(&b, sub + 8);
            doff = dbase + (uint64_t)k * isz;
            dend = doff + isz;
            out->height = (int)fr_u8(&b, sub + 12);
            out->width  = (int)fr_u8(&b, sub + 13);
            out->bearing_x = (int)(int8_t)fr_u8(&b, sub + 14);
            out->bearing_y = (int)(int8_t)fr_u8(&b, sub + 15);
            out->advance   = (int)fr_u8(&b, sub + 16);
        } else if (ifmt == 4) {
            uint32_t ng = fr_u32(&b, sub + 8);
            if (ng > (b.len - sub - 12) / 4) return -1;
            uint32_t arr = sub + 12;
            for (uint32_t j = 0; j < ng; j++) {
                if (fr_u16(&b, arr + j * 4) != gid) continue;
                doff = dbase + fr_u16(&b, arr + j * 4 + 2);
                dend = dbase + fr_u16(&b, arr + (j + 1) * 4 + 2);
                break;
            }
            if (!dend) return -1;
        } else if (ifmt == 5) {
            uint32_t isz = fr_u32(&b, sub + 8);
            uint32_t ng = fr_u32(&b, sub + 20);
            if (ng > (b.len - sub - 24) / 2) return -1;
            uint32_t arr = sub + 24;
            uint32_t idx = 0xFFFFFFFFu;
            for (uint32_t j = 0; j < ng; j++)
                if (fr_u16(&b, arr + j * 2) == gid) { idx = j; break; }
            if (idx == 0xFFFFFFFFu) return -1;
            doff = dbase + (uint64_t)idx * isz;
            dend = doff + isz;
            out->height = (int)fr_u8(&b, sub + 12);
            out->width  = (int)fr_u8(&b, sub + 13);
            out->bearing_x = (int)(int8_t)fr_u8(&b, sub + 14);
            out->bearing_y = (int)(int8_t)fr_u8(&b, sub + 15);
            out->advance   = (int)fr_u8(&b, sub + 16);
        } else return -1;

        if (dend <= doff || dend > b.len) return -1;
        uint32_t o = (uint32_t)doff, n = (uint32_t)(dend - doff);

        /* Strip the per-glyph metrics header that formats 17/18 put in front of
         * the PNG, so the caller gets the image and nothing else. */
        uint32_t skip = 0;
        if (imgfmt == 17) {
            if (n < 5 + 4) return -1;
            out->height = (int)fr_u8(&b, o);
            out->width  = (int)fr_u8(&b, o + 1);
            out->bearing_x = (int)(int8_t)fr_u8(&b, o + 2);
            out->bearing_y = (int)(int8_t)fr_u8(&b, o + 3);
            out->advance   = (int)fr_u8(&b, o + 4);
            skip = 5;
        } else if (imgfmt == 18) {
            if (n < 8 + 4) return -1;
            out->height = (int)fr_u8(&b, o);
            out->width  = (int)fr_u8(&b, o + 1);
            out->bearing_x = (int)(int8_t)fr_u8(&b, o + 2);
            out->bearing_y = (int)(int8_t)fr_u8(&b, o + 3);
            out->advance   = (int)fr_u8(&b, o + 4);
            skip = 8;
        } else if (imgfmt == 19) {
            if (n < 4) return -1;
            skip = 0;
        }
        if (imgfmt >= 17 && imgfmt <= 19) {
            uint32_t dlen = fr_u32(&b, o + skip);
            uint32_t start = o + skip + 4;
            if (dlen > n - skip - 4) return -1;
            out->data = f->data + start;
            out->len = dlen;
        } else {
            out->data = f->data + o;
            out->len = n;
        }
        out->format = cbdt_imgfmt(imgfmt);
        out->ppem = s->ppem;
        return 0;
    }
    return -1;
}

int cbdt_lookup(const struct ttf_font *f, uint16_t gid, int want_ppem,
                struct font_bitmap *out)
{
    struct fr b = FR(f);
    if (!f->off_cblc || !f->off_cbdt || !fr_ok(&b, f->off_cblc, 8)) return -1;
    uint32_t nsizes = fr_u32(&b, f->off_cblc + 4);
    if (nsizes > (b.len - f->off_cblc - 8) / 48) nsizes = (b.len - f->off_cblc - 8) / 48;

    /* Pick the smallest strike that is at least want_ppem; if none is, the
     * largest. Scaling a bitmap down looks far better than scaling one up. */
    int best = -1, best_ppem = 0;
    for (uint32_t i = 0; i < nsizes; i++) {
        struct strike s;
        if (cblc_strike(f, i, &s)) continue;
        if (gid < s.first || gid > s.last) continue;
        if (best < 0) { best = (int)i; best_ppem = s.ppem; continue; }
        int cur_ok = best_ppem >= want_ppem, new_ok = s.ppem >= want_ppem;
        if (new_ok && (!cur_ok || s.ppem < best_ppem)) { best = (int)i; best_ppem = s.ppem; }
        else if (!new_ok && !cur_ok && s.ppem > best_ppem) { best = (int)i; best_ppem = s.ppem; }
    }
    if (best < 0) return -1;
    struct strike s;
    if (cblc_strike(f, (uint32_t)best, &s)) return -1;
    out->data = 0; out->len = 0; out->format = FONTIMG_UNKNOWN;
    out->width = out->height = out->bearing_x = out->bearing_y = out->advance = 0;
    return cblc_find(f, &s, gid, out);
}

/* ------------------------------------------------------------------ sbix -- */

int sbix_lookup(const struct ttf_font *f, uint16_t gid, int want_ppem,
                struct font_bitmap *out)
{
    struct fr b = FR(f);
    uint32_t t = f->off_sbix;
    if (!t || !fr_ok(&b, t, 8) || f->num_glyphs <= 0) return -1;
    uint32_t nstrikes = fr_u32(&b, t + 4);
    if (nstrikes > (b.len - t - 8) / 4) nstrikes = (b.len - t - 8) / 4;
    if (gid >= (uint32_t)f->num_glyphs) return -1;

    uint32_t glyphs = (uint32_t)f->num_glyphs;
    int best = -1, best_ppem = 0;
    for (uint32_t i = 0; i < nstrikes; i++) {
        uint32_t so = fr_off32(&b, t, t + 8 + i * 4);
        if (!so || !fr_ok(&b, so, 4)) continue;
        int ppem = (int)fr_u16(&b, so);
        if (best < 0) { best = (int)i; best_ppem = ppem; continue; }
        int cur_ok = best_ppem >= want_ppem, new_ok = ppem >= want_ppem;
        if (new_ok && (!cur_ok || ppem < best_ppem)) { best = (int)i; best_ppem = ppem; }
        else if (!new_ok && !cur_ok && ppem > best_ppem) { best = (int)i; best_ppem = ppem; }
    }
    if (best < 0) return -1;

    uint32_t so = fr_off32(&b, t, t + 8 + (uint32_t)best * 4);
    if (!so || !fr_ok(&b, so, 4 + (glyphs + 1) * 4)) return -1;
    uint32_t a = fr_u32(&b, so + 4 + (uint32_t)gid * 4);
    uint32_t c = fr_u32(&b, so + 4 + ((uint32_t)gid + 1) * 4);
    if (c <= a) return -1;                        /* equal offsets = no bitmap */
    uint64_t goff = (uint64_t)so + a, gend = (uint64_t)so + c;
    if (gend > b.len || gend - goff < 8) return -1;

    uint32_t o = (uint32_t)goff;
    uint32_t tag = fr_u32(&b, o + 4);
    out->bearing_x = fr_s16(&b, o);
    /* sbix gives the offset of the image's BOTTOM-left from the origin; the rest
     * of this file reports a top-left bearing, so it is converted below once the
     * height is known -- which needs the image decoded. Report what the font
     * says and let the caller adjust by the decoded height. */
    out->bearing_y = fr_s16(&b, o + 2);
    out->data = f->data + o + 8;
    out->len = (uint32_t)(gend - goff) - 8;
    out->width = out->height = 0;
    out->advance = 0;
    out->ppem = best_ppem;
    out->format = (tag == FONT_TAG('p','n','g',' ')) ? FONTIMG_PNG :
                  (tag == FONT_TAG('j','p','g',' ')) ? FONTIMG_JPEG :
                  (tag == FONT_TAG('t','i','f','f')) ? FONTIMG_TIFF : FONTIMG_UNKNOWN;
    /* 'dupe' points at another glyph's bitmap; follow it once. */
    if (tag == FONT_TAG('d','u','p','e') && out->len >= 2) {
        uint16_t other = (uint16_t)fr_u16(&b, o + 8);
        if (other != gid) return sbix_lookup(f, other, want_ppem, out);
        return -1;
    }
    return 0;
}

int font_bitmap_lookup(const struct ttf_font *f, uint16_t gid, int want_ppem,
                       struct font_bitmap *out)
{
    if (f->off_cblc && f->off_cbdt && cbdt_lookup(f, gid, want_ppem, out) == 0) return 0;
    if (f->off_sbix && sbix_lookup(f, gid, want_ppem, out) == 0) return 0;
    return -1;
}
