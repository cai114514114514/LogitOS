/* Host test for inline <svg> layout: the DOM parser records the element's
 * verbatim source span (raw/rawlen) and layout decodes it straight into an
 * IT_IMAGE item (case-preserved viewBox, path data longer than the 255-char
 * attr cap). Links the REAL svg decoder (img.c + svg.c), unlike layout_test
 * which stubs img_decode away. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "layout.h"
#include "css.h"
#include "dom.h"

/* --- stubs (kernel-only deps; image decode is the real thing) --- */
void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }
int text_measure(const char *s, int len, int px, int mono){ (void)s;(void)mono; return len * (px/2); }
int res_fetch(const char *url, uint8_t **buf, int *len){ (void)url;(void)buf;(void)len; return -1; }

static int fail;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n", msg); fail=1; } else printf("ok: %s\n", msg); }while(0)

static const struct item *find_img(int *idx)
{
    int n = layout_count();
    const struct item *it = layout_items();
    for (int i = 0; i < n; i++)
        if (it[i].type == IT_IMAGE) { if (idx) *idx = i; return &it[i]; }
    return 0;
}

static int count_imgs(void)
{
    int n = layout_count(), k = 0;
    const struct item *it = layout_items();
    for (int i = 0; i < n; i++) if (it[i].type == IT_IMAGE) k++;
    return k;
}

static struct node *find_tag(struct node *n, const char *tag)
{
    if (n->type == N_ELEM && !strcmp(n->tag, tag)) return n;
    for (struct node *c = n->first_child; c; c = c->next) {
        struct node *r = find_tag(c, tag);
        if (r) return r;
    }
    return 0;
}

/* path d longer than the DOM attr cap (255): the first ~290 chars draw
 * off-canvas, then an on-canvas filled square. Only the verbatim span can
 * rasterize anything. */
static char LONG_D[512];
static void build_long_d(void)
{
    strcpy(LONG_D, "M100 100 ");
    for (int i = 0; i < 70; i++) strcat(LONG_D, "h10 ");
    strcat(LONG_D, "M0 0L16 0L16 16L0 16Z");     /* ~310 chars total */
}

static int any_opaque(const struct image *im)
{
    for (long i = 0; i < (long)im->w * im->h; i++)
        if (im->rgba[i * 4 + 3]) return 1;
    return 0;
}

