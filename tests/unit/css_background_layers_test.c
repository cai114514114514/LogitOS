/* Reuse the companion's device-pixel recorder, not its assertions or its
 * decoder refusal. DOM, CSS cascade, background discovery, cache and painter
 * are production consumers. This small host decoder accepts only the fixture's
 * uncompressed 24-bit BMP; the guest uses the shipping image codec on the SAME
 * bytes. Neither a parsed layer nor a draw count stands in for a pixel. */
#define main background_companion_main
#define img_decode background_unused_decode
#define img_free background_unused_free
#include "css_background_test.c"
#undef img_free
#undef img_decode
#undef main
#include "css/background.h"
#include "../../include/weaksym.h"
int layout_img_cached(const char *);
void layout_images_reset(void);
#include "../../c/apps/gui/browser/app/background.c"

static unsigned little16(const unsigned char *p)
{ return (unsigned)p[0] | (unsigned)p[1] << 8; }
static unsigned little32(const unsigned char *p)
{ return little16(p) | little16(p + 2) << 16; }

int img_decode(const uint8_t *data, int len, struct image *out)
{
    if (!data || len < 54 || data[0] != 'B' || data[1] != 'M' ||
        little32(data + 14) != 40 || little32(data + 18) != 80 ||
        little32(data + 22) != 40 || little16(data + 26) != 1 ||
        little16(data + 28) != 24 || little32(data + 30) != 0) return -1;
    unsigned offset = little32(data + 10);
    if (offset > (unsigned)len || (unsigned)len - offset < 80 * 40 * 3) return -1;
    memset(out, 0, sizeof *out);
    out->w = 80; out->h = 40;
    out->rgba = malloc(80 * 40 * 4);
    if (!out->rgba) return -1;
    for (int y = 0; y < 40; y++) for (int x = 0; x < 80; x++) {
        const unsigned char *source = data + offset + ((39 - y) * 80 + x) * 3;
        unsigned char *pixel = out->rgba + (y * 80 + x) * 4;
        pixel[0] = source[2]; pixel[1] = source[1]; pixel[2] = source[0]; pixel[3] = 255;
    }
    return 0;
}
void img_free(struct image *image)
{ free(image->rgba); image->rgba = NULL; }

static char *read_file(const char *name, int *length)
{
    FILE *file = fopen(name, "rb");
    if (!file) { perror(name); return NULL; }
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long size = ftell(file);
    if (size < 0 || size > 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file); return NULL;
    }
    char *text = malloc((size_t)size + 1);
    if (!text || fread(text, 1, (size_t)size, file) != (size_t)size) {
        free(text); fclose(file); return NULL;
    }
    fclose(file); text[size] = 0; *length = (int)size; return text;
}

static int front_fetches, back_fetches, second_fetches, missing_fetches, bad_fetches;
static int defer_front;
static int fetch_bitmap(const char *url, unsigned char **data, int *len)
{
    *data = NULL; *len = 0;
    if (!strcmp(url, "assets/stripes.bmp?layer=front")) {
        front_fetches++;
        if (defer_front) { defer_front = 0; return 1; }
    } else if (!strcmp(url, "assets/stripes.bmp?layer=back")) back_fetches++;
    else if (!strcmp(url, "assets/stripes.bmp?layer=second")) second_fetches++;
    else if (!strcmp(url, "assets/missing-layer.bmp")) { missing_fetches++; return -1; }
    else { bad_fetches++; return -1; }
    *data = (unsigned char *)read_file("tests/fixtures/engine-expansion/assets/stripes.bmp", len);
    return *data ? 0 : -1;
}

static void render_page(struct node *root, const char *css)
{
    css_apply(root, css, (int)strlen(css));
    css_extra_apply(root, css, (int)strlen(css));
    layout_page(root, W);
    gui_clip(0, 0, W, H);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) pixels[y][x] = 0xffffff;
    browser_paint(0, 0, W, H, 0);
}

