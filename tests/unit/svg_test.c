/* SVG decoder host test: embedded cases (no asset files). Covers the real
 * GitHub octicon mark path (viewBox scaling, C/S/A commands), rect/circle/
 * ellipse, g fill inheritance, fill-rule evenodd, opacity, color syntaxes,
 * content sniffing (<?xml, whitespace, HTML rejection), and truncated /
 * malformed input robustness (must return cleanly, never crash). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "img.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

static int fail;
static void ok(int cond, const char *msg)
{
    if (!cond) { printf("FAIL %s\n", msg); fail = 1; }
}
static void px(struct image *im, int x, int y, int r, int g, int b, int a, const char *msg)
{
    uint8_t *p = im->rgba + ((long)y * im->w + x) * 4;
    if (p[0] != r || p[1] != g || p[2] != b || p[3] != a) {
        printf("FAIL %s px(%d,%d)=%d,%d,%d,%d want %d,%d,%d,%d\n",
               msg, x, y, p[0], p[1], p[2], p[3], r, g, b, a);
        fail = 1;
    }
}
static int dec(const char *s, struct image *im) { return img_decode((const uint8_t *)s, strlen(s), im); }
static int nonbg(struct image *im)
{
    int n = 0;
    for (long i = 0; i < (long)im->w * im->h; i++) if (im->rgba[i * 4 + 3]) n++;
    return n;
}

/* Real GitHub octicon mark-github-16 path (viewBox 0 0 16 16). Uses
 * M/C/S/A/V/Z + relative variants -- the actual bytes GitHub serves. */
static const char *MARK_D =
    "M6.766 11.328c-2.063-.25-3.516-1.734-3.516-3.656 0-.781.281-"
    "1.625.75-2.188-.203-.515-.172-1.609.063-2.062.625-.078 1.468"
    ".25 1.968.703.594-.187 1.219-.281 1.985-.281.765 0 1.39.094 "
    "1.953.265.484-.437 1.344-.765 1.969-.687.218.422.25 1.515.04"
    "6 2.047.5.593.766 1.39.766 2.203 0 1.922-1.453 3.375-3.547 3"
    ".64.531.344.89 1.094.89 1.954v1.625c0 .468.391.734.86.547C13"
    ".781 14.359 16 11.53 16 8.03 16 3.61 12.406 0 7.984 0 3.563 "
    "0 0 3.61 0 8.031a7.88 7.88 0 0 0 5.172 7.422c.422.156.828-.1"
    "25.828-.547v-1.25c-.219.094-.5.156-.75.156-1.031 0-1.64-.562"
    "-2.078-1.609-.172-.422-.36-.672-.719-.719-.187-.015-.25-.093"
    "-.25-.187 0-.188.313-.328.625-.328.453 0 .844.281 1.25.86.31"
    "3.452.64.655 1.031.655s.641-.14 1-.5c.266-.265.47-.5.657-.65"
    "6";

