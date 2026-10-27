/* ui-aui-text-api: the aui text weight API (design P1), tabular digits (P2),
 * metric vertical centring and explicit line heights (3.6).
 *
 * Drives the REAL aui.c (included, so its statics -- the scale cache, the text
 * caps, the tnum cell cache -- are reachable) over the host harness, as a
 * RECORDER: every SYS_GUI_TEXT_RUN and SYS_GUI_TEXT_RUN2 the API makes is kept
 * with its placement, face bits and weight. SYS_TEXT_MEASURE (the legacy width
 * call and the carets query) is answered by a FAKE PROPORTIONAL FONT defined
 * below: digit advances differ ('1' narrow, '0' widest) and the bold face is
 * 8 % wider, which the harness's box font (every digit 0.55 px) cannot show.
 * The metrics query goes to the harness's built-in answer.
 *
 * Checks print "FAIL <check> ..."; the exit status is 1 on any failure.
 *
 * -DUI_TEXT_API_NEGCTL (tests/ui-aui-text-api.mk) lays digits out at their
 * natural advances instead of in cells and must fail the 'tnum-cells' checks
 * and nothing else. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aui.c"
#include "aui_host.h"

extern void (*v2_text_spy)(const struct logit_run2 *);

static int checks, fails;
static void check(int ok, const char *fmt, ...)
{
    checks++;
    if (ok) return;
    fails++;
    va_list ap; va_start(ap, fmt);
    printf("FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* ------------------------------------------------------------ fake font */

/* Advance of one code point in DEVICE 24.8 at device size pxd. Per mille of
 * the size; bold (LOGIT_FACE_BOLD) is 8 % wider. */
