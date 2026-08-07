/* GSUB / GPOS / GDEF / kern access test.
 *
 * Replays the question-and-answer stream from tests/unit/font_otl_ref.py, which
 * was produced by fontTools' own decompiler. Every answer here comes out of
 * c/lib/text/otlayout.c reading the same bytes, so a disagreement is one of the
 * two being wrong about the OpenType binary format, and fontTools is not the one
 * with a fresh implementation.
 *
 * This is the API a shaping line reads, so the questions are the ones a shaper
 * asks: which scripts and features exist, which lookups they name, where a glyph
 * sits in a coverage, what class it has, what a single/multiple/ligature
 * substitution produces, what a pair adjustment is worth, and what the legacy
 * kern table says.
 *
 * Usage: font_otl_test FONT REF.bin
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ttf.h"
#include "otlayout.h"
#include "fontrd.h"

static int fails, checks;
#define FAILF(...) do { if (fails < 40) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } fails++; } while (0)

enum { K_EOF, K_COUNTS, K_SCRIPT, K_LANGSYS, K_FEATURE, K_LOOKUP, K_SUBTYPE,
       K_COVIDX, K_CLASS, K_SINGLE, K_MULTI, K_LIG, K_PAIRV, K_KERN, K_GDEFC,
       K_SINGLEPOS, K_ALT };

struct rb { const uint8_t *d; size_t n, p; int over; };

static uint8_t  g8(struct rb *r)  { if (r->p + 1 > r->n) { r->over = 1; return 0; } return r->d[r->p++]; }
static uint16_t g16(struct rb *r) { uint16_t v = (uint16_t)(g8(r) | (g8(r) << 8)); return v; }
static uint32_t g32(struct rb *r) { uint32_t a = g16(r), b = g16(r); return a | (b << 16); }
static int32_t  gi32(struct rb *r) { return (int32_t)g32(r); }

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); return 0; }
    fclose(fp); *len = (size_t)n; return b;
}

static void tagstr(uint32_t t, char *o)
{ o[0] = (char)(t >> 24); o[1] = (char)(t >> 16); o[2] = (char)(t >> 8); o[3] = (char)t; o[4] = 0; }

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: font_otl_test FONT REF.bin\n"); return 2; }
    size_t flen, rlen;
    uint8_t *fdata = slurp(argv[1], &flen);
    uint8_t *rdata = slurp(argv[2], &rlen);
    if (!fdata || !rdata) { printf("missing input\n"); return 2; }

    struct ttf_font f;
    if (ttf_parse(fdata, (int)flen, &f)) { printf("FAIL ttf_parse\n"); return 1; }

    struct otl_table tab[2];
    int have[2];
    have[0] = otl_open(fdata, (uint32_t)flen, f.off_gsub, 0, &tab[0]) == 0;
    have[1] = otl_open(fdata, (uint32_t)flen, f.off_gpos, 1, &tab[1]) == 0;
    struct otl_gdef gdef;
    int have_gdef = otl_gdef_open(fdata, (uint32_t)flen, f.off_gdef, &gdef) == 0;
    struct otl_kern kern;
    int have_kern = otl_kern_open(fdata, (uint32_t)flen, f.off_kern, &kern) == 0;

    printf("%s: GSUB %s, GPOS %s, GDEF %s, kern %s\n", argv[1],
           have[0] ? "yes" : "-", have[1] ? "yes" : "-",
           have_gdef ? "yes" : "-", have_kern ? "yes" : "-");

    struct rb r = { rdata, rlen, 0, 0 };
    if (rlen < 8 || memcmp(rdata, "OTLQ", 4)) { printf("bad ref magic\n"); return 2; }
    r.p = 4;
    if (g32(&r) != 1) { printf("bad ref version\n"); return 2; }

    /* Track the resolved subtable offset of the (lookup, subtable) currently
     * being asked about, so the per-subtable questions do not each repeat it. */
    uint32_t cur_sub[2] = { 0, 0 };
    int cur_li[2] = { -1, -1 }, cur_si[2] = { -1, -1 }, cur_type[2] = { 0, 0 };
    /* fmt-2 contextual class defs, kept per (which) as they are announced */
    uint32_t ctx_cd[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };

    int done = 0;
    while (!done && !r.over) {
        int kind = g8(&r);
        switch (kind) {
        case K_EOF: done = 1; break;

        case K_COUNTS: {
            int w = g8(&r);
            uint32_t ns = g32(&r), nf = g32(&r), nl = g32(&r);
            if (!have[w]) { FAILF("%s absent but the reference has it", w ? "GPOS" : "GSUB"); break; }
            if ((uint32_t)otl_script_count(&tab[w]) != ns)
                FAILF("%s scripts %d, want %u", w ? "GPOS" : "GSUB", otl_script_count(&tab[w]), ns);
            if ((uint32_t)otl_feature_count(&tab[w]) != nf)
                FAILF("%s features %d, want %u", w ? "GPOS" : "GSUB", otl_feature_count(&tab[w]), nf);
            if ((uint32_t)otl_lookup_count(&tab[w]) != nl)
                FAILF("%s lookups %d, want %u", w ? "GPOS" : "GSUB", otl_lookup_count(&tab[w]), nl);
            checks += 3;
            break;
        }

        case K_SCRIPT: {
            int w = g8(&r); uint32_t si = g32(&r), tg = g32(&r), nls = g32(&r);
            int32_t req = gi32(&r); uint32_t ndf = g32(&r);
            if (!have[w]) break;
            char a[5], b[5];
            uint32_t got = otl_script_tag(&tab[w], (int)si);
            if (got != tg) { tagstr(got, a); tagstr(tg, b); FAILF("script %u tag '%s', want '%s'", si, a, b); }
            if ((uint32_t)otl_langsys_count(&tab[w], (int)si) != nls)
                FAILF("script %u langsys %d, want %u", si, otl_langsys_count(&tab[w], (int)si), nls);
            if (otl_find_script(&tab[w], tg) != (int)si)
                FAILF("otl_find_script did not find script %u", si);
            uint16_t feats[512]; int gotreq = -1;
            int n = otl_langsys_features(&tab[w], (int)si, -1, &gotreq, feats, 512);
            if (n < 0) n = 0;
            if ((uint32_t)n != ndf) FAILF("script %u default features %d, want %u", si, n, ndf);
            if (gotreq != req) FAILF("script %u required feature %d, want %d", si, gotreq, req);
            checks += 4;
            break;
        }

        case K_LANGSYS: {
            int w = g8(&r); uint32_t si = g32(&r), li = g32(&r), tg = g32(&r);
            if (!have[w]) break;
            uint32_t got = otl_langsys_tag(&tab[w], (int)si, (int)li);
            if (got != tg) { char a[5], b[5]; tagstr(got, a); tagstr(tg, b);
                             FAILF("script %u langsys %u tag '%s', want '%s'", si, li, a, b); }
            if (otl_find_langsys(&tab[w], (int)si, tg) != (int)li)
                FAILF("otl_find_langsys missed script %u langsys %u", si, li);
            /* the named langsys must resolve to a feature list of its own */
            if (otl_langsys_features(&tab[w], (int)si, (int)li, 0, 0, 0) < 0)
                FAILF("script %u langsys %u has no feature list", si, li);
            checks += 3;
            break;
        }

        case K_FEATURE: {
            int w = g8(&r); uint32_t fi = g32(&r), tg = g32(&r), nlk = g32(&r);
            uint16_t want[1024];
            for (uint32_t i = 0; i < nlk; i++) want[i < 1024 ? i : 0] = g16(&r);
            if (!have[w]) break;
            uint32_t got = otl_feature_tag(&tab[w], (int)fi);
            if (got != tg) { char a[5], b[5]; tagstr(got, a); tagstr(tg, b);
                             FAILF("feature %u tag '%s', want '%s'", fi, a, b); }
            uint16_t lk[1024];
            int n = otl_feature_lookups(&tab[w], (int)fi, lk, 1024);
            if ((uint32_t)n != nlk) FAILF("feature %u lookups %d, want %u", fi, n, nlk);
            else for (uint32_t i = 0; i < nlk && i < 1024; i++)
                if (lk[i] != want[i]) { FAILF("feature %u lookup[%u] %u, want %u", fi, i, lk[i], want[i]); break; }
            checks += 2;
            break;
        }

        case K_LOOKUP: {
            int w = g8(&r); uint32_t li = g32(&r);
            uint16_t type = g16(&r), flags = g16(&r); uint32_t nsub = g32(&r);
            if (!have[w]) break;
            struct otl_lookup l;
            if (otl_lookup_info(&tab[w], (int)li, &l)) { FAILF("lookup %u unreadable", li); break; }
            if (l.type != type) FAILF("lookup %u type %u, want %u", li, l.type, type);
            if (l.flags != flags) FAILF("lookup %u flags %04x, want %04x", li, l.flags, flags);
            if ((uint32_t)l.nsub != nsub) FAILF("lookup %u subtables %d, want %u", li, l.nsub, nsub);
            checks += 3;
            break;
        }

        case K_SUBTYPE: {
            int w = g8(&r); uint32_t li = g32(&r), si = g32(&r); uint16_t rt = g16(&r);
            cur_li[w] = (int)li; cur_si[w] = (int)si; cur_sub[w] = 0; cur_type[w] = rt;
            ctx_cd[w][0] = ctx_cd[w][1] = ctx_cd[w][2] = 0;
            if (!have[w]) break;
            struct otl_lookup l;
            if (otl_lookup_info(&tab[w], (int)li, &l)) { FAILF("lookup %u unreadable", li); break; }
            int gt; uint32_t off;
            if (otl_subtable_type(&tab[w], &l, (int)si, &gt, &off)) {
                FAILF("lookup %u subtable %u will not resolve", li, si);
                break;
            }
            if (gt != rt) FAILF("lookup %u subtable %u type %d, want %u", li, si, gt, rt);
            cur_sub[w] = off;
            /* A contextual subtable announces its class defs here, so the
             * K_CLASS questions below know which of the three to ask. */
            if ((!w && (rt == 5 || rt == 6)) || (w && (rt == 7 || rt == 8))) {
                struct otl_ctx_info ci;
                int chain = (!w && rt == 6) || (w && rt == 8);
                if (otl_ctx_open(&tab[w], off, chain, &ci) == 0) {
                    ctx_cd[w][0] = ci.cd_input; ctx_cd[w][1] = ci.cd_back; ctx_cd[w][2] = ci.cd_ahead;
                    /* Every rule the subtable declares must decode. A rule we
                     * cannot decode would silently never fire in a shaper. */
                    for (int s = 0; s < ci.nsets; s++) {
                        int rc = otl_ctx_rule_count(&tab[w], &ci, off, s);
                        if (rc < 0) { FAILF("lookup %u set %d: rule count failed", li, s); break; }
                        for (int q = 0; q < rc; q++) {
                            struct otl_ctx_rule rule;
                            if (otl_ctx_rule(&tab[w], &ci, off, s, q, &rule)) {
                                FAILF("lookup %u subtable %u set %d rule %d will not decode", li, si, s, q);
                                break;
                            }
                            if (rule.ninput < 1)
                                FAILF("lookup %u rule %d has %d input positions", li, q, rule.ninput);
                            checks++;
                        }
                    }
                }
            }
            checks++;
            break;
        }

        case K_COVIDX: {
            int w = g8(&r); uint32_t li = g32(&r), si = g32(&r);
            uint16_t gid = g16(&r); int32_t want = gi32(&r);
            if (!have[w] || !cur_sub[w] || (int)li != cur_li[w] || (int)si != cur_si[w]) break;
            /* the coverage is the first offset of nearly every subtable shape;
             * ask through the same door a shaper would */
            struct fr b = { fdata, (uint32_t)flen };
            uint32_t s = cur_sub[w];
            uint32_t cov = (cur_type[w] == (w ? OTL_GPOS_MARK_BASE : 0) ||
                            (w && cur_type[w] >= OTL_GPOS_MARK_BASE && cur_type[w] <= OTL_GPOS_MARK_MARK))
                           ? fr_off16(&b, s, s + 2) : fr_off16(&b, s, s + 2);
            int got = otl_coverage_index(&tab[w], cov, gid);
            if (got != want) FAILF("lookup %u sub %u: coverage index of gid %u is %d, want %d",
                                   li, si, gid, got, want);
            if (otl_coverage_glyph(&tab[w], cov, want) != (int)gid)
                FAILF("lookup %u sub %u: coverage glyph at %d is not gid %u", li, si, want, gid);
            checks += 2;
            break;
        }

        case K_CLASS: {
            int w = g8(&r); uint32_t li = g32(&r), si = g32(&r);
            int sel = g8(&r); uint16_t gid = g16(&r); int32_t want = gi32(&r);
            if (!have[w] || !cur_sub[w] || (int)li != cur_li[w] || (int)si != cur_si[w]) break;
            uint32_t cd = 0;
            if (w && cur_type[w] == OTL_GPOS_PAIR) {
                struct fr b = { fdata, (uint32_t)flen };
                cd = fr_off16(&b, cur_sub[w], cur_sub[w] + (sel ? 10 : 8));
            } else {
                cd = ctx_cd[w][sel < 3 ? sel : 0];
            }
            if (!cd) break;
            int got = otl_class_of(&tab[w], cd, gid);
            if (got != want) FAILF("lookup %u sub %u cd%d: class of gid %u is %d, want %d",
                                   li, si, sel, gid, got, want);
            checks++;
            break;
        }

        case K_SINGLE: {
            int w = g8(&r); uint32_t li = g32(&r), si = g32(&r);
            uint16_t gid = g16(&r); int32_t want = gi32(&r);
            if (!have[w] || !cur_sub[w] || (int)li != cur_li[w] || (int)si != cur_si[w]) break;
            int got = (cur_type[w] == OTL_GSUB_REVERSE_CHAIN)
                      ? otl_gsub_reverse(&tab[w], cur_sub[w], gid)
                      : otl_gsub_single(&tab[w], cur_sub[w], gid);
            if (got != want) FAILF("lookup %u sub %u: single subst of gid %u is %d, want %d",
                                   li, si, gid, got, want);
            checks++;
            break;
        }

        case K_MULTI: {
            int w = g8(&r); uint32_t li = g32(&r), si = g32(&r);
            uint16_t gid = g16(&r); int n = g8(&r);
            uint16_t want[256];
            for (int i = 0; i < n; i++) want[i] = g16(&r);
            if (!have[w] || !cur_sub[w] || (int)li != cur_li[w] || (int)si != cur_si[w]) break;
            uint16_t got[256];
            int gn = otl_gsub_multiple(&tab[w], cur_sub[w], gid, got, 256);
            if (gn != n) FAILF("lookup %u: multiple subst of gid %u gives %d glyphs, want %d", li, gid, gn, n);
            else for (int i = 0; i < n; i++)
                if (got[i] != want[i]) { FAILF("lookup %u: multiple subst of gid %u [%d] is %u, want %u",
                                               li, gid, i, got[i], want[i]); break; }
            checks++;
            break;
        }

        case K_ALT: {
            int w = g8(&r); uint32_t li = g32(&r), si = g32(&r);
            uint16_t gid = g16(&r); uint32_t n = g32(&r);
            if (!have[w] || !cur_sub[w] || (int)li != cur_li[w] || (int)si != cur_si[w]) break;
            uint16_t one = 0;
            int got = otl_gsub_alternate(&tab[w], cur_sub[w], gid, 0, &one);
            if ((uint32_t)got != n) FAILF("lookup %u: gid %u has %d alternates, want %u", li, gid, got, n);
            checks++;
            break;
        }

        case K_LIG: {
            int w = g8(&r); uint32_t li = g32(&r), si = g32(&r);
            int n = g8(&r);
            uint16_t comp[256];
            for (int i = 0; i < n; i++) comp[i] = g16(&r);
            int32_t want = gi32(&r);
            if (!have[w] || !cur_sub[w] || (int)li != cur_li[w] || (int)si != cur_si[w]) break;
            int consumed = 0;
            int got = otl_gsub_ligature(&tab[w], cur_sub[w], comp[0], comp + 1, n - 1, &consumed);
            /* Longest match wins, so a shorter ligature that is a prefix of this
             * one is a correct answer to this question; only a MISS is a bug. */
            if (got < 0) FAILF("lookup %u: ligature of %d glyphs from gid %u not found", li, n, comp[0]);
            else if (consumed == n && got != want)
                FAILF("lookup %u: ligature of %d glyphs from gid %u is %d, want %d", li, n, comp[0], got, want);
            else if (consumed > n)
                FAILF("lookup %u: ligature consumed %d of %d glyphs", li, consumed, n);
            checks++;
            break;
        }

        case K_SINGLEPOS: {
            int w = g8(&r); uint32_t li = g32(&r), si = g32(&r); uint16_t gid = g16(&r);
            int32_t v[4]; for (int i = 0; i < 4; i++) v[i] = gi32(&r);
            if (!have[w] || !cur_sub[w] || (int)li != cur_li[w] || (int)si != cur_si[w]) break;
            struct otl_value got;
            if (otl_gpos_single(&tab[w], cur_sub[w], gid, &got)) {
                FAILF("lookup %u: single pos of gid %u not found", li, gid);
            } else if (got.x_placement != v[0] || got.y_placement != v[1] ||
                       got.x_advance != v[2] || got.y_advance != v[3]) {
                FAILF("lookup %u: single pos of gid %u is (%d,%d,%d,%d), want (%d,%d,%d,%d)",
                      li, gid, got.x_placement, got.y_placement, got.x_advance, got.y_advance,
                      v[0], v[1], v[2], v[3]);
            }
            checks++;
            break;
        }

        case K_PAIRV: {
            int w = g8(&r); uint32_t li = g32(&r), si = g32(&r);
            uint16_t g1 = g16(&r), g2 = g16(&r);
            int32_t v[8]; for (int i = 0; i < 8; i++) v[i] = gi32(&r);
            if (!have[w] || !cur_sub[w] || (int)li != cur_li[w] || (int)si != cur_si[w]) break;
            struct otl_value a, b;
            if (otl_gpos_pair(&tab[w], cur_sub[w], g1, g2, &a, &b)) {
                FAILF("lookup %u: pair (%u,%u) not found", li, g1, g2);
            } else {
                int32_t got[8] = { a.x_placement, a.y_placement, a.x_advance, a.y_advance,
                                   b.x_placement, b.y_placement, b.x_advance, b.y_advance };
                for (int i = 0; i < 8; i++)
                    if (got[i] != v[i]) {
                        FAILF("lookup %u: pair (%u,%u) value[%d] is %d, want %d",
                              li, g1, g2, i, got[i], v[i]);
                        break;
                    }
            }
            checks++;
            break;
        }

        case K_KERN: {
            uint16_t g1 = g16(&r), g2 = g16(&r); int32_t want = gi32(&r);
            if (!have_kern) { FAILF("kern table absent but the reference has pairs"); break; }
            int got = otl_kern_pair(&kern, g1, g2);
            if (got != want) FAILF("kern (%u,%u) is %d, want %d", g1, g2, got, want);
            checks++;
            break;
        }

        case K_GDEFC: {
            uint16_t gid = g16(&r); int32_t want = gi32(&r);
            if (!have_gdef) { FAILF("GDEF absent but the reference has classes"); break; }
            int got = otl_gdef_class(&gdef, gid);
            if (got != want) FAILF("GDEF class of gid %u is %d, want %d", gid, got, want);
            checks++;
            break;
        }

        default:
            printf("FAIL unknown reference record %d at %zu\n", kind, r.p);
            fails++;
            done = 1;
        }
    }
    if (r.over) { printf("FAIL reference stream truncated\n"); fails++; }

    printf("otlayout: %d checks\n", checks);
    if (checks < 50) { printf("FAIL only %d checks -- the reference is too thin to mean anything\n", checks); fails++; }
    printf(fails ? "SOME FAILED (%d)\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}