static int supported(const char *property, const char *value)
{ return css_background_supports(property, (int)strlen(property), value, (int)strlen(value)); }

static void grammar(void)
{
    check(supported("background-image", "none, linear-gradient(red,blue), url('a)b,c.bmp')"),
          "layer splitter keeps quoted commas and parentheses inside the URL");
    check(supported("background", "url(a) right 3px bottom 4px / 20px 10px no-repeat content-box border-box, linear-gradient(red,blue) white"),
          "complete URL and gradient shorthand validates before producing any slots");
    check(!supported("background", "red url(a), url(b)"),
          "a non-final shorthand layer cannot contain a background color");
    check(!supported("background", "url(a), linear-gradient(red,blue) / -1px blue"),
          "invalid shorthand suffix refuses the whole declaration");
    check(!supported("background-size", "20px 10px, banana"),
          "unused list suffix still participates in declaration validity");
    check(!supported("background-image", "linear-gradient(red,blue), banana"),
          "invalid later image does not salvage a supported first image");
    check(!supported("background-image", "url(a),"),
          "empty last image layer is invalid");
    check(!supported("background", "url(a) fixed, linear-gradient(red,blue)"),
          "unsupported fixed attachment refuses the whole shorthand");
    check(!supported("background-attachment", "scroll, local"),
          "unsupported local attachment is not silently changed to scroll");
    char value[200] = "none";
    for (int i = 1; i < BG_MAX_LAYERS; i++) strcat(value, ",none");
    check(supported("background-image", value), "declared layer capacity is accepted without truncation");
    strcat(value, ",none");
    check(!supported("background-image", value), "over-capacity image list refuses the whole value");
}

static void oracle(struct node *root)
{
    FILE *file = fopen("tests/fixtures/engine-expansion/css-layers-pixels.csv", "r");
    if (!file) { perror("css-layers-pixels.csv"); failures++; return; }
    char line[512], label[256], id[64];
    int rows = 0;
    while (fgets(line, sizeof line, file)) {
        int x, y, w, h; unsigned expected;
        if (sscanf(line, "pixel,%d,%d,%x,%255[^,\n]", &x, &y, &expected, label) == 4) {
            if (x < 0 || x >= W || y < 0 || y >= H) { failures++; break; }
            pixel(x, y, expected, label);
        } else if (sscanf(line, "box,%63[^,],%d,%d,%d,%d,%255[^\n]", id, &x, &y, &w, &h, label) == 6) {
            int ax, ay, aw, ah; struct node *target = find(root, id);
            check(target && layout_node_box(target, &ax, &ay, &aw, &ah) &&
                  ax == x && ay == y && aw == w && ah == h, label);
        } else { fprintf(stderr, "malformed background layers oracle: %s", line); failures++; break; }
        rows++;
    }
    fclose(file);
    check(rows == 53, "all shared guest pixel and geometry assertions were consumed");
}

static void used_lists(struct node *root)
{
    struct bg_layers layers = {0};
    const struct cstyle *style = find(root, "none-cycle")->style;
    check(css_background_layers(style, &layers) && layers.count == 3,
          "none contributes a real image layer");
    check(layers.layer[0].geometry.size[0].px == 20 &&
          layers.layer[1].geometry.size[0].px == 40 &&
          layers.layer[2].geometry.size[0].px == 20,
          "size list cycles from its first item rather than extending its last item");
    style = find(root, "truncate")->style;
    check(css_background_layers(style, &layers) && layers.count == 1 &&
          layers.layer[0].geometry.size[0].px == 10,
          "extra longhand values do not add background image layers");
    style = find(root, "shorthand")->style;
    check(css_background_layers(style, &layers) && layers.count == 1 &&
          layers.layer[0].origin == BG_BORDER_BOX && layers.layer[0].clip == BG_CONTENT_BOX,
          "two visual boxes retain their different shorthand roles");
}