static int fake_mille(unsigned cp)
{
    static const int dig[10] = { 600, 420, 560, 565, 580, 555, 570, 520, 590, 575 };
    if (cp >= '0' && cp <= '9') return dig[cp - '0'];
    if (cp == ':' || cp == '.') return 270;
    if (cp == ' ') return 260;
    if (cp == '%') return 800;
    if (cp >= 0x2E80) return 1000;
    return 550;
}
static int fake_adv24(unsigned cp, int pxd, int face)
{
    long long v = (long long)pxd * fake_mille(cp) * 256 * ((face & LOGIT_FACE_BOLD) ? 108 : 100);
    return (int)(v / 100000);
}
static unsigned u8_next(const unsigned char **pp, const unsigned char *e)
{
    const unsigned char *p = *pp;
    unsigned c = *p++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (c >= 0xF8 || (c >= 0x80 && c < 0xC0)) extra = 0;
    else if (extra) c &= (0x3F >> extra);
    while (extra-- > 0 && p < e && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
    *pp = p;
    return c;
}

static int last_measure_face = -1, last_carets_face = -1;
static int fake_sum24(const char *s, int len, int pxd, int face)
{
    const unsigned char *p = (const unsigned char *)s, *e = p + len;
    int sum = 0;
    while (p < e) sum += fake_adv24(u8_next(&p, e), pxd, face);
    return sum;
}
/* the legacy width call answers whole points, rounded */
static int fake_measure_pt(const char *s, int len, int px, int face, int pct)
{
    int sum = fake_sum24(s, len, aui_host_pt(px, pct), face);
    return (int)(((long long)sum * 100 / pct + 128) / 256);
}
static long fm_measure(struct aui_host *h, long a, long b, long c)
{
    if (b == LOGIT_TEXT_CARETS_QUERY && c == 0) {
        if (!(h->info & LOGIT_GUI2_F_CARETS)) return 0;
        const struct logit_text_carets_query *q = (const struct logit_text_carets_query *)a;
        if (q->size != sizeof *q || q->version != LOGIT_TEXT_CARETS_VERSION) return 0;
        if (q->flags || q->len < 0 || q->len > LOGIT_TEXT_METRICS_MAX_BYTES || q->px < 1 ||
            q->px > 512 || q->max < 1 || q->max > 1024 || (q->face & ~3)) return -1;
        last_carets_face = q->face;
        int pxd = aui_host_pt(q->px, h->pct), n = 0, x = 0;
        const unsigned char *s = (const unsigned char *)q->s, *e = s + q->len, *p = s;
        q->off[n] = 0; q->x24_8[n++] = 0;
        while (p < e) {
            x += fake_adv24(u8_next(&p, e), pxd, q->face);
            if (n >= q->max) return -1;
            q->off[n] = (unsigned short)(p - s); q->x24_8[n++] = x;
        }
        *q->count_out = n;
        return 1;
    }
    if (b < 0) return aui_host_sys_builtin(h, SYS_TEXT_MEASURE, a, b, c);   /* metrics query */
    int px = (int)((c >> 2) & 0x3FFFFFFF), face = (int)(c & 3);
    if (b > 1024 || px < 1 || px > 512) return 0;
    last_measure_face = face;
    return fake_measure_pt((const char *)a, (int)b, px, face, h->pct);
}

/* ------------------------------------------------------------ recording */

struct rec { char s[256]; int x24, y24, px24, face, weight, run2, bold; };
static struct rec recs[256];
static int nrec;
static void add_rec(const char *s, int len, int x24, int y24, int px24, int face, int weight, int run2, int bold)
{
    if (nrec >= 256) return;
    struct rec *r = &recs[nrec++];
    if (len > 250) len = 250;
    if (len < 0) len = 0;
    memcpy(r->s, s, (size_t)len); r->s[len] = 0;
    r->x24 = x24; r->y24 = y24; r->px24 = px24; r->face = face; r->weight = weight;
    r->run2 = run2; r->bold = bold;
}
static long spy_run(struct aui_host *h, long a, long b, long c)
{
    const struct logit_run *r = (const struct logit_run *)a;
    add_rec(r->s, r->len, r->x * 256, r->y * 256, r->px * 256, r->mono, 0, 0, r->bold);
    return aui_host_sys_builtin(h, SYS_GUI_TEXT_RUN, a, b, c);
}
static void spy_run2(const struct logit_run2 *r)
{ add_rec(r->s, r->len, r->x, r->y, r->px, r->face, r->weight, 1, 0); }

static struct aui_host *H;
static int g_pct, g_v2;
static void setup(int pct, int v2)
{
    aui_host_handlers_clear();
    H = aui_host_new(320, 200, pct);
    g_pct = pct; g_v2 = v2; sc_pct = pct;
    if (v2) aui_host_enable_v2(H);
    aui_host_on(SYS_GUI_TEXT_RUN, spy_run);
    aui_host_on(SYS_TEXT_MEASURE, fm_measure);
    v2_text_spy = spy_run2;
    aui_host_clock_set(1000);
    aui_begin(0x202020);
    nrec = 0;
}
static void teardown(void) { aui_end(); aui_feed_done(); aui_host_free(H); H = 0; }
static unsigned long long measures(void) { return H->calls[SYS_TEXT_MEASURE]; }

static const char *mode_name(void) { return g_v2 ? "v2" : "info0"; }
static int want_face(int w) { return w >= 600 ? LOGIT_FACE_BOLD : 0; }

/* ----------------------------------------------- expected digit geometry */

/* The 24.8-point advance of one ASCII char as the API sees it: v2 from the
 * carets answer (device 24.8 -> points, rounded), INFO=0 from the whole-point
 * width call. */
static int d2p(int v, int pct) { return (int)floor(((double)v * 100 + pct / 2) / pct); }
static int adv_pt24(char ch, int px, int face)
{
    if (g_v2) {
        int pxd = aui_host_pt(px, g_pct);
        return d2p(fake_adv24((unsigned char)ch, pxd, face), g_pct);
    }
    char s[2] = { ch, 0 };
    return fake_measure_pt(s, 1, px, face, g_pct) * 256;
}
/* The widest digit (the cell), computed independently of aui: v2 positions are
 * cumulative over "0123456789" exactly as one carets query reports them. */
static int cell_pt24(int px, int face)
{
    int best = 0;
    if (g_v2) {
        int pxd = aui_host_pt(px, g_pct), x = 0;
        for (int d = 0; d < 10; d++) {
            int nx = x + fake_adv24('0' + d, pxd, face);
            int a = d2p(nx, g_pct) - d2p(x, g_pct);
            if (a > best) best = a;
            x = nx;
        }
    } else {
        for (int d = 0; d < 10; d++) { int a = adv_pt24((char)('0' + d), px, face); if (a > best) best = a; }
    }
    return best;
}
/* Twice the centre (24.8 points) of byte i of the string the recs since rec0
 * spell, from each run's x plus the natural advances before it in its run. */
static int glyph_centre2(int rec0, int i, int px, int face)
{
    int at = 0;
    for (int r = rec0; r < nrec; r++) {
        int n = (int)strlen(recs[r].s), x = recs[r].x24;
        for (int j = 0; j < n; j++, at++) {
            int a = adv_pt24(recs[r].s[j], px, face);
            if (at == i) return 2 * x + a;
            x += a;
        }
    }
    return -(1 << 30);
}
/* Twice the right edge of the CELL of byte i (a digit): centre + cell/2. */
static int cell_right2(int rec0, int i, int px, int face)
{ return glyph_centre2(rec0, i, px, face) + cell_pt24(px, face); }
static void concat(int rec0, char *out, int max)
{
    out[0] = 0;
    for (int r = rec0; r < nrec; r++) strncat(out, recs[r].s, (size_t)(max - 1 - (int)strlen(out)));
}

/* -------------------------------------------------- 1. weight per API */

static const int weights[4] = { 400, 500, 600, 700 };
static const char *LONGS = "The quick brown fox jumps over the lazy dog again and again";

/* every rec since r0 is a run2 at weight w with face want_face(w) */
static void all_run2_w(int r0, int w, const char *api)
{
    int ok = nrec > r0;
    for (int r = r0; r < nrec; r++)
        ok = ok && recs[r].run2 && recs[r].weight == w && recs[r].face == want_face(w);
    check(ok, "weight %s w%d @%d: %d run(s), first run2=%d weight=%d face=%d", api, w, g_pct, nrec - r0,
          nrec > r0 ? recs[r0].run2 : -1, nrec > r0 ? recs[r0].weight : -1, nrec > r0 ? recs[r0].face : -1);
}
/* every rec since r0 is a legacy run with bold == (w >= 600), and no run2 */
static void all_legacy_w(int r0, int w, const char *api)
{
    int ok = nrec > r0;
    for (int r = r0; r < nrec; r++)
        ok = ok && !recs[r].run2 && recs[r].bold == (w >= 600) && recs[r].face == 0;
    check(ok, "fallback-face %s w%d @%d: %d run(s), first run2=%d bold=%d", api, w, g_pct, nrec - r0,
          nrec > r0 ? recs[r0].run2 : -1, nrec > r0 ? recs[r0].bold : -1);
}

static void draw_each_api(int w, void (*expect)(int, int, const char *))
{
    int r0;
    r0 = nrec; aui_text_w(10, 10, 14, w, 0xFFFFFF, "Hello"); expect(r0, w, "aui_text_w");
    r0 = nrec; aui_text_in_w(aui_r(10, 30, 200, 24), "Hello", 14, w, 0xFFFFFF, AUI_ALIGN_CENTER);
    expect(r0, w, "aui_text_in_w");
    r0 = nrec; aui_text_ellipsis_w(10, 60, 280, "Hello", 14, w, 0xFFFFFF, AUI_TRUNC_TAIL);
    expect(r0, w, "aui_text_ellipsis_w(fits)");
    r0 = nrec; aui_text_ellipsis_w(10, 80, 90, LONGS, 14, w, 0xFFFFFF, AUI_TRUNC_MIDDLE);
    expect(r0, w, "aui_text_ellipsis_w(cut)");
    r0 = nrec; aui_text_vc(aui_r(10, 100, 200, 28), "Hello", 14, w, 0xFFFFFF, AUI_ALIGN_RIGHT);
    expect(r0, w, "aui_text_vc");
    r0 = nrec; aui_text_tnum(10, 130, 14, w, 0xFFFFFF, "12:34", AUI_ALIGN_LEFT);
    expect(r0, w, "aui_text_tnum");
    r0 = nrec; aui_text_wrap_lh(aui_r(10, 150, 120, 60), LONGS, -1, 14, w, 0xFFFFFF, 0, 0,
                                AUI_ALIGN_LEFT, AUI_TRUNC_TAIL);
    expect(r0, w, "aui_text_wrap_lh");
}

static void t_weights(void)
{
    static const int pcts[3] = { 100, 125, 150 };
    for (int pi = 0; pi < 3; pi++)
        for (int wi = 0; wi < 4; wi++) {
            int w = weights[wi];
            setup(pcts[pi], 1);
            draw_each_api(w, all_run2_w);
            /* measurement and the carets query use the resolved face */
            last_measure_face = -1;
            int m = aui_text_measure_w("Hello", 14, w);
            check(last_measure_face == want_face(w) && m == fake_measure_pt("Hello", 5, 14, want_face(w), g_pct),
                  "weight measure face w%d @%d: face %d width %d", w, g_pct, last_measure_face, m);
            last_carets_face = -1;
            aui_text_ellipsis_w(10, 80, 90, LONGS, 14, w, 0xFFFFFF, AUI_TRUNC_TAIL);
            check(last_carets_face == want_face(w), "weight carets face w%d @%d: %d", w, g_pct, last_carets_face);
            /* the cut is fitted at the resolved face: the drawn line fits the box */
            {
                int r0 = nrec;
                aui_text_ellipsis_w(10, 80, 90, LONGS, 14, w, 0xFFFFFF, AUI_TRUNC_TAIL);
                int wd = nrec == r0 + 1 ? fake_sum24(recs[r0].s, (int)strlen(recs[r0].s),
                                                     aui_host_pt(14, g_pct), want_face(w)) : 1 << 30;
                check(wd <= aui_dev(90) * 256, "weight ellipsis fits at the resolved face w%d @%d: %d > %d",
                      w, g_pct, wd, aui_dev(90) * 256);
            }
            teardown();
            setup(pcts[pi], 0);
            draw_each_api(w, all_legacy_w);
            teardown();
        }
}

/* Weight 0 through the new entry points is the legacy call; the 2-argument
 * aui_text_w is still the width. */
static int same_recs(int a0, int a1, int b0, int b1)
{
    if (a1 - a0 != b1 - b0) return 0;
    for (int i = 0; i < a1 - a0; i++) {
        const struct rec *x = &recs[a0 + i], *y = &recs[b0 + i];
        if (strcmp(x->s, y->s) || x->x24 != y->x24 || x->y24 != y->y24 || x->px24 != y->px24 ||
            x->face != y->face || x->weight != y->weight || x->run2 != y->run2 || x->bold != y->bold) return 0;
    }
    return 1;
}
static void t_legacy(void)
{
    for (int v2 = 0; v2 < 2; v2++) {
        setup(125, v2);
        int a0, a1, b0, b1;
        unsigned long long m0, m1, m2;
        /* warm the metrics cache so both halves of each pair see the same state */
        aui_text_in(aui_r(5, 5, 150, 26), "Legacy", 0xABCDEF, 17, AUI_ALIGN_CENTER);
        nrec = 0;
        a0 = nrec; aui_text_sz(7, 9, "Legacy", 0xABCDEF, 13); a1 = nrec;
        b0 = nrec; aui_text_w(7, 9, 13, 0, 0xABCDEF, "Legacy"); b1 = nrec;
        check(same_recs(a0, a1, b0, b1), "legacy aui_text_w weight 0 == aui_text_sz (%s)", mode_name());
        m0 = measures(); a0 = nrec; aui_text_in(aui_r(5, 5, 150, 26), "Legacy", 0xABCDEF, 17, AUI_ALIGN_CENTER);
        a1 = nrec; m1 = measures();
        b0 = nrec; aui_text_in_w(aui_r(5, 5, 150, 26), "Legacy", 17, 0, 0xABCDEF, AUI_ALIGN_CENTER);
        b1 = nrec; m2 = measures();
        check(same_recs(a0, a1, b0, b1) && m1 - m0 == m2 - m1, "legacy aui_text_in_w weight 0 == aui_text_in (%s)",
              mode_name());
        m0 = measures(); a0 = nrec; aui_text_ellipsis_ex(3, 40, 80, LONGS, 0xFFFFFF, 13, AUI_TRUNC_HEAD);
        a1 = nrec; m1 = measures();
        b0 = nrec; aui_text_ellipsis_w(3, 40, 80, LONGS, 13, 0, 0xFFFFFF, AUI_TRUNC_HEAD);
        b1 = nrec; m2 = measures();
        check(same_recs(a0, a1, b0, b1) && m1 - m0 == m2 - m1,
              "legacy aui_text_ellipsis_w weight 0 == aui_text_ellipsis_ex (%s)", mode_name());
        check(aui_text_w("Legacy", 13) == aui_text_measure_w("Legacy", 13, 0) &&
              aui_text_w("Legacy", 13) == fake_measure_pt("Legacy", 6, 13, 0, g_pct),
              "legacy aui_text_w(s, px) is the width (%s)", mode_name());
        a0 = nrec; aui_text_wrap(aui_r(4, 4, 90, 80), LONGS, -1, 13, 0, 0xFFFFFF, 20, 0, AUI_ALIGN_LEFT, AUI_TRUNC_TAIL);
        a1 = nrec;
        b0 = nrec; aui_text_wrap_lh(aui_r(4, 4, 90, 80), LONGS, -1, 13, 0, 0xFFFFFF, 20, 0, AUI_ALIGN_LEFT, AUI_TRUNC_TAIL);
        b1 = nrec;
        check(a1 > a0 && same_recs(a0, a1, b0, b1), "legacy aui_text_wrap_lh weight 0 == aui_text_wrap (%s)",
              mode_name());
        teardown();
    }
}

/* -------------------------------------------------- 2. tabular digits */

static void t_tnum_cells(void)
{
    static const int pxs[3] = { 14, 17, 64 }, ws[2] = { 400, 600 }, pcts[3] = { 100, 125, 150 };
    int worst_v2 = 0, worst_i0 = 0;
    for (int v2 = 1; v2 >= 0; v2--)
        for (int pi = 0; pi < 3; pi++)
            for (int xi = 0; xi < 3; xi++)
                for (int wi = 0; wi < 2; wi++) {
                    setup(pcts[pi], v2);
                    int px = pxs[xi], w = ws[wi], face = want_face(w);
                    int cell = cell_pt24(px, face), colon = adv_pt24(':', px, face);
                    int r1 = nrec; aui_text_tnum(10, 20, px, w, 0xFFFFFF, "11:11", AUI_ALIGN_LEFT);
                    int r2 = nrec; aui_text_tnum(10, 20, px, w, 0xFFFFFF, "88:88", AUI_ALIGN_LEFT);
                    int r3 = nrec;
                    char a[64], b[64];
                    int save = nrec; nrec = r2; concat(r1, a, sizeof a); nrec = save;
                    concat(r2, b, sizeof b);
                    check(!strcmp(a, "11:11") && !strcmp(b, "88:88"), "tnum text %s px%d w%d @%d: '%s' '%s'",
                          mode_name(), px, w, g_pct, a, b);
                    nrec = r2; int c1 = glyph_centre2(r1, 3, px, face); nrec = r3;
                    int c2 = glyph_centre2(r2, 3, px, face);
                    /* the 4th glyph's centre is its cell's centre: two cells and a colon in */
                    int want2 = 2 * (10 * 256) + 2 * (2 * cell + colon) + cell;
                    int tol2 = v2 ? 2 : 2 * 256;            /* 1/256 pt (v2), 1 pt (whole-point runs) */
                    int d = abs(c1 - c2);
                    if (v2 && d > worst_v2) worst_v2 = d;
                    if (!v2 && d > worst_i0) worst_i0 = d;
                    check(d <= tol2, "tnum-cells 4th-glyph x '11:11' vs '88:88' %s px%d w%d @%d: %d vs %d (1/512 pt)",
                          mode_name(), px, w, g_pct, c1, c2);
                    check(abs(c1 - want2) <= tol2 && abs(c2 - want2) <= tol2,
                          "tnum-cells 4th-glyph at its cell centre %s px%d w%d @%d: %d %d want %d",
                          mode_name(), px, w, g_pct, c1, c2, want2);
                    check(aui_text_tnum_width("11:11", px, w) == aui_text_tnum_width("88:88", px, w) &&
                          aui_text_tnum_width("88:88", px, w) == (4 * cell + colon + 255) / 256,
                          "tnum-cells width %s px%d w%d @%d: %d %d want %d", mode_name(), px, w, g_pct,
                          aui_text_tnum_width("11:11", px, w), aui_text_tnum_width("88:88", px, w),
                          (4 * cell + colon + 255) / 256);

                    /* RIGHT: the last digit's cell edge on x; a trailing unit hangs past it */
                    int r4 = nrec; aui_text_tnum(300, 60, px > 17 ? 17 : px, w, 0xFFFFFF, "12:31", AUI_ALIGN_RIGHT);
                    int qpx = px > 17 ? 17 : px;
                    int e2 = cell_right2(r4, 4, qpx, face);
                    check(abs(e2 - 2 * 300 * 256) <= tol2, "tnum-cells right-aligned '12:31' %s px%d w%d @%d: edge %d want %d",
                          mode_name(), qpx, w, g_pct, e2, 2 * 300 * 256);
                    int r5 = nrec; aui_text_tnum(300, 90, qpx, w, 0xFFFFFF, "41%", AUI_ALIGN_RIGHT);
                    int e5 = cell_right2(r5, 1, qpx, face), u5 = glyph_centre2(r5, 2, qpx, face)
                             - adv_pt24('%', qpx, face);
                    check(abs(e5 - 2 * 300 * 256) <= tol2 && abs(u5 - 2 * 300 * 256) <= tol2,
                          "tnum-cells right-aligned unit hangs '41%%' %s px%d w%d @%d: edge %d unit %d want %d",
                          mode_name(), qpx, w, g_pct, e5, u5, 2 * 300 * 256);
                    int r6 = nrec; aui_text_tnum(160, 120, qpx, w, 0xFFFFFF, "1:11", AUI_ALIGN_CENTER);
                    int qcell = cell_pt24(qpx, face), qcolon = adv_pt24(':', qpx, face);
                    int st = 160 * 256 - (3 * qcell + qcolon) / 2;           /* the string's left edge */
                    int s6 = glyph_centre2(r6, 0, qpx, face) - qcell;        /* twice the first cell's left edge */
                    check(abs(s6 - 2 * st) <= tol2 + 2, "tnum-cells centred '1:11' %s px%d w%d @%d: left %d want %d",
                          mode_name(), qpx, w, g_pct, s6, 2 * st);
                    teardown();
                }
    printf("tnum: 4th-glyph centre spread at most %d/512 pt (v2), %d/512 pt (INFO=0)\n", worst_v2, worst_i0);
}

/* Weight and face on the digit runs, and the one-query cell cache. */
static void t_tnum_cache(void)
{
    static const int pcts[4] = { 100, 125, 150, 100 };
    for (int v2 = 1; v2 >= 0; v2--) {
        for (int pi = 0; pi < 4; pi++) {
            setup(pcts[pi], v2);
            int px = 22;                          /* a size no other check uses */
            unsigned long long m0 = measures();
            int wd = aui_text_tnum_width("2048", px, 600);
            unsigned long long m1 = measures();
            int wd2 = aui_text_tnum_width("2048", px, 600);
            unsigned long long m2 = measures();
            int cell = cell_pt24(px, LOGIT_FACE_BOLD);
            int miss = (int)(m1 - m0), hit = (int)(m2 - m1);
            if (pi < 3) {
                /* a miss is one carets query (v2) or ten widths (INFO=0) */
                check(miss == (v2 ? 1 : 10), "tnum cache miss at %d%% (%s): %d measure syscalls", g_pct,
                      mode_name(), miss);
            } else {
                check(miss == 0, "tnum cache keeps 100%% across 125/150 (%s): %d syscalls", mode_name(), miss);
            }
            check(hit == 0, "tnum cache hit at %d%% (%s): %d measure syscalls", g_pct, mode_name(), hit);
            check(wd == wd2, "tnum cache value stable at %d%% (%s): %d %d", g_pct, mode_name(), wd, wd2);
            check(wd == (4 * cell + 255) / 256, "tnum-cells cache value at %d%% (%s): %d want %d",
                  g_pct, mode_name(), wd, (4 * cell + 255) / 256);
            /* an all-digit string draws with no measure syscall once cached */
            unsigned long long m3 = measures();
            aui_text_tnum(10, 10, px, 600, 0xFFFFFF, "2048", AUI_ALIGN_LEFT);
            check(measures() == m3, "tnum digits draw with no measure syscall at %d%% (%s)", g_pct, mode_name());
            teardown();
        }
    }
    /* a tabular font costs one run: every digit as wide as its cell */
    setup(100, 1);
    int r0 = nrec; aui_text_tnum(10, 10, 14, 400, 0xFFFFFF, "0000", AUI_ALIGN_LEFT);
    check(nrec - r0 == 1 && !strcmp(recs[r0].s, "0000"), "tnum equal-width digits merge into one run: %d run(s)",
          nrec - r0);
    teardown();
}

/* ------------------------------------------- 3. baseline and centring */

static void t_baseline(void)
{
    static const int cases[2][3] = { { 0, 24, 14 }, { 0, 28, 13 } };
    for (int i = 0; i < 2; i++) {
        int top = cases[i][0], h = cases[i][1], px = cases[i][2];
        double ref = (top + (h - 1.448 * px) / 2 + 1.160 * px) * 256;
        int got = aui_text_baseline24(top * 256, h * 256, px);
        check(fabs(got - ref) < 1.0, "baseline (%d,%d,%d): %d/256 pt, exact %.3f/256", top, h, px, got, ref);
    }
    double worst = 0;
    for (int px = 11; px <= 72; px++)
        for (int h = 0; h <= 96; h += 1)
            for (int top = -3; top <= 40; top += 43 / 3) {
                double ref = (top + (h - 1.448 * px) / 2 + 1.160 * px) * 256;
                double d = fabs(aui_text_baseline24(top * 256, h * 256, px) - ref);
                if (d > worst) worst = d;
            }
    check(worst <= 0.5 + 1e-9, "baseline one rounding: worst %.3f/256 pt", worst);
    printf("baseline: (0,24,14) = %d, (0,28,13) = %d (1/256 pt); worst %.3f/256 over 11..72 px\n",
           aui_text_baseline24(0, 24 * 256, 14), aui_text_baseline24(0, 28 * 256, 13), worst);

    /* aui_text_vc puts the run's baseline (y + ascent) there */
    static const int pcts[3] = { 100, 125, 150 };
    for (int pi = 0; pi < 3; pi++) {
        setup(pcts[pi], 1);
        int asc24 = (int)floor(14 * 256 * 1.160 + 0.5);
        int r0 = nrec; aui_text_vc(aui_r(10, 10, 200, 24), "Hg", 14, 500, 0xFFFFFF, AUI_ALIGN_LEFT);
        check(nrec == r0 + 1 && recs[r0].y24 + asc24 == aui_text_baseline24(10 * 256, 24 * 256, 14) &&
              recs[r0].x24 == 10 * 256, "vc baseline @%d: y %d + %d want %d", g_pct,
              nrec > r0 ? recs[r0].y24 : 0, asc24, aui_text_baseline24(10 * 256, 24 * 256, 14));
        r0 = nrec; aui_text_vc(aui_r(10, 40, 200, 28), "Hg", 13, 400, 0xFFFFFF, AUI_ALIGN_RIGHT);
        int asc13 = (int)floor(13 * 256 * 1.160 + 0.5);
        int wpt = fake_measure_pt("Hg", 2, 13, 0, g_pct);
        check(nrec == r0 + 1 && recs[r0].y24 + asc13 == aui_text_baseline24(40 * 256, 28 * 256, 13) &&
              recs[r0].x24 == (210 - wpt) * 256, "vc right @%d: x %d want %d", g_pct,
              nrec > r0 ? recs[r0].x24 : 0, (210 - wpt) * 256);
        teardown();
    }
}

/* ------------------------------------------------ 4. line heights */

static void t_wrap(void)
{
    for (int v2 = 1; v2 >= 0; v2--) {
        static const int pcts[3] = { 100, 125, 150 };
        for (int pi = 0; pi < 3; pi++) {
            setup(pcts[pi], v2);
            int r0 = nrec;
            int nl = aui_text_wrap_lh(aui_r(10, 10, 110, 150), LONGS, -1, 14, 400, 0xFFFFFF, 22, 0,
                                      AUI_ALIGN_LEFT, AUI_TRUNC_TAIL);
            check(nl >= 3 && nrec - r0 >= 3, "wrap lines (%s @%d): %d lines %d runs", mode_name(), g_pct, nl, nrec - r0);
            if (nrec - r0 >= 3)
                check(recs[r0 + 1].y24 - recs[r0].y24 == 22 * 256 && recs[r0 + 2].y24 - recs[r0].y24 == 44 * 256,
                      "wrap line 2 at +22 (%s @%d): +%d/256 pt", mode_name(), g_pct, recs[r0 + 1].y24 - recs[r0].y24);
            int hm = aui_text_measure_wrap_lh(110, LONGS, -1, 14, 400, 22, 0);
            check(hm == nl * 22, "wrap measure = lines x 22 (%s @%d): %d vs %d", mode_name(), g_pct, hm, nl * 22);
            /* line_h 0: the 3.6 line height of the style (body 14 -> 20), not 5/4 */
            r0 = nrec;
            aui_text_wrap_lh(aui_r(10, 10, 110, 150), LONGS, -1, 14, 600, 0xFFFFFF, 0, 0, AUI_ALIGN_LEFT, AUI_TRUNC_TAIL);
            if (nrec - r0 >= 2)
                check(recs[r0 + 1].y24 - recs[r0].y24 == 20 * 256, "wrap default 3.6 line height (%s @%d): +%d/256",
                      mode_name(), g_pct, recs[r0 + 1].y24 - recs[r0].y24);
            else check(0, "wrap default 3.6 line height (%s @%d): %d runs", mode_name(), g_pct, nrec - r0);
            teardown();
        }
    }
    check(aui_text_line_h(14) == 20 && aui_text_line_h(13) == 18 && aui_text_line_h(12) == 16 &&
          aui_text_line_h(11) == 14 && aui_text_line_h(17) == 24 && aui_text_line_h(22) == 30 &&
          aui_text_line_h(26) == 34 && aui_text_line_h(64) == 72 && aui_text_line_h(72) == 80,
          "line heights 3.6: 14->%d 13->%d 12->%d", aui_text_line_h(14), aui_text_line_h(13), aui_text_line_h(12));
}

int main(void)
{
    t_weights();
    t_legacy();
    t_tnum_cells();
    t_tnum_cache();
    t_baseline();
    t_wrap();
    printf("%s ui-aui-text-api: %d checks, %d failed\n", fails ? "result:" : "ok  ", checks, fails);
    return fails ? 1 : 0;
}
