/* Exercise the actual HTML -> CSS cascade -> layout consumers. Numeric flex
 * tests cannot see a DOM bridge that discards basis, ratios or alignment.
 * The deterministic glyph/decoder oracles isolate geometry here; guest gates
 * settle actual fonts, image bytes and painted placement. */
#define img_decode intrinsic_unused_img_decode
#define img_free intrinsic_unused_img_free
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main
#undef img_decode
#undef img_free

int img_decode(const uint8_t *data, int len, struct image *image)
{
    if (len != 8) return -1;
    memcpy(&image->w, data, 4); memcpy(&image->h, data+4, 4);
    if (image->w <= 0 || image->w > 1024 || image->h <= 0 || image->h > 1024) return -1;
    image->rgba = calloc((size_t)image->w * image->h, 4);
    return image->rgba ? 0 : -1;
}
void img_free(struct image *image) { free(image->rgba); image->rgba = 0; }

static void fresh(const char *rules, const char *body)
{
    char html[12000];
    layout_free(); if (g_root) dom_free(g_root); g_root = 0;
    snprintf(html, sizeof html,
             "<style>body{margin:0;font-size:16px;line-height:20px}%s</style>%s", rules, body);
    page(html, 800);
}
static int coordinate(const char *id, int axis)
{
    int x=0, y=0, w=0, h=0;
    CHECK(layout_node_box(ID(id), &x, &y, &w, &h), "positioned box exists");
    return axis ? y : x;
}
static void image_cache(const char *url, int w, int h)
{
    int dims[2] = {w, h};
    CHECK(layout_img_store(url, (const unsigned char *)dims, sizeof dims), "cache accepts decoded dimension oracle");
}