static void late_and_failed_urls(void)
{
    const char *html = "<div id=second></div><div id=failed></div>";
    struct node *root = dom_parse(html, (int)strlen(html));
    const char *css = "body{margin:0}div{position:absolute;top:10px;width:100px;height:60px;background-color:white;background-repeat:no-repeat}"
        "#second{left:10px;background-image:linear-gradient(transparent,transparent),url(assets/stripes.bmp?layer=second);background-size:100% 100%}"
        "#failed{left:120px;background-image:url(assets/missing-layer.bmp),none,linear-gradient(blue,blue);background-size:20px 10px,40px 10px;background-position:0 0,30px 0}";
    render_page(root, css);
    check(background_images_wants("assets/stripes.bmp?layer=second"),
          "second image layer retains its actual URL request");
    check(background_images_wants("assets/missing-layer.bmp"),
          "failed image remains a layer until its load attempt resolves");
    background_images_load(8, fetch_bitmap);
    check(second_fetches == 1 && missing_fetches == 1 && layout_img_cached("assets/missing-layer.bmp"),
          "later URL and failed URL enter the shared positive and negative cache");
    render_page(root, css);
    pixel(12,30,0xe72939,"second bitmap layer paints decoded source pixels");
    pixel(122,12,0x0000ff,"failed first image does not renumber the third layer");
    pixel(152,12,0xffffff,"failed-image slot keeps the third layer position cycling");
    background_images_load(8, fetch_bitmap);
    check(second_fetches == 1 && missing_fetches == 1,
          "negative background cache prevents repeated failed-image IO");
    layout_free(); dom_free(root);
}

static void edge_geometry(void)
{
    const char *html = "<div id=edge></div><div id=single-box></div><div id=one-space></div><div id=auto-round></div><div id=rounded></div>";
    struct node *root = dom_parse(html, (int)strlen(html));
    const char *css = "body{margin:0}div{position:absolute;top:10px;width:100px;height:60px;box-sizing:border-box;background-color:white;background-repeat:no-repeat}"
        "#edge{left:10px;background-image:linear-gradient(blue,blue);background-size:20px 10px;background-position:right 3px bottom 4px}"
        "#single-box{left:120px;padding:10px;border:4px solid transparent;background:linear-gradient(blue,blue) 0 0 / 100% 100% no-repeat content-box}"
        "#one-space{left:230px;width:40px;background-image:linear-gradient(blue,blue);background-size:30px 10px;background-position:center top;background-repeat:space no-repeat}"
        "#auto-round{left:10px;top:80px;background-image:url(assets/stripes.bmp?layer=front);background-size:30px auto;background-repeat:round no-repeat}"
        "#rounded{left:120px;top:80px;border:4px solid transparent;padding:6px;border-radius:24px;background-color:blue;background-clip:content-box}";
    render_page(root, css);
    pixel(88,57,0x0000ff,"four-value edge position offsets inward after subtracting image size");
    pixel(85,57,0xffffff,"right-edge position is not a left-edge positive offset");
    pixel(122,40,0xffffff,"single shorthand visual box also clips the base color");
    pixel(135,25,0x0000ff,"single shorthand visual box defines the positioning area");
    pixel(232,12,0xffffff,"space with only one possible tile honors authored position");
    pixel(236,12,0x0000ff,"one space tile remains centered");
    pixel(12,96,0xe72939,"round restores the auto axis using the natural aspect ratio");
    pixel(12,98,0xffffff,"round auto axis does not stretch to the full painting area");
    pixel(131,91,0xffffff,"content clip follows its inset rounded corner");
    pixel(160,91,0x0000ff,"rounded content clip retains its straight inner top edge");
    layout_free(); dom_free(root);
}

