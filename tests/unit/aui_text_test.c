/* aui-text-adopt: wrap, UTF-8-safe U+2026 ellipsis (tail / middle / head) through one
 * carets query, baseline alignment from the metrics query, alpha text. Drives the
 * REAL aui.c (included, so its statics -- scale cache, text caps -- are reachable)
 * over the host harness with tests/unit/aui_host/v2_text.c registered. Failures print
 * "FAIL <check>"; the exit status is non-zero on any.
 *
 * -DAUI_TEXT_LEGACY_ELLIPSIS restores the byte cut with "..." and must fail the
 * 'utf8' checks (and the middle-extension one) and nothing else. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aui.c"
#include "aui_host.h"

extern unsigned long v2_text_nmetrics, v2_text_ncarets, v2_text_nrun2;
extern void (*v2_text_spy)(const struct logit_run2 *);

static int dev24(int v, int pct);
static int checks, fails;
static void check(int ok, const char *what)
{ checks++; if (!ok) { fails++; printf("FAIL %s\n", what); } }
static void checkf(int ok, const char *fmt, const char *a, int b)
{ char buf[160]; snprintf(buf, sizeof buf, fmt, a, b); check(ok, buf); }

/* ------------------------------------------------------------ recording */

struct rec { char s[1200]; int x24, y24, px24, alpha, run2; };
static struct rec recs[160];
static int nrec;
static void add_rec(const char *s, int len, int x24, int y24, int px24, int alpha, int run2)
{
    if (nrec >= 160) return;
    struct rec *r = &recs[nrec++];
    if (len > 1190) len = 1190;
    if (len < 0) len = 0;
    memcpy(r->s, s, (size_t)len); r->s[len] = 0;
    r->x24 = x24; r->y24 = y24; r->px24 = px24; r->alpha = alpha; r->run2 = run2;
}
static long spy_run(struct aui_host *h, long a, long b, long c)
{
    const struct logit_run *r = (const struct logit_run *)a;
    add_rec(r->s, r->len, r->x * 256, r->y * 256, r->px * 256, 255, 0);
    return aui_host_sys_builtin(h, SYS_GUI_TEXT_RUN, a, b, c);
}
static void spy_run2(const struct logit_run2 *r)
{ add_rec(r->s, r->len, r->x, r->y, r->px, r->alpha, 1); }

static struct aui_host *H;
static int g_pct;
static void setup(int pct, int v2)
{
    aui_host_handlers_clear();
    H = aui_host_new(160, 120, pct);
    g_pct = pct; sc_pct = pct;
    if (v2) aui_host_enable_v2(H);
    aui_host_on(SYS_GUI_TEXT_RUN, spy_run);
    v2_text_spy = spy_run2;
    aui_host_clock_set(1000);
    aui_begin(0x202020);
    nrec = 0;
}
static void teardown(void) { aui_end(); aui_feed_done(); aui_host_free(H); H = 0; }

/* ------------------------------------------------------------- utf-8 */

static int u8_len_ok(const unsigned char *p, int n, unsigned *cp)
{
    unsigned c = p[0];
    if (c < 0x80) { *cp = c; return 1; }
    int k = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 ? 1 : -1;
    if (k < 0 || c > 0xF4 || k >= n) return 0;
    unsigned v = c & (0x3F >> k);
    for (int i = 1; i <= k; i++) { if ((p[i] & 0xC0) != 0x80) return 0; v = (v << 6) | (p[i] & 0x3F); }
    if ((k == 2 && v < 0x800) || (k == 3 && (v < 0x10000 || v > 0x10FFFF)) || (v >= 0xD800 && v < 0xE000)) return 0;
    *cp = v;
    return k + 1;
}
static int u8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = (int)strlen(s);
    while (n > 0) {
        unsigned cp; int k = u8_len_ok(p, n, &cp);
        if (!k) return 0;
        p += k; n -= k;
    }
    return 1;
}
/* a UTF-8 boundary that does not split a cluster (ZWJ / VS / tone / combining) */
static int cluster_edge(const char *s, int off)
{
    int n = (int)strlen(s);
    if (off <= 0 || off >= n) return 1;
    const unsigned char *p = (const unsigned char *)s;
    unsigned cp = 0;
    if (!u8_len_ok(p + off, n - off, &cp)) return 0;                  /* inside a sequence */
    if (cp == 0x200D || (cp >= 0x300 && cp <= 0x36F) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
        (cp >= 0x1F3FB && cp <= 0x1F3FF)) return 0;
    int q = off - 1;
    while (q > 0 && (p[q] & 0xC0) == 0x80) q--;
    unsigned pc = 0; u8_len_ok(p + q, n - q, &pc);
    return pc != 0x200D;
}
#define ELL "\xE2\x80\xA6"
static int count_sub(const char *s, const char *sub)
{ int n = 0; for (const char *p = s; (p = strstr(p, sub)); p++) n++; return n; }
static int ends_with(const char *s, const char *t)
{ size_t a = strlen(s), b = strlen(t); return a >= b && !memcmp(s + a - b, t, b); }
static int starts_with(const char *s, const char *t) { return !strncmp(s, t, strlen(t)); }
static int dev_adv(const char *s, int px) { return aui_host_text_advance(s, (int)strlen(s), aui_host_pt(px, g_pct)); }

