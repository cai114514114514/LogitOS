/* ASan/UBSan fuzz + explicit rejection tests for the font parsers.
 *
 * A font is untrusted input the moment a page uses @font-face, and malformed
 * font parsing is a classic remote-code-execution surface: every structure in an
 * OpenType file is reached through an offset and a count that came out of the
 * same file. So the bar is two-sided.
 *
 *  1. REJECTION. A font that is corrupt in a way we can name must be refused, or
 *     refuse the glyph, and never hand back an outline built out of whatever
 *     happened to be next to it in memory. These are asserted individually so a
 *     regression says which check stopped working.
 *
 *  2. SURVIVAL. Every truncation of a real font, and hundreds of thousands of
 *     bit-flipped variants of it, are pushed through the whole pipeline --
 *     ttf_parse, cmap, hmtx, glyf/CFF outlines for a sweep of glyph ids, GSUB,
 *     GPOS, GDEF, kern, COLR/CPAL, CBDT, sbix -- with ASan and UBSan watching.
 *     Nothing about the OUTPUT is asserted here; a mutated font may legitimately
 *     produce nonsense. What must not happen is a read outside the buffer, a
 *     signed overflow, or a hang.
 *
 * Usage: font_fuzz FONT [FONT...]        (SCALE=n env var to go deeper)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ttf.h"
#include "otlayout.h"
#include "fontcolor.h"
#include "fontrd.h"
#include "text.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static struct fp_cmd cmdbuf[8192];
static uint8_t scratch[1 << 18];
static uint8_t covbuf[1 << 20];

/* Everything a consumer would do with a font, run for its side effects. */
static void exercise(const uint8_t *d, int len)
{
    struct ttf_font f;
    if (ttf_parse(d, len, &f) != 0) return;

    for (uint32_t cp = 0x20; cp < 0x2000; cp += 37) ttf_glyph_id(&f, cp);
    ttf_glyph_id(&f, 0x1F600);
    ttf_glyph_id(&f, 0x10FFFF);

    int n = f.num_glyphs;
    int step = n > 256 ? n / 256 : 1;
    for (int g = 0; g < n; g += step) {
        ttf_advance(&f, g);
        struct fp_path p;
        fp_init(&p, cmdbuf, (int)sizeof cmdbuf);
        ttf_glyph_path(&f, g, &p, scratch, (int)sizeof scratch);
        /* and all the way through the rasterizer, which is where a corrupt
         * outline turns into coordinates and loop bounds */
        int w, h, ox, oy;
        text_raster(&f, g, 24, covbuf, (int)sizeof covbuf, &w, &h, &ox, &oy);
        text_raster_extent(&f, g, 24, &ox, &oy, &w, &h);
    }
    /* out-of-range glyph ids must be refused, not indexed */
    struct fp_path p;
    fp_init(&p, cmdbuf, (int)sizeof cmdbuf);
    ttf_glyph_path(&f, -1, &p, scratch, (int)sizeof scratch);
    ttf_glyph_path(&f, n, &p, scratch, (int)sizeof scratch);
    ttf_glyph_path(&f, 65535, &p, scratch, (int)sizeof scratch);
    ttf_advance(&f, -1);
    ttf_advance(&f, 70000);

    struct otl_table t;
    for (int which = 0; which < 2; which++) {
        uint32_t off = which ? f.off_gpos : f.off_gsub;
        if (otl_open(d, (uint32_t)len, off, which, &t) != 0) continue;
        int ns = otl_script_count(&t);
        for (int s = 0; s < ns && s < 32; s++) {
            otl_script_tag(&t, s);
            int nl = otl_langsys_count(&t, s);
            for (int l = -1; l < nl && l < 16; l++) {
                int req; uint16_t fe[64];
                otl_langsys_features(&t, s, l, &req, fe, 64);
            }
        }
        int nf = otl_feature_count(&t);
        for (int i = 0; i < nf && i < 256; i++) {
            uint16_t lk[64];
            otl_feature_tag(&t, i);
            otl_feature_lookups(&t, i, lk, 64);
        }
        int nlk = otl_lookup_count(&t);
        for (int i = 0; i < nlk && i < 256; i++) {
            struct otl_lookup l;
            if (otl_lookup_info(&t, i, &l)) continue;
            for (int s = 0; s < l.nsub && s < 32; s++) {
                int rt; uint32_t sub;
                if (otl_subtable_type(&t, &l, s, &rt, &sub)) continue;
                for (uint16_t g = 0; g < 400; g += 7) {
                    otl_coverage_index(&t, sub + 2, g);
                    otl_class_of(&t, sub + 4, g);
                    if (!which) {
                        uint16_t out[64];
                        otl_gsub_single(&t, sub, g);
                        otl_gsub_multiple(&t, sub, g, out, 64);
                        otl_gsub_alternate(&t, sub, g, 3, out);
                        int consumed;
                        uint16_t rest[4] = { (uint16_t)(g + 1), (uint16_t)(g + 2),
                                             (uint16_t)(g + 3), (uint16_t)(g + 4) };
                        otl_gsub_ligature(&t, sub, g, rest, 4, &consumed);
                        otl_gsub_reverse(&t, sub, g);
                    } else {
                        struct otl_value v1, v2;
                        struct otl_anchor a1, a2;
                        int h1, h2, cls;
                        otl_gpos_single(&t, sub, g, &v1);
                        otl_gpos_pair(&t, sub, g, (uint16_t)(g + 1), &v1, &v2);
                        otl_gpos_cursive(&t, sub, g, &a1, &h1, &a2, &h2);
                        otl_gpos_mark(&t, sub, rt, g, (uint16_t)(g + 1), 1, &a1, &a2, &cls);
                        otl_gpos_lig_components(&t, sub, g);
                    }
                }
                struct otl_revchain rc;
                otl_revchain(&t, sub, &rc);
                for (int chain = 0; chain < 2; chain++) {
                    struct otl_ctx_info ci;
                    if (otl_ctx_open(&t, sub, chain, &ci)) continue;
                    for (int q = 0; q < ci.nsets && q < 64; q++) {
                        int cnt = otl_ctx_rule_count(&t, &ci, sub, q);
                        for (int k = 0; k < cnt && k < 32; k++) {
                            struct otl_ctx_rule rule;
                            otl_ctx_rule(&t, &ci, sub, q, k, &rule);
                        }
                    }
                }
            }
        }
    }

    struct otl_gdef gd;
    if (otl_gdef_open(d, (uint32_t)len, f.off_gdef, &gd) == 0) {
        for (uint16_t g = 0; g < 512; g += 3) {
            int carets[32];
            otl_gdef_class(&gd, g);
            otl_gdef_mark_attach(&gd, g);
            otl_gdef_mark_set_covers(&gd, 0, g);
            otl_gdef_mark_set_covers(&gd, 5, g);
            otl_gdef_lig_carets(&gd, g, carets, 32);
        }
    }
    struct otl_kern k;
    if (otl_kern_open(d, (uint32_t)len, f.off_kern, &k) == 0)
        for (uint16_t g = 0; g < 512; g += 3) otl_kern_pair(&k, g, (uint16_t)(g + 1));

    colr_version(&f);
    cpal_palette_count(&f);
    cpal_entry_count(&f);
    for (int i = 0; i < 64; i++) cpal_color(&f, i % 4, i);
    for (int g = 0; g < n && g < 512; g++) {
        struct colr_layer lay[64];
        colr_layers(&f, (uint16_t)g, lay, 64);
        struct font_bitmap bm;
        if (font_bitmap_lookup(&f, (uint16_t)g, 32, &bm) == 0 && bm.len)
            (void)bm.data[bm.len - 1];          /* touch the last byte: it must be ours */
        font_bitmap_lookup(&f, (uint16_t)g, 4096, &bm);
    }
}

