/* Host test for c/lib/text/para.c with fake carets callbacks. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "para.h"
#include "linebreak.h"

static int g_fail, g_calls;
#define CHECK(name, cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s: ", name); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned dec(const char *s, int len, int *adv)
{
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80 || len < 2) { *adv = 1; return c; }
    if (c < 0xE0) { *adv = 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
    if (c < 0xF0 && len >= 3) { *adv = 3; return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); }
    if (len >= 4) { *adv = 4; return ((c & 7) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); }
    *adv = 1; return c;
}
static int is_comb(unsigned cp) { return cp >= 0x300 && cp <= 0x36F; }

/* Fixed advance: 8 px per cell; CJK (>= 0x2E80) is 2 cells; combining joins the
 * previous cluster (no stop between). "\r\n" is one cluster. */
static int fake_carets(void *ctx, const char *s, int len, int px, unsigned face,
                       unsigned short *off, int *x, int max)
{
    (void)px; (void)face; int wide_cjk = ctx != 0;
    g_calls++;
    int n = 0, i = 0, xx = 0;
    if (max < 1) return -1;
    off[n] = 0; x[n++] = 0;
    while (i < len) {
        int adv; unsigned cp = dec(s + i, len - i, &adv);
        int cells = (cp >= 0x2E80 && wide_cjk) ? 2 : (is_comb(cp) ? 0 : 1);
        if (cells == 0 && n > 1) { i += adv; off[n - 1] = (unsigned short)i; continue; }
        i += adv; xx += cells * 8 * 256;
        if (i < len && s[i - adv] == '\r' && s[i] == '\n') { i++; }
        if (n >= max) return -1;
        off[n] = (unsigned short)i; x[n++] = xx;
    }
    /* a combining mark moved off[n-1] but x stays: fine (zero advance) */
    return n;
}

static char *render(const char *s, const struct tl_line *l, char *buf)
{
    int k = 0;
    if (!l->ellipsis) { memcpy(buf, s + l->start, l->len); k = l->len; }
    else {
        memcpy(buf, s + l->start, l->cut_off - l->start); k = l->cut_off - l->start;
        memcpy(buf + k, "\xe2\x80\xa6", 3); k += 3;
        int t = l->start + l->len - (l->cut_off + l->cut_len);
        memcpy(buf + k, s + l->cut_off + l->cut_len, t); k += t;
    }
    buf[k] = 0; return buf;
}

static struct tl_para_opts opts(int cells, int maxl, int trunc)
{
    struct tl_para_opts o = {0};
    o.width24_8 = cells * 8 * 256; o.px = 16; o.line_height24_8 = 20 * 256;
    o.max_lines = maxl; o.trunc = trunc; return o;
}

static unsigned short soff[4096]; static int sx[4096];
static struct tl_line ln[256];

/* ---- brute-force greedy reference (no truncation) ---- */
static int ref_layout(const char *s, int len, int W, void *ctx, int *starts)
{
    static unsigned short ro[4096]; static int rx[4096]; static unsigned char brk[8200];
    int n = fake_carets(ctx, s, len, 16, 0, ro, rx, 4096); g_calls--;
    tl_break_utf8(s, len, brk, 0);
    int cnt = 0, si = 0;
    while (si < n - 1) {
        int best = -1, firstfit_any = -1, mand = n - 1;
        for (int k = si + 1; k < n; k++) if (brk[ro[k]] == TL_BRK_MANDATORY) { mand = k; break; }
        for (int k = si + 1; k <= mand; k++) {
            if (brk[ro[k]] == TL_BRK_NONE) continue;
            int t = k; while (t > si) {                /* trim trailing blank clusters */
                int bl = 1; for (int i = ro[t - 1]; i < ro[t]; i++) if (s[i] != ' ' && s[i] != '\n' && s[i] != '\r') bl = 0;
                if (!bl) break; t--; }
            if (rx[t] - rx[si] <= W) best = k;
        }
        (void)firstfit_any;
        if (best < 0) {                                /* over-long: farthest fitting cluster, min one */
            best = si + 1;
            for (int k = si + 1; k < n; k++) {
                int t = k; while (t > si) {
                    int bl = 1; for (int i = ro[t - 1]; i < ro[t]; i++) if (s[i] != ' ' && s[i] != '\n' && s[i] != '\r') bl = 0;
                    if (!bl) break; t--; }
                if (rx[t] - rx[si] <= W) best = k; else break;
            }
        }
        starts[cnt++] = ro[si]; si = best;
    }
    return cnt;
}