/* ------------------------------------------------------------ ellipsis */

static const char *const strs[] = {
    "\xE7\x9F\xAD\xE3\x81\x84\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x83\xA9\xE3\x83\x99\xE3\x83\xAB\xE3\x81\x8C\xE3\x81\xA8\xE3\x81\xA6\xE3\x82\x82\xE9\x95\xB7\xE3\x81\x84",
    "\xF0\x9F\x98\x80\xF0\x9F\x98\x83\xF0\x9F\x98\x84\xF0\x9F\x98\x81\xF0\x9F\x98\x86\xF0\x9F\x98\x85\xF0\x9F\xA4\xA3\xF0\x9F\x98\x82\xF0\x9F\x99\x82\xF0\x9F\x99\x83",
    "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7 family \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7 together \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD forever",
    "very_long_document_name_final_version_2.txt",
    "The quick brown fox jumps over the lazy dog",
};
#define NSTR ((int)(sizeof strs / sizeof *strs))

static void t_ellipsis(void)
{
    static const int pcts[3] = { 100, 125, 150 };
    int cuts = 0, maxcalls = 0;
    for (int pi = 0; pi < 3; pi++)
        for (int si = 0; si < NSTR; si++)
            for (int mode = 0; mode < 3; mode++)           /* TAIL, MIDDLE, HEAD */
                for (int maxw = 14; maxw <= 220; maxw += 13) {
                    setup(pcts[pi], 1);
                    const char *s = strs[si];
                    int px = 13, wdev = aui_dev(maxw);
                    int fits = dev_adv(s, px) <= wdev;
                    unsigned long long c0 = H->calls[SYS_TEXT_MEASURE];
                    int ng = nrec;
                    aui_text_ellipsis_ex(4, 6, maxw, s, 0xFFFFFF, px, mode);
                    int dc = (int)(H->calls[SYS_TEXT_MEASURE] - c0);
                    if (dc > maxcalls) maxcalls = dc;
                    char tag[120];
                    snprintf(tag, sizeof tag, "str%d mode%d w%d @%d", si, mode, maxw, pcts[pi]);
                    checkf(dc <= 2, "utf8 measure-count (<= 2 syscalls) %s %d", tag, dc);
                    checkf(nrec - ng == 1, "utf8 one run %s %d", tag, nrec - ng);
                    {
                        char d[2400] = "";
                        for (int i = ng; i < nrec; i++) strcat(d, recs[i].s);     /* what the user sees */
                        checkf(u8_valid(d), "utf8 valid %s %d", tag, 0);
                        if (fits) checkf(!strcmp(d, s), "utf8 whole-string-kept %s %d", tag, 0);
                        else {
                            cuts++;
                            checkf(count_sub(d, ELL) == 1 && !strstr(d, "..."), "utf8 U+2026 %s %d", tag, 0);
                            checkf(dev_adv(d, px) <= wdev, "utf8 width <= box %s %d", tag, dev_adv(d, px) - wdev);
                            const char *e = strstr(d, ELL);
                            if (e) {
                                int pre = (int)(e - d);
                                const char *post = e + 3;
                                int plen = (int)strlen(post), slen_ = (int)strlen(s);
                                int ok = !strncmp(d, s, (size_t)pre) && plen <= slen_ &&
                                         !strcmp(post, s + slen_ - plen);
                                if (mode == AUI_TRUNC_TAIL) ok = ok && plen == 0;
                                if (mode == AUI_TRUNC_HEAD) ok = ok && pre == 0;
                                checkf(ok, "utf8 mode pieces %s %d", tag, 0);
                                checkf(cluster_edge(s, pre) && cluster_edge(s, slen_ - plen), "utf8 cluster edge %s %d", tag, 0);
                            }
                            /* MIDDLE keeps the extension when the box is wide enough for it */
                            if (si == 3 && mode == AUI_TRUNC_MIDDLE && wdev >= aui_host_pt(100, g_pct))
                                checkf(ends_with(d, ".txt"), "middle extension kept %s %d", tag, 0);
                        }
                    }
                    teardown();
                }
    printf("ellipsis: %d cut cases, at most %d measure syscalls per call\n", cuts, maxcalls);
}