static void ratio_cases(void)
{
    fresh("#card{width:240px;aspect-ratio:16/9}",
          "<div id=card></div><div id=next>Title</div>");
    EQ(height("card"), 135, "ratio: card aspect height");
    EQ(coordinate("next", 1), 135, "ratio: title follows actual reserved box");
    CHECK(!((struct cstyle *)ID("card")->style)->has_h, "ratio: used height leaves computed auto intact");
    layout_page(g_root, 800);
    EQ(height("card"), 135, "ratio: repeated layout is stable");
    fresh("#card{width:200px;aspect-ratio:2/1;padding:10px;border:2px solid;box-sizing:border-box}",
          "<div id=card></div>");
    EQ(width("card"), 200, "ratio: border sizing keeps authored width");
    EQ(height("card"), 100, "ratio: border ratio includes extras once");
    fresh("#card{width:200px;aspect-ratio:2/1;padding:10px;border:2px solid}", "<div id=card></div>");
    EQ(width("card"), 224, "ratio: content width includes extras");
    EQ(height("card"), 124, "ratio: content ratio excludes extras");
    fresh("#card{width:150px;aspect-ratio:1.5}", "<div id=card></div>");
    EQ(height("card"), 100, "ratio: fractional numerator is preserved");
    fresh("#card{width:100px;aspect-ratio:1/1}#tall{height:180px}",
          "<div id=card><div id=tall></div></div>");
    EQ(height("card"), 180, "ratio: automatic content minimum enlarges height");
    fresh("#card{width:100px;aspect-ratio:1/1;min-height:0}#tall{height:180px}",
          "<div id=card><div id=tall></div></div>");
    EQ(height("card"), 100, "ratio: explicit zero minimum permits overflow");
    EQ(height("tall"), 180, "ratio: overflow content keeps its geometry");
    fresh("#card{width:100px;aspect-ratio:1/1;overflow:auto}#tall{height:180px}",
          "<div id=card><div id=tall></div></div>");
    EQ(height("card"), 100, "ratio: scroll container keeps ratio height");
    fresh("#card{width:100px;aspect-ratio:1/1;max-height:80px}", "<div id=card></div>");
    EQ(height("card"), 80, "ratio: max height bounds preferred height");
    fresh("#card{height:60px;aspect-ratio:2/1}", "<div id=card></div>");
    EQ(width("card"), 120, "ratio: definite height transfers to automatic block width");
    fresh("#card{height:60px;aspect-ratio:2/1}#wide{display:inline-block;width:150px;height:10px}",
          "<div id=card><span id=wide></span></div>");
    EQ(width("card"), 150, "ratio: automatic content minimum enlarges width");
    fresh("#card{height:60px;aspect-ratio:2/1;min-width:0}#wide{display:inline-block;width:150px;height:10px}",
          "<div id=card><span id=wide></span></div>");
    EQ(width("card"), 120, "ratio: explicit zero width minimum permits overflow");
    fresh("#card{display:inline-block;height:40px;aspect-ratio:2/1}",
          "<div><span id=card></span></div>");
    EQ(width("card"), 80, "ratio: atomic inline uses transferred width");
    EQ(height("card"), 40, "ratio: atomic inline retains definite height");
    fresh("#card{width:100px;height:30px;aspect-ratio:2/1}", "<div id=card></div>");
    EQ(height("card"), 30, "ratio: two definite dimensions override ratio");
    fresh("#card{width:160px;aspect-ratio:16/9}#pct{height:50%}",
          "<div id=card><div id=pct></div></div>");
    EQ(height("pct"), 45, "ratio: percentage child sees ratio-derived definite height");
    fresh("#row{display:flex;align-items:flex-start;width:300px}#card{height:100px;aspect-ratio:1.5;min-width:0}",
          "<div id=row><div id=card></div></div>");
    EQ(width("card"), 150, "ratio: flex transfers fractional preferred cross size");
    fresh("#row{display:flex;align-items:flex-start;width:300px}#card{width:150px;aspect-ratio:1.5;min-height:0}",
          "<div id=row><div id=card></div></div>");
    EQ(height("card"), 100, "ratio: flex cross size follows allocated main size");
    fresh("#row{display:flex;align-items:flex-start;width:300px}#card{height:100px;aspect-ratio:2/1;box-sizing:border-box;padding:10px;border:2px solid;min-width:0}",
          "<div id=row><div id=card></div></div>");
    EQ(width("card"), 200, "ratio: flex transfer uses authored border ratio");
    EQ(height("card"), 100, "ratio: flex border transfer retains preferred height");
    fresh("#row{display:flex;align-items:flex-start;width:300px}#card{height:100px;aspect-ratio:1000000000000/1;max-width:120px;min-width:0}",
          "<div id=row><div id=card></div></div>");
    EQ(width("card"), 120, "ratio: extreme transfer is bounded before integer conversion");
    fresh("#grid{display:grid;grid-template-columns:160px}#card{aspect-ratio:16/9}",
          "<div id=grid><div id=card></div></div>");
    EQ(width("card"), 160, "ratio: grid normal uses block inline width");
    EQ(height("card"), 90, "ratio: grid normal preserves preferred ratio");
    fresh("#grid{display:grid;grid-template-columns:160px;grid-template-rows:120px;align-items:stretch}#card{aspect-ratio:16/9}",
          "<div id=grid><div id=card></div></div>");
    EQ(height("card"), 120, "ratio: explicit grid stretch may distort ratio");
    image_cache("ratio-photo", 320, 160);
    fresh("#photo{display:block;width:120px;aspect-ratio:1/1}", "<img id=photo src=ratio-photo>");
    EQ(height("photo"), 120, "ratio: authored replaced ratio overrides natural ratio");
    fresh("#photo{display:block;aspect-ratio:1000/1;min-height:10px}","<img id=photo src=ratio-photo>");
    EQ(height("photo"),10,"ratio: fractional intrinsic height still honors its minimum");
    EQ(width("photo"),10000,"ratio: compatible automatic minimum transfers without early rounding");
    fresh("#photo{display:block;aspect-ratio:1000/1;min-height:10px;box-sizing:border-box;padding:2px;border:1px solid}","<img id=photo src=ratio-photo>");
    EQ(height("photo"),10,"ratio: automatic border minimum excludes fixed padding from scaling");
    EQ(width("photo"),10000,"ratio: automatic border ratio keeps its determining box");
    fresh("#photo{display:block;width:120px;aspect-ratio:auto 1/1}", "<img id=photo src=ratio-photo>");
    EQ(height("photo"), 60, "ratio: auto replaced ratio uses actual decode metadata");
    const struct image *im = layout_img_get("ratio-photo");
    CHECK(im && im->w == 320 && im->h == 160, "cache: borrowed pixels have real decoded dimensions");
    CHECK(!layout_img_get("never-fetched"), "cache: unknown image lookup does not fabricate pixels");
    fresh("#photo{display:block;width:120px;aspect-ratio:auto 2/1}", "<img id=photo src=ratio-pending>");
    EQ(height("photo"), 60, "ratio: pending image reserves authored fallback ratio");
    fresh("#card{width:100px;aspect-ratio:0/1}", "<div id=card></div>");
    EQ(height("card"), 0, "ratio: degenerate ratio behaves as auto");
}