static void refused_native_color(void)
{
    const char *html="<div id=refused></div>";
    struct node *root=dom_parse(html,(int)strlen(html));
    render_page(root,"body{margin:0}#refused{position:absolute;left:10px;top:10px;width:50px;height:30px;background:red url(fixed.bmp) fixed}");
    /* No earlier author color hides a native partial acceptance. A supports
     * query alone missed the native parser still publishing red from this
     * otherwise refused shorthand; the control lets that leak reappear. */
    pixel(20,20,0xffffff,"refused attachment cannot publish native shorthand color");
    check(!background_images_wants("fixed.bmp"),"refused attachment cannot publish resource desires");
    check(!supported("background","url(fixed.bmp) inherit"),"CSS-wide keyword cannot become a shorthand color component");
    layout_free();dom_free(root);
}

static int dirty_ready, dirty_polls, dirty_completions;
static int fetch_dirty_bitmap(const char *url, unsigned char **data, int *len)
{
    *data = NULL; *len = 0;
    if (strcmp(url,"assets/stripes.bmp?dirty=first") &&
        strcmp(url,"assets/stripes.bmp?dirty=third")) return -1;
    if (!dirty_ready) { dirty_polls++; return 1; }
    dirty_completions++;
    *data = (unsigned char *)read_file("tests/fixtures/engine-expansion/assets/stripes.bmp",len);
    return *data ? 0 : -1;
}

static void repaint_frame(void)
{
    /* This is only paint. Reapplying CSS or layout here would change the raw
     * spans/display list and hide the missing cache-identity signature. */
    gui_clip(0,0,W,H);
    browser_paint(0,0,W,H,0);
}

static int covers_box(int dirty, int x, int y, int w, int h, const struct node *node)
{
    int bx,by,bw,bh;
    return dirty == 1 && layout_node_box(node,&bx,&by,&bw,&bh) &&
        x <= bx && y <= by && x+w >= bx+bw && y+h >= by+bh;
}

static void background_dirty_frames(void)
{
    /* The guest downloaded and decoded 9654 bytes into an 80x40 background,
     * and painted its surface, yet submitted no dirty canvas region. Pixel
     * tests alone missed this: gui_flush_rect consumes a separate public result.
     * Complete the same pending URLs on a stable DOM, then demand BOTH outputs.
     * First/third layers and a shared URL on a distant box guard against a
     * signature that sees only one resource or only its first consumer. */
    layout_images_reset(); dirty_ready=dirty_polls=dirty_completions=0;
    const char *html="<div id=dirty-first></div><div id=dirty-third></div><div id=dirty-reuse></div>";
    struct node *root=dom_parse(html,(int)strlen(html));
    const char *css="html,body{margin:0;background:white}div{position:absolute;background-color:white;background-size:100% 100%;background-repeat:no-repeat}"
        "#dirty-first{left:10px;top:10px;width:80px;height:40px;background-image:url(assets/stripes.bmp?dirty=first)}"
        "#dirty-third{left:180px;top:100px;width:120px;height:60px;background-image:none,linear-gradient(transparent,transparent),url(assets/stripes.bmp?dirty=third)}"
        "#dirty-reuse{left:310px;top:200px;width:80px;height:40px;background-image:url(assets/stripes.bmp?dirty=first)}";
    render_page(root,css);
    background_images_load(8,fetch_dirty_bitmap);
    check(dirty_polls>0 && background_images_pending() &&
          !layout_img_get("assets/stripes.bmp?dirty=first") && !layout_img_get("assets/stripes.bmp?dirty=third"),
          "dirty frame keeps real background requests pending without cached pixels");
    pixel(12,12,0xffffff,"pending first background retains its base color");
    pixel(182,102,0xffffff,"pending third background retains its base color");
    pixel(312,202,0xffffff,"pending shared background retains its base color");
    unsigned before[3]={pixels[12][12],pixels[102][182],pixels[202][312]};
    int x=0,y=0,w=0,h=0;
    repaint_frame();
    check(browser_paint_dirty_rect(&x,&y,&w,&h)==0,
          "unchanged pending background frame has no dirty rectangle");
    dirty_ready=1; background_images_load(8,fetch_dirty_bitmap);
    check(dirty_completions==2 && !background_images_pending(),
          "existing pending URLs complete into the shared image cache once each");
    repaint_frame();
    int dirty=browser_paint_dirty_rect(&x,&y,&w,&h);
    pixel(12,12,0xe72939,"completed first background changes actual surface pixels");
    pixel(182,102,0xe72939,"completed third background changes actual surface pixels");
    pixel(312,202,0xe72939,"completed shared background changes every consumer surface");
    check(before[0]!=pixels[12][12] && before[1]!=pixels[102][182] && before[2]!=pixels[202][312],
          "late image completion changes pixels without a DOM CSS or layout mutation");
    if(dirty!=1)printf("background dirty result=%d after cache completion\n",dirty);
    check(covers_box(dirty,x,y,w,h,find(root,"dirty-first")),
          "late first background completion dirties its entire affected box");
    check(covers_box(dirty,x,y,w,h,find(root,"dirty-third")),
          "late third background completion dirties its entire affected box");
    check(covers_box(dirty,x,y,w,h,find(root,"dirty-reuse")),
          "late shared background completion dirties every affected box");
    unsigned ready[3]={pixels[12][12],pixels[102][182],pixels[202][312]};
    repaint_frame();
    check(ready[0]==pixels[12][12] && ready[1]==pixels[102][182] && ready[2]==pixels[202][312],
          "stable decoded background frame preserves surface pixels");
    check(browser_paint_dirty_rect(&x,&y,&w,&h)==0,
          "stable decoded background frame does not invent a dirty rectangle");
    layout_free();dom_free(root);
}