static int on_stop(const char *s, int len, int pos)    /* pos is a caret stop of s? */
{
    unsigned short o[4096]; int x[4096];
    int n = fake_carets(0, s, len, 16, 0, o, x, 4096); g_calls--;
    for (int i = 0; i < n; i++) if (o[i] == pos) return 1;
    return 0;
}
static int on_stop_w(const char *s, int len, int pos)
{
    unsigned short o[4096]; int x[4096];
    int n = fake_carets((void *)1, s, len, 16, 0, o, x, 4096); g_calls--;
    for (int i = 0; i < n; i++) if (o[i] == pos) return 1;
    return 0;
}

static void check_cuts(const char *name, const char *s, int len, const struct tl_line *l, int wide)
{
    int (*st)(const char *, int, int) = wide ? on_stop_w : on_stop;
    int pts[4] = { l->start, l->cut_off, l->cut_off + l->cut_len, l->start + l->len };
    for (int i = 0; i < 4; i++)
        CHECK("cluster-split", st(s, len, pts[i]), "%s: boundary %d (byte %d) is not a cluster stop", name, i, pts[i]);
}

static void run_layout(const char *name, const char *s, int cells, int maxl, int trunc, void *ctx,
                       struct tl_line *out, int *nout)
{
    struct tl_para_opts o = opts(cells, maxl, trunc);
    struct tl_measure_cb cb = { fake_carets, ctx };
    g_calls = 0;
    int n = tl_para_layout(s, (int)strlen(s), &o, &cb, out, 256, soff, sx, 4096);
    CHECK("layout-ok", n >= 1, "%s: returned %d", name, n);
    *nout = n;
    CHECK("calls<=2", g_calls <= 2, "%s: %d callback calls", name, g_calls);
}

static const char *texts[] = {
    "The quick brown fox jumps over the lazy dog and keeps running far away",
    "a b c d e f g h i j k l m n o p q r s t u v w x y z",
    "supercalifragilisticexpialidocious is long and so is antidisestablishmentarianism",
    "line one\nline two is a bit longer than one\n\nfourth after blank",
    "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88 mixed with latin words \xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf\xe4\xb8\x96\xe7\x95\x8c",
    "e\xcc\x81" "e\xcc\x81" "e\xcc\x81 cafe\xcc\x81 re\xcc\x81sume\xcc\x81 na\xcc\x88ive coo\xcc\x88perate",
    "trailing   spaces   everywhere   here   ",
    "crlf\r\nline\r\nend",
};