static void column_cases(void)
{
    fresh("#col{display:flex;flex-direction:column;width:240px}#a{flex-basis:60px}#b{flex-basis:90px}",
          "<div id=col><div id=a></div><div id=b></div></div>");
    EQ(height("a"), 60, "column: auto basis occupies main axis");
    EQ(height("b"), 90, "column: second auto basis occupies main axis");
    EQ(height("col"), 150, "column: indefinite container sums hypothetical sizes");
    EQ(coordinate("b", 1), 60, "column: basis moves following item");
    fresh("#col{display:flex;flex-direction:column;width:240px}#a{flex-basis:0%;min-height:0}#pct{height:50%}#natural{height:30px}",
          "<div id=col><div id=a><div id=pct></div><div id=natural></div></div></div>");
    EQ(height("a"), 30, "column: indefinite percentage basis uses content");
    EQ(height("pct"), 0, "column: content main size does not fabricate percentage basis");
    fresh("#col{display:flex;flex-direction:column;flex-wrap:wrap;align-content:flex-start;width:300px;height:100px}#a,#b,#c{width:80px;flex-basis:60px;min-height:0}",
          "<div id=col><div id=a></div><div id=b></div><div id=c></div></div>");
    EQ(height("a"), 60, "column: wrap retains item basis");
    EQ(coordinate("b", 0), 80, "column: wrap creates second column");
    EQ(coordinate("c", 0), 160, "column: wrap creates third column");
    EQ(coordinate("b", 1), 0, "column: wrapped item restarts main axis");
}

static void grid_cases(void)
{
    fresh("#grid{display:grid;grid-template-columns:auto auto;width:300px;justify-content:start}#a,#b{width:20px;height:10px}",
          "<div id=grid><div id=a></div><div id=b></div></div>");
    EQ(coordinate("b", 0), 20, "grid: explicit start keeps auto track content width");
    fresh("#grid{display:grid;grid-template-columns:auto auto;width:300px}#a,#b{width:20px;height:10px}",
          "<div id=grid><div id=a></div><div id=b></div></div>");
    EQ(coordinate("b", 0), 150, "grid: default normal stretches auto tracks");
    fresh("#grid{display:grid;grid-template-columns:auto auto;width:300px;justify-content:stretch}#a,#b{width:20px;height:10px}",
          "<div id=grid><div id=a></div><div id=b></div></div>");
    EQ(coordinate("b", 0), 150, "grid: explicit stretch stretches auto tracks");
}