int main(int argc, char **argv)
{
    int html_length, css_length;
    char *html = read_file("tests/fixtures/engine-expansion/css-layers.html", &html_length);
    char *css = read_file("tests/fixtures/engine-expansion/css-layers.css", &css_length);
    if (!html || !css) { free(html); free(css); return 2; }
    css_init(); css_viewport(W,H); grammar();
    struct node *root = dom_parse(html, html_length);
    if (!root) { free(html); free(css); return 2; }
    render_page(root, css); used_lists(root);
    check(background_images_wants("assets/stripes.bmp?layer=back"),
          "third image layer keeps asynchronous discovery alive");
    check(!background_images_wants("assets/stripes.bmp?invalid=tail"),
          "invalid shorthand never schedules its otherwise valid URL prefix");
    check(background_images_pending(), "background layers keep the actual loader awake");
    defer_front = 1;
    check(background_images_load(1,fetch_bitmap) == 0 &&
          !layout_img_cached("assets/stripes.bmp?layer=front"),
          "pending background does not become a failed cache entry");
    check(background_images_pending(), "pending multi-layer URL survives until completion");
    background_images_load(8,fetch_bitmap);
    check(front_fetches == 2 && back_fetches == 1 && bad_fetches == 0,
          "one pending poll and one completion load each distinct visible URL once");
    check(!background_images_pending(), "completed layer requests release the loader drain");
    background_images_load(8,fetch_bitmap);
    check(front_fetches == 2 && back_fetches == 1,
          "duplicate URLs across layers and elements share one decoded cache");
    render_page(root, css); oracle(root);
    char computed_color[64];
    check(css_computed_text(find(root,"shorthand"),CSSP_BACKGROUND_COLOR,computed_color,sizeof computed_color)>0&&
          !strcmp(computed_color,"rgb(255, 0, 0)"),
          "computed background color agrees with the rendered complex shorthand");
    if (argc > 1) {
        FILE *ppm = fopen(argv[1], "wb");
        if (!ppm) { perror(argv[1]); return 2; }
        fprintf(ppm,"P6\n%d %d\n255\n",W,H);
        for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
            unsigned p=pixels[y][x]; unsigned char rgb[3]={p>>16,p>>8,p}; fwrite(rgb,1,3,ppm);
        }
        fclose(ppm);
    }
    layout_free(); dom_free(root); free(html); free(css);
    late_and_failed_urls(); edge_geometry(); refused_native_color(); background_dirty_frames(); layout_images_reset();
    printf("css-background-layers: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