int main(void)
{
    char buf[1024];

    /* 1. wrap points == brute-force greedy; no line wider than the box */
    for (unsigned t = 0; t < sizeof texts / sizeof *texts; t++)
        for (int wide = 0; wide < 2; wide++)
            for (int cells = 3; cells <= 40; cells += 3) {
                const char *s = texts[t]; int len = (int)strlen(s);
                struct tl_line *o = ln; int n;
                char nm[64]; snprintf(nm, sizeof nm, "text%u wide%d w%d", t, wide, cells);
                run_layout(nm, s, cells, 0, TL_TRUNC_TAIL, wide ? (void *)1 : 0, o, &n);
                int rs[256]; int rn = ref_layout(s, len, cells * 8 * 256, wide ? (void *)1 : 0, rs);
                CHECK("wrap-ref", n == rn, "%s: %d lines, reference %d", nm, n, rn);
                int pos = 0;
                for (int i = 0; i < n && i < rn; i++) {
                    CHECK("wrap-ref", o[i].start == rs[i], "%s: line %d starts at %d, reference %d", nm, i, o[i].start, rs[i]);
                    CHECK("contiguous", o[i].start == pos, "%s: line %d gap/overlap", nm, i);
                    pos = o[i].start + o[i].len;
                    /* a single cluster wider than the box is the only excuse */
                    CHECK("width", o[i].width24_8 <= cells * 8 * 256 || o[i].len <= 4, "%s: line %d width %d > %d", nm, i, o[i].width24_8, cells * 8 * 256);
                    check_cuts(nm, s, len, &o[i], wide);
                }
                CHECK("contiguous", pos == len, "%s: lines end at %d of %d", nm, pos, len);
            }

    /* 2. mandatory breaks */
    {
        int n; run_layout("mand", "ab\ncd\n\nef", 40, 0, TL_TRUNC_TAIL, 0, ln, &n);
        CHECK("mandatory", n == 4, "got %d lines, want 4", n);
        CHECK("mandatory", n == 4 && ln[0].len == 3 && ln[0].width24_8 == 2 * 8 * 256, "line 0 len %d width %d", ln[0].len, ln[0].width24_8);
        CHECK("mandatory", n == 4 && ln[2].len == 1 && ln[2].width24_8 == 0, "blank line len %d", ln[2].len);
        run_layout("mand-crlf", "ab\r\ncd", 40, 0, TL_TRUNC_TAIL, 0, ln, &n);
        CHECK("mandatory", n == 2 && ln[0].len == 4, "crlf: %d lines len0 %d", n, ln[0].len);
        run_layout("empty", "", 10, 0, TL_TRUNC_TAIL, 0, ln, &n);
        CHECK("empty", n == 1 && ln[0].len == 0, "empty para %d", n);
    }

    /* 3. middle truncation of a path */
    {
        const char *p = "/very/long/path/name.txt"; int n;
        run_layout("middle", p, 12, 1, TL_TRUNC_MIDDLE, 0, ln, &n);
        CHECK("middle", n == 1 && ln[0].ellipsis == TL_ELL_MIDDLE, "n=%d ell=%d", n, ln[0].ellipsis);
        render(p, &ln[0], buf);
        CHECK("middle", strcmp(buf, "/very\xe2\x80\xa6me.txt") == 0, "got '%s'", buf);
        CHECK("width", ln[0].width24_8 <= 12 * 8 * 256, "width %d", ln[0].width24_8);
        check_cuts("middle", p, (int)strlen(p), &ln[0], 0);
        /* fits entirely: no ellipsis */
        run_layout("middle-fit", p, 30, 1, TL_TRUNC_MIDDLE, 0, ln, &n);
        CHECK("middle", n == 1 && ln[0].ellipsis == 0 && ln[0].len == 24, "fit: n=%d ell=%d", n, ln[0].ellipsis);
        /* wide characters and combining marks around the cut, at every width */
        const char *u = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e/e\xcc\x81" "cole/\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88/na\xcc\x88ive/\xe3\x81\xaf.txt";
        for (int mode = 0; mode < 3; mode++)
            for (int cells = 4; cells < 26; cells++) {
                char nm[48]; snprintf(nm, sizeof nm, "uni mode%d w%d", mode, cells);
                run_layout(nm, u, cells, 1, mode, (void *)1, ln, &n);
                check_cuts(nm, u, (int)strlen(u), &ln[0], 1);
                CHECK("width", ln[0].width24_8 <= cells * 8 * 256, "%s: width %d", nm, ln[0].width24_8);
            }
    }

    /* 4. head and tail */
    {
        const char *p = "/very/long/path/name.txt"; int n;
        run_layout("head", p, 12, 1, TL_TRUNC_HEAD, 0, ln, &n);
        render(p, &ln[0], buf);
        CHECK("head", ln[0].ellipsis == TL_ELL_HEAD && strcmp(buf, "\xe2\x80\xa6" "th/name.txt") == 0, "got '%s'", buf);
        run_layout("tail", p, 12, 1, TL_TRUNC_TAIL, 0, ln, &n);
        render(p, &ln[0], buf);
        CHECK("tail", ln[0].ellipsis == TL_ELL_TAIL && strcmp(buf, "/very/long/\xe2\x80\xa6") == 0, "got '%s'", buf);
    }

    /* 5. max_lines = 2 puts the ellipsis on line 2 */
    {
        const char *t = texts[0]; int n;
        run_layout("max2", t, 20, 2, TL_TRUNC_TAIL, 0, ln, &n);
        CHECK("max-lines", n == 2 && ln[0].ellipsis == 0 && ln[1].ellipsis == TL_ELL_TAIL, "n=%d ell0=%d ell1=%d", n, ln[0].ellipsis, n > 1 ? ln[1].ellipsis : -1);
        CHECK("width", n == 2 && ln[1].width24_8 <= 20 * 8 * 256, "line 2 width %d", ln[1].width24_8);
        int rs[256]; ref_layout(t, (int)strlen(t), 20 * 8 * 256, 0, rs);
        CHECK("max-lines", n == 2 && ln[1].start == rs[1], "line 2 starts at %d, reference %d", ln[1].start, rs[1]);
        run_layout("max2-none", t, 20, 2, TL_TRUNC_NONE, 0, ln, &n);
        CHECK("max-lines", n == 2 && ln[1].ellipsis == 0, "NONE: n=%d", n);
        /* text that fits in max_lines gets no ellipsis */
        run_layout("max2-fit", "short text here", 20, 2, TL_TRUNC_TAIL, 0, ln, &n);
        CHECK("max-lines", n == 1 && ln[0].ellipsis == 0, "fit: n=%d", n);
    }

    /* 6. callback budget: exactly 1 call when nothing is truncated, 2 when it is */
    {
        int n; run_layout("calls1", texts[0], 20, 0, TL_TRUNC_TAIL, 0, ln, &n);
        CHECK("calls<=2", g_calls == 1, "untruncated used %d calls", g_calls);
        run_layout("calls2", texts[0], 20, 1, TL_TRUNC_TAIL, 0, ln, &n);
        CHECK("calls<=2", g_calls == 2, "truncated used %d calls", g_calls);
    }

    /* 7. alignment and bad arguments */
    {
        int n; struct tl_para_opts o = opts(20, 0, TL_TRUNC_TAIL); struct tl_measure_cb cb = { fake_carets, 0 };
        o.align = TL_ALIGN_C;
        n = tl_para_layout("abcd", 4, &o, &cb, ln, 8, soff, sx, 64);
        CHECK("align", n == 1 && ln[0].x_off24_8 == (20 - 4) * 8 * 256 / 2, "center x_off %d", ln[0].x_off24_8);
        o.align = TL_ALIGN_R;
        n = tl_para_layout("abcd", 4, &o, &cb, ln, 8, soff, sx, 64);
        CHECK("align", n == 1 && ln[0].x_off24_8 == (20 - 4) * 8 * 256, "right x_off %d", ln[0].x_off24_8);
        CHECK("args", tl_para_layout("abcd", 4, &o, &cb, ln, 8, soff, sx, 3) == -1, "scratch too small accepted");
        o.width24_8 = 8 * 256;
        CHECK("args", tl_para_layout(texts[0], (int)strlen(texts[0]), &o, &cb, ln, 2, soff, sx, 4096) == -1, "out too small accepted");
        CHECK("args", tl_para_layout(0, 4, &o, &cb, ln, 8, soff, sx, 64) == -1, "NULL text accepted");
    }

    if (g_fail) { printf("%d check(s) failed\n", g_fail); return 1; }
    printf("text-paragraph: all checks passed\n");
    return 0;
}
