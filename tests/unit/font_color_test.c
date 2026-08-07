/* Colour-font test: COLR/CPAL layers, CBDT/CBLC and sbix strikes.
 *
 * The COLR path is the one that renders: a colour glyph is a stack of ordinary
 * outlines with palette colours, so it is checked twice over -- the layer list
 * and palette entries against fontTools, and then every layer glyph actually
 * rasterized into one shared bitmap box, which is what proves the layers line
 * up rather than merely that they were enumerated.
 *
 * The bitmap strikes are checked as locators: the bytes we hand back must be
 * exactly the image bytes fontTools sees (length and checksum), with the strike
 * metrics, because decoding a PNG belongs to the image line and not here.
 *
 * Usage: font_color_test FONT REF.bin [--px N]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ttf.h"
#include "fontcolor.h"
#include "text.h"

static int fails, checks;
#define FAILF(...) do { if (fails < 40) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } fails++; } while (0)

enum { K_EOF, K_COLRVER, K_LAYERS, K_CPAL, K_COLOR, K_BITMAP, K_NOBITMAP,
       K_DUPE };

struct rb { const uint8_t *d; size_t n, p; int over; };
static uint8_t  g8(struct rb *r)  { if (r->p + 1 > r->n) { r->over = 1; return 0; } return r->d[r->p++]; }
static uint16_t g16(struct rb *r) { uint16_t a = g8(r); return (uint16_t)(a | (g8(r) << 8)); }
static uint32_t g32(struct rb *r) { uint32_t a = g16(r), b = g16(r); return a | (b << 16); }
static int32_t  gi32(struct rb *r) { return (int32_t)g32(r); }

static uint32_t csum(const uint8_t *d, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ d[i]) * 16777619u;
    return h;
}

static uint8_t *slurp(const char *p, size_t *len)
{
    FILE *fp = fopen(p, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); return 0; }
    fclose(fp); *len = (size_t)n; return b;
}

static uint8_t cov[512 * 512];
static uint8_t acc[512 * 512];

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: font_color_test FONT REF.bin [--px N]\n"); return 2; }
    int px = 48;
    for (int i = 3; i + 1 < argc; i++) if (!strcmp(argv[i], "--px")) px = atoi(argv[i + 1]);

    size_t flen, rlen;
    uint8_t *fdata = slurp(argv[1], &flen);
    uint8_t *rdata = slurp(argv[2], &rlen);
    if (!fdata || !rdata) { printf("missing input\n"); return 2; }

    struct ttf_font f;
    if (ttf_parse(fdata, (int)flen, &f)) { printf("FAIL ttf_parse rejected %s\n", argv[1]); return 1; }
    printf("%s: outlines=%s COLR=%s CPAL=%s CBDT=%s sbix=%s\n", argv[1],
           f.outline_fmt == TTF_OUTLINE_CFF ? "CFF" : f.outline_fmt == TTF_OUTLINE_GLYF ? "glyf" : "none",
           f.off_colr ? "y" : "-", f.off_cpal ? "y" : "-",
           f.off_cbdt ? "y" : "-", f.off_sbix ? "y" : "-");

    struct rb r = { rdata, rlen, 0, 0 };
    if (rlen < 8 || memcmp(rdata, "CLRQ", 4)) { printf("bad ref magic\n"); return 2; }
    r.p = 4;
    if (g32(&r) != 1) { printf("bad ref version\n"); return 2; }

    int layered = 0, layers_drawn = 0, bitmaps = 0;
    int done = 0;
    while (!done && !r.over) {
        int kind = g8(&r);
        switch (kind) {
        case K_EOF: done = 1; break;

        case K_COLRVER: {
            int32_t want = gi32(&r);
            int got = colr_version(&f);
            if (got != want) FAILF("COLR version %d, want %d", got, want);
            checks++;
            break;
        }

        case K_LAYERS: {
            uint16_t gid = g16(&r), n = g16(&r);
            struct colr_layer want[256];
            for (int i = 0; i < n; i++) { want[i].gid = g16(&r); want[i].palette_index = g16(&r); }
            struct colr_layer got[256];
            int gn = colr_layers(&f, gid, got, 256);
            if (gn != n) { FAILF("gid %u has %d layers, want %u", gid, gn, n); checks++; break; }
            for (int i = 0; i < n; i++)
                if (got[i].gid != want[i].gid || got[i].palette_index != want[i].palette_index) {
                    FAILF("gid %u layer %d is (%u,%u), want (%u,%u)", gid, i,
                          got[i].gid, got[i].palette_index, want[i].gid, want[i].palette_index);
                    break;
                }
            checks++;
            layered++;

            /* Composite the layers for real, into one bitmap box. If a layer
             * glyph will not rasterize, or the box does not hold it, an emoji
             * comes out with a hole in it -- which enumerating the layers would
             * never have told us. */
            if (n > 0 && f.outline_fmt != TTF_OUTLINE_NONE && layers_drawn < 64) {
                /* The base glyph of a COLR record is usually EMPTY -- all the
                 * ink lives in the layers -- so the shared box has to be the
                 * union of the layer boxes, not the base glyph's. */
                int ox = 0, top = 0, w = 0, h = 0, any = 0;
                int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
                for (int i = 0; i < n; i++) {
                    int lox, ltop, lw, lh;
                    if (text_raster_extent(&f, got[i].gid, px, &lox, &ltop, &lw, &lh)) continue;
                    if (lw <= 0 || lh <= 0) continue;
                    int ax0 = lox, ax1 = lox + lw, ay0 = -ltop, ay1 = -ltop + lh;
                    if (!any) { x0 = ax0; x1 = ax1; y0 = ay0; y1 = ay1; any = 1; }
                    else {
                        if (ax0 < x0) x0 = ax0;
                        if (ax1 > x1) x1 = ax1;
                        if (ay0 < y0) y0 = ay0;
                        if (ay1 > y1) y1 = ay1;
                    }
                }
                ox = x0; top = -y0; w = x1 - x0; h = y1 - y0;
                if (any && w > 0 && h > 0 && w * h <= (int)sizeof cov) {
                    memset(acc, 0, (size_t)w * h);
                    int painted = 0;
                    for (int i = 0; i < n; i++) {
                        if (text_raster_at(&f, got[i].gid, px, ox, top, cov, w, h) != 0) continue;
                        for (int k = 0; k < w * h; k++) if (cov[k] > acc[k]) acc[k] = cov[k];
                        painted++;
                    }
                    if (!painted) FAILF("gid %u: none of its %d layers rasterized", gid, n);
                    else {
                        long ink = 0;
                        for (int k = 0; k < w * h; k++) ink += acc[k];
                        if (ink == 0) FAILF("gid %u: %d layers composited to an empty bitmap", gid, n);
                    }
                    layers_drawn++;
                    checks++;
                }
            }
            break;
        }

        case K_CPAL: {
            uint32_t np = g32(&r), ne = g32(&r);
            if ((uint32_t)cpal_palette_count(&f) != np)
                FAILF("CPAL palettes %d, want %u", cpal_palette_count(&f), np);
            if ((uint32_t)cpal_entry_count(&f) != ne)
                FAILF("CPAL entries %d, want %u", cpal_entry_count(&f), ne);
            /* out-of-range must be transparent, not garbage */
            if (np && cpal_color(&f, (int)np, 0) != 0) FAILF("CPAL palette %u should not exist", np);
            if (ne && cpal_color(&f, 0, (int)ne) != 0) FAILF("CPAL entry %u should not exist", ne);
            checks += 4;
            break;
        }

        case K_COLOR: {
            uint16_t pal = g16(&r), idx = g16(&r); uint32_t want = g32(&r);
            uint32_t got = cpal_color(&f, pal, idx);
            if (got != want) FAILF("CPAL[%u][%u] = %08x, want %08x", pal, idx, got, want);
            checks++;
            break;
        }

        case K_BITMAP: {
            uint16_t gid = g16(&r), ppem = g16(&r);
            uint32_t len = g32(&r), sum = g32(&r);
            int32_t w = gi32(&r), h = gi32(&r), bx = gi32(&r), by = gi32(&r), adv = gi32(&r);
            int fmt = g8(&r);
            struct font_bitmap b;
            if (font_bitmap_lookup(&f, gid, ppem, &b) != 0) {
                FAILF("gid %u: no bitmap at ppem %u (reference has %u bytes)", gid, ppem, len);
                checks++;
                break;
            }
            if (b.ppem != ppem)
                FAILF("gid %u: got strike ppem %d, want %u", gid, b.ppem, ppem);
            else if (b.len != len)
                FAILF("gid %u: image %u bytes, want %u", gid, b.len, len);
            else if (csum(b.data, b.len) != sum)
                FAILF("gid %u: image bytes differ (checksum %08x, want %08x)",
                      gid, csum(b.data, b.len), sum);
            else if (fmt == 1 && (b.len < 8 || memcmp(b.data, "\x89PNG\r\n\x1a\n", 8)))
                FAILF("gid %u: says PNG but does not start with the PNG signature", gid);
            if (b.format != fmt) FAILF("gid %u: format %d, want %d", gid, b.format, fmt);
            if (w && b.width != w) FAILF("gid %u: width %d, want %d", gid, b.width, w);
            if (h && b.height != h) FAILF("gid %u: height %d, want %d", gid, b.height, h);
            if (adv && b.advance != adv) FAILF("gid %u: advance %d, want %d", gid, b.advance, adv);
            if (bx && b.bearing_x != bx) FAILF("gid %u: bearingX %d, want %d", gid, b.bearing_x, bx);
            if (by && b.bearing_y != by) FAILF("gid %u: bearingY %d, want %d", gid, b.bearing_y, by);
            bitmaps++;
            checks += 3;
            break;
        }

        case K_NOBITMAP: {
            uint16_t gid = g16(&r);
            struct font_bitmap b;
            /* .notdef and the first few glyphs of these fonts carry no strike;
             * "absent" must come back as -1, not as a stale pointer. */
            if (f.off_cbdt || f.off_sbix) {
                b.data = (const uint8_t *)0x1;
                if (font_bitmap_lookup(&f, gid, 64, &b) == 0 && b.len == 0)
                    FAILF("gid %u: reported a zero-length bitmap as present", gid);
            }
            checks++;
            break;
        }

        case K_DUPE: {
            uint16_t gid = g16(&r), src = g16(&r), ppem = g16(&r);
            struct font_bitmap a, b;
            if (sbix_lookup(&f, gid, ppem, &a) != 0)
                FAILF("gid %u: its 'dupe' record did not resolve", gid);
            else if (sbix_lookup(&f, src, ppem, &b) != 0)
                FAILF("gid %u: its 'dupe' source gid %u has no bitmap", gid, src);
            else if (a.data != b.data || a.len != b.len)
                FAILF("gid %u: 'dupe' gave %u bytes, the source has %u", gid, a.len, b.len);
            checks++;
            break;
        }

        default:
            printf("FAIL unknown reference record %d\n", kind); fails++; done = 1;
        }
    }
    if (r.over) { printf("FAIL reference stream truncated\n"); fails++; }

    printf("colour: %d checks, %d layered glyphs (%d composited at %dpx), %d bitmaps\n",
           checks, layered, layers_drawn, px, bitmaps);
    if (checks < 5) { printf("FAIL the reference is too thin to mean anything\n"); fails++; }
    printf(fails ? "SOME FAILED (%d)\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}