/* INFO = 0: no CARETS, no TEXT_RUN2 -- the old path with a code-point-safe cut. */
static void t_ellipsis_fallback(void)
{
    int cuts = 0;
    for (int pi = 0; pi < 3; pi++)
        for (int si = 0; si < NSTR; si++)
            for (int maxw = 14; maxw <= 220; maxw += 13) {
                static const int pcts[3] = { 100, 125, 150 };
                setup(pcts[pi], 0);
                int px = 13, wdev = aui_dev(maxw);
                aui_text_ellipsis(4, 6, maxw, strs[si], 0xFFFFFF, px);
                char tag[120];
                snprintf(tag, sizeof tag, "str%d w%d @%d", si, maxw, pcts[pi]);
                int ok = nrec >= 1 && nrec <= 2;
                for (int i = 0; i < nrec; i++) ok = ok && u8_valid(recs[i].s);
                checkf(ok, "utf8 fallback valid %s %d", tag, 0);
                if (nrec == 2) {
                    cuts++;
                    checkf(!strcmp(recs[1].s, "..."), "utf8 fallback dots %s %d", tag, 0);
                    checkf(dev_adv(recs[0].s, px) + dev_adv("...", px) <= wdev + 2, "utf8 fallback width %s %d", tag, 0);
                }
                teardown();
            }
    check(cuts > 0, "utf8 fallback exercised");
}

/* ----------------------------------------------------------------- wrap */

static const char *const para = "The quick brown fox jumps over the lazy dog and keeps running far away from here";
static const char *const cjk = "\xE7\x9F\xAD\xE3\x81\x84\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x83\xA9\xE3\x83\x99\xE3\x83\xAB\xE3\x81\x8C\xE3\x81\xA8\xE3\x81\xA6\xE3\x82\x82\xE9\x95\xB7\xE3\x81\x84\xE3\x81\xAE\xE3\x81\xA7\xE6\x8A\x98\xE3\x82\x8A\xE8\xBF\x94\xE3\x81\x99";

static void squash(const char *s, char *out)       /* drop spaces and the ellipsis */
{
    while (*s) {
        if (*s == ' ') s++;
        else if (!strncmp(s, ELL, 3)) s += 3;
        else *out++ = *s++;
    }
    *out = 0;
}