static void grid_bounds_cases(void)
{
    /* The guest specimen uses MINIMUM-only empty items. Specifying width in
     * this host mirror would skip the failing intrinsic-contribution bridge. */
    fresh("#grid{display:grid;grid-template-columns:auto auto;width:180px;justify-content:start}#grid>div{height:20px;min-width:20px;background:red}",
          "<div id=grid><div id=a></div><div id=b></div></div>");
    EQ(width("a"), 20, "grid: auto track includes minimum-only empty item");
    EQ(coordinate("b", 0), 20, "grid: minimum contribution moves following auto track");
    fresh("#grid{display:grid;grid-template-columns:auto auto;width:180px}#grid>div{height:20px;min-width:20px;background:red}",
          "<div id=grid><div id=a></div><div id=b></div></div>");
    EQ(width("a"), 90, "grid: normal still stretches minimum-only auto tracks");
    fresh("#grid{display:grid;grid-template-columns:10px 10px;width:80px;justify-content:start}#grid>div{height:20px;min-width:20px}",
          "<div id=grid><div id=a></div><div id=b></div></div>");
    EQ(width("a"), 20, "grid: placed minimum may exceed fixed track");
    EQ(coordinate("b", 0), 10, "grid: item minimum does not enlarge explicit fixed track");
    fresh("#grid{display:grid;grid-template-columns:100px;width:100px}#a{height:20px;max-width:30px}",
          "<div id=grid><div id=a></div></div>");
    EQ(width("a"), 30, "grid: placed maximum constrains automatic stretch");
    fresh("#grid{display:grid;grid-template-columns:100px;width:100px}#a{height:20px;min-width:40px;max-width:30px}",
          "<div id=grid><div id=a></div></div>");
    EQ(width("a"), 40, "grid: minimum wins over contradictory maximum");
    fresh("#grid{display:grid;grid-template-columns:auto;width:100px;justify-content:start}#a{height:20px;min-width:20px;padding:2px;border:1px solid}",
          "<div id=grid><div id=a></div></div>");
    EQ(width("a"), 26, "grid: intrinsic minimum includes content-box extras once");
    fresh("#grid{display:grid;grid-template-columns:auto;width:100px;justify-content:start}#a{height:20px;min-width:20px;padding:2px;border:1px solid;box-sizing:border-box}",
          "<div id=grid><div id=a></div></div>");
    EQ(width("a"), 20, "grid: intrinsic border minimum excludes duplicate extras");
    fresh("#grid{display:grid;grid-template-columns:40px;width:80px}#a{height:20px;min-width:75%}",
          "<div id=grid><div id=a></div></div>");
    EQ(width("a"), 40, "grid: percentage minimum uses area rather than container");
    fresh("#grid{display:grid;grid-template-columns:40px;width:80px}#a{height:20px;max-width:50%}",
          "<div id=grid><div id=a></div></div>");
    EQ(width("a"), 20, "grid: percentage maximum resolves against actual area");
    fresh("#grid{display:grid;grid-template-columns:40px;width:80px;justify-items:center}#a{height:20px;min-width:20px}",
          "<div id=grid><div id=a></div></div>");
    EQ(width("a"), 20, "grid: centered fit-content honors explicit minimum");
    EQ(coordinate("a", 0), 10, "grid: alignment uses clamped placed width");
    fresh("#grid{display:grid;grid-template-columns:40px;width:80px;align-content:start}#a{min-height:20px}",
          "<div id=grid><div id=a></div></div>");
    EQ(height("a"), 20, "grid: auto row includes minimum-only empty item");
    EQ(height("grid"), 20, "grid: minimum-only row contributes to container height");
    fresh("#grid{display:grid;grid-template-columns:40px;grid-template-rows:10px;width:40px;align-content:start}#a{min-height:20px;background:red}#next{height:5px}",
          "<div id=grid><div id=a></div></div><div id=next></div>");
    EQ(height("a"), 20, "grid: placed minimum may exceed fixed row");
    EQ(height("grid"), 10, "grid: child overflow does not enlarge used track height");
    EQ(coordinate("next", 1), 10, "grid: following sibling uses tracks rather than child overflow");
    int scroll_w=0, scroll_h=0;
    CHECK(layout_node_scroll(ID("grid"), &scroll_w, &scroll_h), "grid: overflow extent exists");
    EQ(scroll_h, 20, "grid: overflowing minimum remains scrollable extent");
    fresh("#grid{display:grid;grid-template-columns:40px;grid-template-rows:10px;width:40px;align-content:start}#a{height:20px;background:red}#next{height:5px}",
          "<div id=grid><div id=a></div></div><div id=next></div>");
    EQ(height("a"), 20, "grid: definite oversized item retains its own height");
    EQ(height("grid"), 10, "grid: definite overflow retains solved track height");
    EQ(coordinate("next", 1), 10, "grid: definite overflow leaves following flow position unchanged");
    fresh("#grid{display:grid;grid-template-columns:40px;grid-template-rows:100px;width:40px}#a{max-height:30px}",
          "<div id=grid><div id=a></div></div>");
    EQ(height("a"), 30, "grid: placed maximum constrains row stretch");
    EQ(height("grid"), 100, "grid: maximum item size does not shrink explicit row");
    fresh("#grid{display:grid;grid-template-columns:40px;grid-template-rows:100px;width:40px}#a{min-height:40px;max-height:30px}",
          "<div id=grid><div id=a></div></div>");
    EQ(height("a"), 40, "grid: height minimum wins over contradictory maximum");
    fresh("#grid{display:grid;grid-template-columns:40px;grid-template-rows:40px;width:40px;height:80px}#a{height:50%}",
          "<div id=grid><div id=a></div></div>");
    EQ(height("a"), 20, "grid: percentage height uses actual row area");
    fresh("#grid{display:grid;grid-template-columns:40px;grid-template-rows:40px;width:40px;height:80px}#a{min-height:75%}",
          "<div id=grid><div id=a></div></div>");
    EQ(height("a"), 40, "grid: percentage height minimum avoids whole-container basis");
    fresh("#grid{display:grid;grid-template-columns:40px;grid-template-rows:40px;width:40px;height:80px}#a{max-height:50%}",
          "<div id=grid><div id=a></div></div>");
    EQ(height("a"), 20, "grid: percentage height maximum resolves against area");
    fresh("#grid{display:grid;grid-template-columns:40px;grid-template-rows:100px;width:40px;align-items:center}#a{min-height:20px}",
          "<div id=grid><div id=a></div></div>");
    EQ(height("a"), 20, "grid: centered fit-content honors height minimum");
    EQ(coordinate("a", 1), 40, "grid: vertical alignment uses clamped height");
    fresh("#grid{display:grid;grid-template-columns:40px;grid-template-rows:40px;width:40px}#a{height:0}",
          "<div id=grid><div id=a></div></div>");
    EQ(height("a"), 0, "grid: definite zero height suppresses stretching");
}

int main(int argc, char **argv)
{
    css_init(); css_viewport(800, 600);
    if (argc == 1 || !strcmp(argv[1], "ratio")) ratio_cases();
    if (argc == 1 || !strcmp(argv[1], "column")) column_cases();
    if (argc == 1 || !strcmp(argv[1], "grid")) grid_cases();
    if (argc == 1 || !strcmp(argv[1], "grid-bounds")) grid_bounds_cases();
    layout_free(); if (g_root) dom_free(g_root);
    printf("css-layout: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
