/* UI font kerning test: GPOS `kern` survives subsetting in ui.ttf / ui-bold.ttf.
 *
 * Shapes through shape_line() (the single measure+draw entry point) at
 * px == units_per_em so widths are in font units.  Checks:
 *   1. width("AV") < width("A") + width("V"), likewise "To"  -> FAIL kern AV
 *   2. CJK / fullwidth-punctuation strings keep their committed widths, so
 *      the kern-only GPOS cannot move CJK text.
 * Usage: font_ui_kern_test UI.ttf UI-BOLD.ttf [--print]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shape.h"
#include "ttf.h"

#define MAXCP 64
#define MAXG  256
#define MAXRUN 16
static uint32_t cps[MAXCP]; static uint8_t lv[MAXCP]; static int ord[MAXCP];
static struct text_run runs[MAXRUN]; static struct shape_glyph gl[MAXG];
static uint8_t bidi[16 * 1024];

static int fails, checks;

static uint8_t *load(const char *p, int *len)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read %s\n", p); exit(2); }
    fclose(f); *len = (int)n; return b;
}

static int width(const struct ttf_font *f, const char *s)
{
    struct shape_font_set fs = { { f }, 1 };
    struct shape_scratch sc = { cps, lv, ord, gl, runs, bidi, MAXCP, MAXG, MAXRUN, (int)sizeof bidi };
    return shape_line(&fs, s, (int)strlen(s), f->units_per_em, 0, 0, NULL, &sc);
}

static const char *kpairs[][2] = { {"A", "V"}, {"T", "o"} };
static const char *knames[] = { "AV", "To" };

/* regression strings; widths [0]=regular [1]=bold, font units at upem 1000 */
static const struct { const char *s; int w[2]; } cjk[] = {
    { "中文", { 2000, 2000 } },
    { "你好世界", { 4000, 4000 } },
    { "，。、！？", { 5000, 5000 } },
    { "「测试」文本", { 6000, 6000 } },
    { "汉字ＡＢＣ１２３", { 8000, 8000 } },
    { "《标题》——说明…", { 8788, 8816 } },
    { "ｱｲｳ", { 1500, 1500 } },
};

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: font_ui_kern_test UI.ttf BOLD.ttf [--print]\n"); return 2; }
    int print = argc > 3 && !strcmp(argv[3], "--print");
    for (int w = 0; w < 2; w++) {
        int len; uint8_t *blob = load(argv[1 + w], &len);
        struct ttf_font f;
        if (ttf_parse(blob, len, &f) != 0) { fprintf(stderr, "%s does not parse\n", argv[1 + w]); return 2; }
        const char *face = w ? "bold" : "regular";
        for (unsigned k = 0; k < 2; k++) {
            int pair = width(&f, knames[k]);
            int sum = width(&f, kpairs[k][0]) + width(&f, kpairs[k][1]);
            checks++;
            if (!(pair < sum)) {
                printf("FAIL kern %s (%s): pair=%d sum=%d\n", knames[k], face, pair, sum);
                fails++;
            }
        }
        for (unsigned i = 0; i < sizeof cjk / sizeof cjk[0]; i++) {
            int got = width(&f, cjk[i].s);
            if (print) { printf("%s %s %d\n", face, cjk[i].s, got); continue; }
            checks++;
            if (got != cjk[i].w[w]) {
                printf("FAIL cjk %s (%s): width=%d expected %d\n", cjk[i].s, face, got, cjk[i].w[w]);
                fails++;
            }
        }
    }
    printf("font_ui_kern_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