static void wrap_checks(int v2, int pct)
{
    char tag[64];
    snprintf(tag, sizeof tag, "%s@%d", v2 ? "v2" : "legacy", pct);
    struct aui_rect r = aui_r(6, 10, 70, 1000);
    int lh = 17;

    /* everything fits in as many lines as it takes; each line fits the box */
    setup(pct, v2);
    int n = aui_text_wrap(r, para, -1, 13, 0, 0xFFFFFF, lh, 0, AUI_ALIGN_LEFT, AUI_TRUNC_TAIL);
    checkf(n >= 3 && n == nrec, "wrap lines drawn %s %d", tag, n);
    char all[600] = "", flat[600], want[600];
    int fit = 1;
    for (int i = 0; i < nrec; i++) { strcat(all, recs[i].s); fit = fit && dev_adv(recs[i].s, 13) <= aui_dev(70); }
    squash(all, flat); squash(para, want);
    checkf(fit, "wrap every line fits the box %s %d", tag, 0);
    checkf(!strcmp(flat, want), "wrap keeps every character %s %d", tag, 0);
    int free_n = n;
    int hgt = aui_text_measure_wrap(70, para, -1, 13, 0, lh, 0);
    checkf(hgt == free_n * lh, "wrap measure height == lines * line_h %s %d", tag, hgt);
    teardown();

    /* max_lines is obeyed, and the last line is cut with U+2026 (or "..." legacy) */
    for (int ml = 1; ml <= 3; ml++) {
        setup(pct, v2);
        n = aui_text_wrap(r, para, -1, 13, 0, 0xFFFFFF, lh, ml, AUI_ALIGN_LEFT, AUI_TRUNC_TAIL);
        checkf(n == ml && nrec == (v2 ? ml : ml + 1), "wrap obeys max_lines %s %d", tag, n);   /* legacy: the dots are a second run */
        int ok = 1;
        for (int i = 0; i < nrec; i++) ok = ok && u8_valid(recs[i].s) && dev_adv(recs[i].s, 13) <= aui_dev(70);
        checkf(ok, "wrap capped lines valid and fit %s %d", tag, 0);
        checkf(nrec > 0 && (v2 ? ends_with(recs[nrec - 1].s, ELL) : ends_with(recs[nrec - 1].s, "...")),
               "wrap last line ellipsised %s %d", tag, 0);
        int hm = aui_text_measure_wrap(70, para, -1, 13, 0, lh, ml);
        checkf(hm == ml * lh, "wrap measure honours max_lines %s %d", tag, hm);
        teardown();
        setup(pct, v2);
        n = aui_text_wrap(r, para, -1, 13, 0, 0xFFFFFF, lh, ml, AUI_ALIGN_LEFT, AUI_TRUNC_NONE);
        int none = 1;
        for (int i = 0; i < nrec; i++) none = none && !strstr(recs[i].s, ELL) && !strstr(recs[i].s, "...");
        checkf(n == ml && none, "wrap TRUNC_NONE stops without an ellipsis %s %d", tag, n);
        teardown();
    }

    /* no spaces (CJK): breaks between clusters, never inside a byte sequence */
    setup(pct, v2);
    n = aui_text_wrap(r, cjk, -1, 13, 0, 0xFFFFFF, lh, 0, AUI_ALIGN_LEFT, AUI_TRUNC_NONE);
    int ok = n >= 2 && n == nrec;
    for (int i = 0; i < nrec; i++) ok = ok && u8_valid(recs[i].s) && dev_adv(recs[i].s, 13) <= aui_dev(70);
    if (!ok) for (int i = 0; i < nrec; i++) printf("  line %d: adv %d box %d valid %d\n", i, dev_adv(recs[i].s, 13), aui_dev(70), u8_valid(recs[i].s));
    checkf(ok, "wrap CJK lines valid and fit %s %d", tag, n);
    teardown();

    /* alignment */
    if (v2) {
        setup(pct, v2);
        n = aui_text_wrap(r, "ab cd", -1, 13, 0, 0xFFFFFF, lh, 0, AUI_ALIGN_RIGHT, AUI_TRUNC_TAIL);
        int w = dev_adv("ab cd", 13);
        int xdev = dev24(recs[0].x24, pct);
        checkf(n == 1 && xdev + w <= aui_dev(r.x + r.w) + 1 && xdev + w >= aui_dev(r.x + r.w) - 1,
               "wrap right-align ends at the box edge %s %d", tag, xdev + w - aui_dev(r.x + r.w));
        teardown();
    }
}

static void t_wrap(void)
{
    static const int pcts[3] = { 100, 125, 150 };
    for (int i = 0; i < 3; i++) { wrap_checks(1, pcts[i]); wrap_checks(0, pcts[i]); }
}

/* ------------------------------------------------------------- baseline */

static int dev24(int v, int pct)
{
    long long n = (long long)v * pct;
    return (int)(n >= 0 ? n / 25600 : -((-n + 25599) / 25600));
}