/* --------------------------------------------------- named rejection cases -- */

static void must_reject(const char *what, uint8_t *d, int len)
{
    struct ttf_font f;
    CHECK(ttf_parse(d, len, &f) != 0, "%s was ACCEPTED by ttf_parse", what);
}

static void put32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* Find a table record in the directory, or 0. */
static uint8_t *rec_of(uint8_t *d, int len, const char *tag)
{
    if (len < 12) return 0;
    int n = (d[4] << 8) | d[5];
    for (int i = 0; i < n; i++) {
        uint8_t *r = d + 12 + i * 16;
        if (r + 16 > d + len) break;
        if (!memcmp(r, tag, 4)) return r;
    }
    return 0;
}

static void rejection_tests(const uint8_t *orig, int len, const char *name)
{
    uint8_t *d = malloc((size_t)len);
    struct ttf_font f;

    /* A font we have not touched must still parse -- otherwise every rejection
     * below would pass for the wrong reason. */
    memcpy(d, orig, len);
    CHECK(ttf_parse(d, len, &f) == 0, "%s: the untouched font did not parse", name);

    memcpy(d, orig, len);
    put32(d, 0x74746366);                       /* 'ttcf' */
    must_reject("a TrueType Collection (must be split first)", d, len);

    memcpy(d, orig, len);
    put32(d, 0xDEADBEEF);
    must_reject("a font with an unknown sfnt version", d, len);

    memcpy(d, orig, len);
    put16(d + 4, 0xFFFF);                       /* absurd numTables */
    ttf_parse(d, len, &f);                      /* must not crash; may parse or not */

    for (const char **t = (const char *[]){ "head", "hhea", "maxp", "hmtx", "cmap", 0 }; *t; t++) {
        memcpy(d, orig, len);
        uint8_t *r = rec_of(d, len, *t);
        if (!r) continue;
        put32(r + 8, 0x7FFFFFFF);               /* offset past the end of the file */
        char msg[64];
        snprintf(msg, sizeof msg, "a font whose %s is past EOF", *t);
        must_reject(msg, d, len);
    }

    /* An outline table that points past EOF: the font may still parse (the other
     * flavour could be present) but no glyph may be produced from it. */
    for (const char **t = (const char *[]){ "glyf", "loca", "CFF ", 0 }; *t; t++) {
        memcpy(d, orig, len);
        uint8_t *r = rec_of(d, len, *t);
        if (!r) continue;
        put32(r + 8, (uint32_t)len - 4);
        put32(r + 12, 4);
        if (ttf_parse(d, len, &f) == 0) {
            int drew = 0;
            for (int g = 0; g < f.num_glyphs && g < 64; g++) {
                struct fp_path p;
                fp_init(&p, cmdbuf, (int)sizeof cmdbuf);
                if (ttf_glyph_path(&f, g, &p, scratch, (int)sizeof scratch) == 0 && p.n > 0) drew++;
            }
            CHECK(drew == 0, "%s: %s truncated to 4 bytes still produced %d outlines",
                  name, *t, drew);
        }
    }

    /* Truncation: every prefix must either be rejected or be usable without
     * reading past its own end (ASan enforces the second half). */
    for (int n = 0; n <= len; n += (len > 4096 ? 97 : 1)) exercise(orig, n);

    free(d);
}