int main(void)
{
    build_long_d();

    /* 1. inline svg emits IT_IMAGE sized from the camelCase viewBox, decoded
     * pixels present (img != NULL, decode happened inline) */
    char html1[1024];
    snprintf(html1, sizeof html1,
        "<body><p>before</p>"
        "<svg viewBox=\"0 0 16 16\" fill=\"black\"><path d=\"%s\"/></svg>"
        "<p>after</p></body>", LONG_D);
    struct node *r1 = dom_parse(html1, (int)strlen(html1));
    CHECK(r1 != NULL, "svg dom_parse");
    struct node *svg1 = find_tag(r1, "svg");
    CHECK(svg1 && svg1->raw && svg1->rawlen > 300,
          "raw span recorded (long path survives >255 chars)");
    CHECK(svg1 && svg1->raw && memcmp(svg1->raw, "<svg ", 5) == 0 &&
          svg1->raw[svg1->rawlen - 1] == '>',
          "raw span covers '<svg' to the closing tag's '>'");
    css_apply(r1, 0, 0);
    layout_page(r1, 400);
    const struct item *im1 = find_img(0);
    CHECK(im1 != NULL, "inline svg emitted an IT_IMAGE item");
    CHECK(im1 && im1->img != NULL, "svg decoded inline (img attached, no fetch)");
    CHECK(im1 && im1->w == 16 && im1->h == 16, "viewBox 0 0 16 16 -> 16x16 box");
    CHECK(im1 && im1->img && any_opaque(im1->img),
          "path d past the 255-char attr cap rasterized (raw span, not DOM attr)");
    dom_free(r1);

    /* 2. CSS width/height beats viewBox */
    const char *html2 =
        "<body><svg class='ic' viewBox='0 0 16 16'>"
        "<path d='M0 0L16 0L16 16L0 16Z'/></svg></body>";
    const char *css2 = ".ic{width:32px;height:24px}";
    struct node *r2 = dom_parse(html2, (int)strlen(html2));
    CHECK(r2 != NULL, "css-size dom_parse");
    css_apply(r2, css2, (int)strlen(css2));
    layout_page(r2, 400);
    const struct item *im2 = find_img(0);
    CHECK(im2 && im2->w == 32 && im2->h == 24, "CSS width/height override viewBox");
    dom_free(r2);

    /* 3. width/height attributes beat viewBox; lone width keeps the aspect */
    const char *html3 =
        "<body>"
        "<svg width='20' height='10' viewBox='0 0 16 16'><path d='M0 0L16 0L16 16L0 16Z'/></svg>"
        "<svg width='8' viewBox='0 0 16 16'><path d='M0 0L16 0L16 16L0 16Z'/></svg>"
        "</body>";
    struct node *r3 = dom_parse(html3, (int)strlen(html3));
    CHECK(r3 != NULL, "attr-size dom_parse");
    css_apply(r3, 0, 0);
    layout_page(r3, 400);
    {
        int n = layout_count(), got = 0, w10 = 0, sq = 0;
        const struct item *it = layout_items();
        for (int i = 0; i < n; i++) {
            if (it[i].type != IT_IMAGE) continue;
            got++;
            if (it[i].w == 20 && it[i].h == 10) w10 = 1;
            if (it[i].w == 8 && it[i].h == 8) sq = 1;
        }
        CHECK(got == 2, "both attribute-sized svgs emitted");
        CHECK(w10, "width/height attrs override viewBox (20x10)");
        CHECK(sq, "lone width attr derives height from viewBox aspect (8x8)");
    }
    dom_free(r3);

    /* 4. nested svg: the outer element emits exactly one image; the inner one
     * stays inside the outer's raw span and is never flowed on its own */
    const char *html4 =
        "<body><svg viewBox='0 0 24 24'>"
        "<svg viewBox='0 0 16 16'><path d='M0 0L16 0L16 16L0 16Z'/></svg>"
        "</svg></body>";
    struct node *r4 = dom_parse(html4, (int)strlen(html4));
    CHECK(r4 != NULL, "nested dom_parse");
    css_apply(r4, 0, 0);
    layout_page(r4, 400);
    CHECK(count_imgs() == 1, "nested svg emits exactly one IT_IMAGE (depth counting)");
    const struct item *im4 = find_img(0);
    CHECK(im4 && im4->w == 24 && im4->h == 24, "nested svg sized by outer viewBox");
    dom_free(r4);

    /* 5. display:inline-block (GitHub .octicon) must not route the svg to the
     * empty block-box path; it still renders inline */
    const char *html5 =
        "<body><a href='/x'><svg class='oct' viewBox='0 0 16 16'>"
        "<path d='M0 0L16 0L16 16L0 16Z'/></svg></a></body>";
    const char *css5 = ".oct{display:inline-block}";
    struct node *r5 = dom_parse(html5, (int)strlen(html5));
    CHECK(r5 != NULL, "inline-block dom_parse");
    css_apply(r5, css5, (int)strlen(css5));
    layout_page(r5, 400);
    const struct item *im5 = find_img(0);
    CHECK(im5 && im5->w == 16 && im5->h == 16,
          "display:inline-block svg still emits IT_IMAGE");
    CHECK(im5 && im5->href && !strcmp(im5->href, "/x"), "svg icon inherits the link target");
    dom_free(r5);

    /* 6. robustness: truncated / malformed svg must not crash the parser or
     * the layout engine */
    const char *html6 = "<body><svg viewBox='0 0 16 16'><path d='M1";
    struct node *r6 = dom_parse(html6, (int)strlen(html6));
    CHECK(r6 != NULL, "truncated svg dom_parse");
    css_apply(r6, 0, 0);
    layout_page(r6, 400);
    CHECK(layout_height() >= 0, "truncated svg: layout survived");
    dom_free(r6);

    const char *html7 = "<body><svg viewBox='garbage'><path d='QQQ 1 2'/></svg><p>ok</p></body>";
    struct node *r7 = dom_parse(html7, (int)strlen(html7));
    CHECK(r7 != NULL, "garbage svg dom_parse");
    css_apply(r7, 0, 0);
    layout_page(r7, 400);
    CHECK(layout_height() >= 0, "garbage svg: layout survived");
    dom_free(r7);

    /* 7. svg inside a flex row counts its width, GitHub style: the label
     * comes first, the icon right after it on the same row */
    const char *html8 =
        "<body><div class='bar'>"
        "<button class='c'>Solutions</button>"
        "<button class='b'><svg viewBox='0 0 16 16'><path d='M0 0L16 0L16 16L0 16Z'/></svg></button>"
        "</div></body>";
    const char *css8 = ".bar{display:flex}";
    struct node *r8 = dom_parse(html8, (int)strlen(html8));
    CHECK(r8 != NULL, "flex svg dom_parse");
    css_apply(r8, css8, (int)strlen(css8));
    layout_page(r8, 400);
    {
        int n = layout_count(), sx = -1, sy = -1, tx = -1, ty = -1, tw = 0;
        const struct item *it = layout_items();
        for (int i = 0; i < n; i++) {
            if (it[i].type == IT_IMAGE) { sx = it[i].x; sy = it[i].y; }
            if (it[i].type == IT_TEXT && it[i].len == 9 && !memcmp(it[i].text, "Solutions", 9)) {
                tx = it[i].x; ty = it[i].y; tw = it[i].w;
            }
        }
        CHECK(sx >= 0, "flex: svg icon emitted");
        CHECK(tx >= 0 && sx >= tx + tw, "flex: icon placed after the label text");
        CHECK(sx >= 0 && sy == ty, "flex: icon and label share the row");
    }
    dom_free(r8);

    printf(fail ? "\nLAYOUT SVG TEST FAILED\n" : "\nLAYOUT SVG TEST PASSED\n");
    return fail;
}