int main(void)
{
    struct image im;

    /* 1: plain rect, #rrggbb fill */
    ok(!dec("<svg width=\"4\" height=\"4\"><rect width=\"4\" height=\"4\" fill=\"#ff0000\"/></svg>", &im), "rect decode");
    ok(im.w == 4 && im.h == 4, "rect size");
    px(&im, 0, 0, 255, 0, 0, 255, "rect");
    px(&im, 3, 3, 255, 0, 0, 255, "rect");
    img_free(&im);

    /* 2: viewBox -> viewport scaling + H/V/Z path commands + currentColor */
    ok(!dec("<svg viewBox=\"0 0 4 4\" width=\"8\" height=\"8\" fill=\"currentColor\">"
            "<path d=\"M0 0h4v4H0z\"/></svg>", &im), "scale decode");
    ok(im.w == 8 && im.h == 8, "scale size");
    px(&im, 4, 4, 0, 0, 0, 255, "scale center");
    px(&im, 7, 7, 0, 0, 0, 255, "scale corner");
    img_free(&im);

    /* 3: the GitHub mark at 32px, uniform #24292f fill */
    {
        char buf[2048];
        snprintf(buf, sizeof buf,
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 16\" width=\"32\" height=\"32\">"
            "<path fill=\"#24292f\" d=\"%s\"/></svg>", MARK_D);
        ok(!dec(buf, &im), "mark decode");
        ok(im.w == 32 && im.h == 32, "mark size");
        px(&im, 16, 4, 36, 41, 47, 255, "mark ring ink");
        px(&im, 16, 16, 0, 0, 0, 0, "mark cat hole transparent");
        px(&im, 0, 0, 0, 0, 0, 0, "mark corner transparent");
        int nb = nonbg(&im);
        printf("mark nonbg=%d/1024\n", nb);
        ok(nb > 400 && nb < 1000, "mark ink coverage sane");
        img_free(&im);
    }

    /* 4: circle + rgb() functional color */
    ok(!dec("<svg width=\"8\" height=\"8\"><circle cx=\"4\" cy=\"4\" r=\"4\" fill=\"rgb(0,128,255)\"/></svg>", &im), "circle decode");
    px(&im, 4, 4, 0, 128, 255, 255, "circle center");
    px(&im, 0, 0, 0, 0, 0, 0, "circle corner transparent");
    img_free(&im);

    /* 5: fill-rule evenodd vs nonzero on nested subpaths */
    ok(!dec("<svg width=\"8\" height=\"8\"><path fill-rule=\"evenodd\" fill=\"#0000ff\" "
            "d=\"M0 0h8v8H0z M2 2h4v4H2z\"/></svg>", &im), "evenodd decode");
    px(&im, 1, 1, 0, 0, 255, 255, "evenodd outer");
    px(&im, 4, 4, 0, 0, 0, 0, "evenodd hole");
    img_free(&im);
    ok(!dec("<svg width=\"8\" height=\"8\"><path fill=\"#0000ff\" "
            "d=\"M0 0h8v8H0z M2 2h4v4H2z\"/></svg>", &im), "nonzero decode");
    px(&im, 4, 4, 0, 0, 255, 255, "nonzero same-winding filled");
    img_free(&im);

    /* 6: <g> inheritance + fill-opacity + #rgb shorthand */
    ok(!dec("<svg width=\"2\" height=\"2\"><g fill=\"#0f0\" fill-opacity=\"0.5\">"
            "<rect width=\"2\" height=\"2\"/></g></svg>", &im), "g decode");
    px(&im, 0, 0, 0, 255, 0, 127, "g inherited half-alpha green");
    img_free(&im);

    /* 7: unsupported stuff (defs/style/transform/text) is skipped, not fatal */
    ok(!dec("<svg width=\"2\" height=\"2\"><defs><rect width=\"2\" height=\"2\" fill=\"red\"/></defs>"
            "<style>.a{fill:#fff}</style><g transform=\"rotate(45)\">"
            "<rect x=\"0\" y=\"0\" width=\"2\" height=\"2\" fill=\"blue\"/></g>"
            "<text x=\"0\" y=\"1\">hi</text></svg>", &im), "unsupported skipped");
    px(&im, 1, 1, 0, 0, 255, 255, "only real rect drawn");
    img_free(&im);

    /* 8: sniffing -- <?xml decl, doctype, leading whitespace; HTML rejected */
    ok(!dec("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!-- comment -->\n<svg width=\"1\" height=\"1\"><rect width=\"1\" height=\"1\" fill=\"red\"/></svg>", &im),
            "xml decl decode");
    img_free(&im);
    ok(!dec("  \n\t<svg width=\"1\" height=\"1\"/>", &im), "ws-only svg decode");
    img_free(&im);
    ok(dec("<!DOCTYPE html><html><body><svg width=\"4\" height=\"4\"></svg></body></html>", &im),
            "html doc rejected");
    ok(dec("\x89PNG\r\n\x1a\nnot really", &im) == 0 ? 0 : 1, "png bytes not claimed by svg");

    /* 9: unknown path command stops the path but stays stable */
    ok(!dec("<svg width=\"4\" height=\"4\"><path fill=\"#123456\" d=\"M0 0h4v4R2 2z\"/></svg>", &im), "unknown cmd tolerated");
    img_free(&im);

    /* 10: truncation fuzz -- every prefix of a real document must decode or
     * fail cleanly, never crash (run under ASan for the memory checks). */
    {
        char buf[2048];
        int len = snprintf(buf, sizeof buf,
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 16\" width=\"32\" height=\"32\">"
            "<g fill=\"#24292f\"><path d=\"%s\"/><circle cx=\"8\" cy=\"8\" r=\"3\"/>"
            "<rect x=\"1\" y=\"1\" width=\"2\" height=\"2\"/></g></svg>", MARK_D);
        for (int cut = 0; cut <= len; cut += 7) {
            int r = img_decode((const uint8_t *)buf, cut, &im);
            if (!r) img_free(&im);
        }
        printf("truncation sweep done (%d bytes)\n", len);
    }

    /* 11: garbage that sniffs like svg but isn't parseable */
    ok(dec("<svg", &im), "bare <svg fails");
    ok(dec("<svg x", &im), "truncated tag fails");
    ok(!dec("<svg width=\"4\" height=\"4\"><path d=\"M", &im), "truncated path partial render");
    img_free(&im);
    ok(!dec("<svg width=\"999999\" height=\"999999\"><rect width=\"999999\" height=\"999999\"/></svg>", &im),
            "huge dims clamped");
    ok(im.w <= 2048 && im.h <= 2048, "huge dims clamped size");
    img_free(&im);

    printf(fail ? "SOME FAILED\n" : "ALL PASS\n");
    return fail;
}