/* ------------------------------------------------------------------ main -- */

static uint8_t *slurp(const char *p, long *len)
{
    FILE *fp = fopen(p, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END); *len = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*len);
    if (!b || fread(b, 1, (size_t)*len, fp) != (size_t)*len) { fclose(fp); return 0; }
    fclose(fp);
    return b;
}

int main(int argc, char **argv)
{
    int scale = 1;
    const char *s = getenv("SCALE");
    if (s) scale = atoi(s);
    if (scale < 1) scale = 1;

    if (argc < 2) { printf("usage: font_fuzz FONT [FONT...]\n"); return 2; }
    srand(20260807);

    for (int a = 1; a < argc; a++) {
        long len = 0;
        uint8_t *d = slurp(argv[a], &len);
        if (!d) { printf("FAIL cannot read %s\n", argv[a]); fails++; continue; }
        printf("--- %s (%ld bytes)\n", argv[a], len);

        rejection_tests(d, (int)len, argv[a]);

        int iters = 4000 * scale;
        for (int it = 0; it < iters; it++) {              /* full-length, mutated */
            uint8_t *m = malloc((size_t)len);
            memcpy(m, d, (size_t)len);
            int k = 1 + rand() % 8;
            for (int j = 0; j < k; j++) m[rand() % len] ^= (uint8_t)(1 + rand() % 255);
            exercise(m, (int)len);
            free(m);
        }
        for (int it = 0; it < iters; it++) {              /* truncated + mutated */
            int n = 1 + rand() % (int)len;
            uint8_t *m = malloc((size_t)n);
            memcpy(m, d, (size_t)n);
            int k = 1 + rand() % 4;
            for (int j = 0; j < k; j++) m[rand() % n] ^= (uint8_t)(1 + rand() % 255);
            exercise(m, n);
            free(m);
        }
        /* Targeted: scribble on the table DIRECTORY, where every offset lives.
         * Random bit flips almost never land in those 16-byte records, and they
         * are exactly the bytes that decide where we read from. */
        int dirbytes = (int)len < 12 + 16 * 64 ? (int)len : 12 + 16 * 64;
        for (int it = 0; it < iters; it++) {
            uint8_t *m = malloc((size_t)len);
            memcpy(m, d, (size_t)len);
            int k = 1 + rand() % 6;
            for (int j = 0; j < k; j++) m[rand() % dirbytes] ^= (uint8_t)(1 + rand() % 255);
            exercise(m, (int)len);
            free(m);
        }
        free(d);
    }

    printf(fails ? "FONT FUZZ: %d FAILURES\n" : "FONT FUZZ DONE\n", fails);
    return fails ? 1 : 0;
}