static void t_baseline(void)
{
    static const int pcts[3] = { 100, 125, 150 };
    for (int pi = 0; pi < 3; pi++)
        for (int v2 = 0; v2 < 2; v2++) {
            int pct = pcts[pi];
            setup(pct, v2);
            struct aui_rect r = aui_r(4, 10, 120, 24);
            aui_text_in(r, "Small", 0xFFFFFF, 13, AUI_ALIGN_LEFT);
            aui_text_in(r, "Large", 0xFFFFFF, 17, AUI_ALIGN_LEFT);
            aui_text_in(r, "Body", 0xFFFFFF, AUI_FS_BODY, AUI_ALIGN_CENTER);
            char tag[48]; snprintf(tag, sizeof tag, "%s @%d", v2 ? "v2" : "legacy", pct);
            checkf(nrec == 3, "baseline runs drawn %s %d", tag, nrec);
            if (nrec == 3) {
                int pxs[3] = { 13, 17, AUI_FS_BODY }, bl[3];
                for (int i = 0; i < 3; i++) {
                    int y = v2 ? dev24(recs[i].y24, pct) : aui_host_pt(recs[i].y24 / 256, pct);
                    bl[i] = y + (80 * aui_host_pt(pxs[i], pct) + 50) / 100;
                }
                /* design 3.6 metric centring (ui-aui-theme): every size is centred
                 * in the rect by the font's own metrics, so the sizes no longer
                 * share one baseline. Each run's top + ascent IS
                 * aui_text_baseline24() of the rect: exactly with TEXT_RUN2 (24.8
                 * points); within one device pixel on the legacy path, which
                 * rounds the run to whole points. */
                for (int i = 0; i < 3; i++) {
                    int want = aui_text_baseline24(10 * 256, 24 * 256, pxs[i]);
                    int d = recs[i].y24 + tx_asc24(pxs[i]) - want;
                    if (d < 0) d = -d;
                    int tol = v2 ? 0 : (25600 + pct - 1) / pct;      /* 1 device px in 24.8 pt */
                    checkf(d <= tol, "baseline 3.6 metric rule %s %d", tag, d);
                }
                /* and the text sits inside its rect */
                int top = aui_dev(10), bot = aui_dev(34);
                checkf(bl[0] > top && bl[0] < bot, "baseline inside rect %s %d", tag, bl[0]);
            }
            check(!v2 || nrec == 0 || recs[0].run2, "baseline v2 uses TEXT_RUN2");
            teardown();
        }
    /* design 3.6: aui_text_in centres by the ui*.ttf metrics in logit_tokens.h,
     * so it issues NO metrics query -- not on the first run, not on the next,
     * not after a scale change (the cache these checks used to pin is gone). */
    setup(100, 1);
    struct aui_rect r = aui_r(0, 0, 100, 24);
    unsigned long m0 = v2_text_nmetrics;
    aui_text_in(r, "x", 0xFFFFFF, 13, AUI_ALIGN_LEFT);
    aui_text_in(r, "y", 0xFFFFFF, 13, AUI_ALIGN_LEFT);
    check(v2_text_nmetrics == m0 && nrec == 2, "aui_text_in issues no metrics query @100");
    teardown();
    setup(175, 1);                                 /* same px and face, new scale */
    m0 = v2_text_nmetrics;
    aui_text_in(r, "x", 0xFFFFFF, 13, AUI_ALIGN_LEFT);
    check(v2_text_nmetrics == m0 && nrec == 1, "aui_text_in issues no metrics query @175");
    teardown();
}

/* ---------------------------------------------------------------- alpha */

static int has_red_between(int lo, int hi)
{
    for (int i = 0; i < H->w * H->h; i++) {
        int rr = (int)((H->px[i] >> 16) & 255);
        if (rr >= lo && rr <= hi && ((H->px[i] >> 8) & 255) < 0x40) return 1;
    }
    return 0;
}
static void t_alpha(void)
{
    setup(100, 1);
    unsigned long r0 = v2_text_nrun2;
    aui_text_a(10, 10, "MMMM", 0xFF0000, 16, 128);
    check(v2_text_nrun2 == r0 + 1, "alpha text issues TEXT_RUN2");
    check(nrec == 1 && recs[0].run2 && recs[0].alpha == 128, "alpha text carries the alpha");
    /* bg 0x20 blended 50% with 0xFF => about 0x90 */
    check(has_red_between(0x80, 0xA0), "alpha text is blended with the surface");
    aui_text_a(10, 40, "MMMM", 0xFF0000, 16, 255);
    check(v2_text_nrun2 == r0 + 1, "opaque alpha text takes the plain run");
    teardown();
    setup(100, 0);
    aui_text_a(10, 10, "MMMM", 0xFF0000, 16, 128);
    check(nrec == 1 && !recs[0].run2, "alpha text without TEXT_RUN2 falls back to a plain run");
    teardown();
}

/* ------------------------------------------------------------- caps */

static void t_caps(void)
{
    setup(100, 0);
    check(!tx_has_layout() && !tx_has_run2(), "INFO = 0: no v2 text features");
    teardown();
    setup(100, 1);
    check(tx_has_layout() && tx_has_run2(), "v2: CARETS and TEXT_RUN2 reported");
    teardown();
}

int main(void)
{
    t_caps();
    t_ellipsis();
    t_ellipsis_fallback();
    t_wrap();
    t_baseline();
    t_alpha();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
