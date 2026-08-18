/* tests/unit/layout_box_test.c -- the box table and the two placement fixes.
 *
 * WHAT THIS MEASURES. c/apps/browser/layout.c now keeps one record per element
 * that GENERATED a box, whether or not it painted, and answers two queries off
 * it: layout_node_box() (the border box in document coordinates) and
 * layout_node_scroll() (the scrollable overflow area). The consumer is
 * js_cssom.c's geometry, which until now read boxes out of the display list --
 * a list of INK -- and therefore answered 0 for every element with no
 * background, no border and no text of its own.
 *
 * So the assertions below are deliberately weighted toward elements that PAINT
 * NOTHING. A suite that only checked boxes with a background would pass
 * against a table populated exactly where the old display list already had an
 * entry, which is the negative control this file is paired with
 * (LAYOUT_NEGCTL_BOX_INK_ONLY, see tests/layoutbox.mk).
 *
 * The two placement fixes are here too, with their own controls:
 *   LAYOUT_NEGCTL_BODY_NOPAD    <body>'s padding and border take no space
 *   LAYOUT_NEGCTL_ABS_PARENT    position:absolute anchors at its PARENT
 *   LAYOUT_NEGCTL_SCROLL_IS_CLIENT   scrollWidth is always clientWidth
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "layout.h"
#include "css.h"
#include "dom.h"

void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }
/* A monospace approximation, exactly like every other host layout harness:
 * these assertions are about BOX GEOMETRY, never about glyph advances. */
int text_measure(const char *s, int len, int px, int mono){ (void)s;(void)mono; return len*(px/2); }
int res_fetch(const char *u, uint8_t **b, int *l){ (void)u;(void)b;(void)l; return -1; }
void img_free(struct image *o){ (void)o; }
int img_decode(const uint8_t *p, int n, struct image *o){ (void)p;(void)n;(void)o; return -1; }

static int checks, fails;
#define CHECK(c,m) do{ checks++; if(!(c)){ printf("  FAIL: %s\n",(m)); fails++; } }while(0)
#define EQ(got,want,m) do{ checks++; if((got)!=(want)){ \
        printf("  FAIL: %s -- got %d, want %d\n",(m),(int)(got),(int)(want)); fails++; } }while(0)

static int tag_is(const char *t, const char *lit){ int i=0; for(;lit[i];i++) if(t[i]!=lit[i]) return 0; return t[i]==0; }
static int collect_style(struct node *n, char *out, int o, int max){
    if(!n) return o;
    if(n->type==N_ELEM && tag_is(n->tag,"style"))
        for(struct node *c=n->first_child;c;c=c->next)
            if(c->type==N_TEXT && c->text)
                for(int i=0;i<c->textlen && o<max-1;i++) out[o++]=c->text[i];
    for(struct node *c=n->first_child;c;c=c->next) o=collect_style(c,out,o,max);
    return o;
}

static struct node *g_root;
static struct node *find_id(struct node *n, const char *id){
    if(n->type==N_ELEM){ const char *a=dom_attr(n,"id"); if(a && !strcmp(a,id)) return n; }
    for(struct node *c=n->first_child;c;c=c->next){ struct node *r=find_id(c,id); if(r) return r; }
    return 0;
}
static struct node *ID(const char *id){
    struct node *e = find_id(g_root, id);
    if(!e){ printf("  FAIL: no element #%s in the document\n", id); fails++; checks++; }
    return e;
}

/* Lay the page out at `cw` and leave it current for the queries below. */
static void page(const char *html, int cw){
    static char css[16384], exp[32768];
    g_root = dom_parse(html, (int)strlen(html));
    int cl = collect_style(g_root, css, 0, (int)sizeof css);
    /* THE BROWSER'S ORDER, not a shorter one. browser.c runs css_expand_vars
     * over the collected sheet and hands the EXPANDED text to css_apply; this
     * helper used to skip that step, so every `var()` in a test reached LibCSS
     * unsubstituted and resolved to nothing.
     *
     * That is not a harmless simplification -- it is a harness that disagrees
     * with the thing it is testing, and it produced a false finding the day it
     * was noticed: `padding-top: var(--cover-radio)` came out 0 here and was
     * about to be reported as a browser bug. bilibili writes exactly that
     * declaration. */
    int el = css_expand_vars(css, cl, exp, (int)sizeof exp);
    css_apply(g_root, exp, el);
    layout_page(g_root, cw);
}

/* The four numbers, in one assertion, because a box that is the right size in
 * the wrong place is not a partial pass. */
static void box_is(const char *id, int x, int y, int w, int h){
    struct node *e = ID(id); if(!e) return;
    int gx=0,gy=0,gw=0,gh=0;
    int ok = layout_node_box(e,&gx,&gy,&gw,&gh);
    checks++;
    if(!ok){ printf("  FAIL: #%s generated no box at all (want %d,%d %dx%d)\n", id,x,y,w,h); fails++; return; }
    if(gx!=x||gy!=y||gw!=w||gh!=h){
        printf("  FAIL: #%s box = (%d,%d %dx%d), want (%d,%d %dx%d)\n", id,gx,gy,gw,gh,x,y,w,h);
        fails++;
    }
}
static void nobox(const char *id){
    struct node *e = ID(id); if(!e) return;
    int gx=0,gy=0,gw=0,gh=0;
    checks++;
    if(layout_node_box(e,&gx,&gy,&gw,&gh)){
        printf("  FAIL: #%s generated a box (%d,%d %dx%d) and must not\n", id,gx,gy,gw,gh);
        fails++;
    }
}
static void scroll_is(const char *id, int w, int h){
    struct node *e = ID(id); if(!e) return;
    int gw=0,gh=0;
    int ok = layout_node_scroll(e,&gw,&gh);
    checks++;
    if(!ok){ printf("  FAIL: #%s has no scrollable area (want %dx%d)\n", id,w,h); fails++; return; }
    if(gw!=w||gh!=h){ printf("  FAIL: #%s scroll = %dx%d, want %dx%d\n", id,gw,gh,w,h); fails++; }
}

/* How many of the document's elements have a box the CSSOM could read out of
 * the DISPLAY LIST alone -- the "EXACT" class -- against how many have one in
 * the table. The gap between the two IS the NOBOX class this line exists to
 * close, so it is asserted rather than printed. */
static void class_counts(struct node *n, int *elems, int *inked, int *tabled){
    if(n->type==N_ELEM){
        (*elems)++;
        const struct item *it = layout_items();
        for(int i=0;i<layout_count();i++)
            if(it[i].node==n && it[i].type==IT_RECT){ (*inked)++; break; }
        int x,y,w,h;
        if(layout_node_box(n,&x,&y,&w,&h)) (*tabled)++;
    }
    for(struct node *c=n->first_child;c;c=c->next) class_counts(c,elems,inked,tabled);
}

/* ---- percentage padding, which is how the whole web reserves a picture ----
 *
 * CSS 2.1 8.4: a percentage on ANY padding edge -- top and bottom included --
 * resolves against the containing block's WIDTH. That asymmetry is the entire
 * mechanism behind the aspect-ratio box: a wrapper with `height:0` and
 * `padding-top:56.25%` is exactly 16:9 of its own width, and the cover image
 * inside it is `position:absolute; inset:0`. Every card grid on the modern web
 * is built this way, bilibili's included.
 *
 * MEASURED THERE FIRST, which is why this test exists. Its video cards paint
 * their titles at coordinates INSIDE the thumbnail -- the browser draws the
 * text and then blits the cover over it -- and the grey area below the image,
 * where the title belongs, is empty. The cause is one line in css_engine.c:
 * padding was converted with len_px(..., NULL), discarding the percentage flag
 * that function computes, so padding-top:56.25% became FIFTY-SIX PIXELS. The
 * wrapper is then far shorter than the cover it exists to reserve space for,
 * and an absolutely positioned height:100% cover swallows the card.
 *
 * A percentage read as a pixel count is the worst shape a units bug can take:
 * it produces a plausible small number rather than a zero or a crash, so every
 * box still has a size and the page still paints. */
static void t_pct_padding(void)
{
    printf("-- percentage padding resolves against the containing block WIDTH\n");
    /* 400 wide, so 56.25% is 225 and 25% is 100 -- numbers no pixel reading of
     * the same declarations can coincide with. */
    page("<html><head><style>body{margin:0}"
         "#cb{width:400px}"
         "#ar{height:0;padding-top:56.25%}"
         "#pad{padding:25%}"
         "#in{height:10px}"
         "</style></head><body>"
         "<div id=cb><div id=ar></div><div id=pad><div id=in></div></div></div>"
         "</body></html>", 800);
    box_is("ar", 0, 0, 400, 225);
    /* All four edges against WIDTH, bottom and top included: 400 * 0.25 = 100. */
    box_is("pad", 0, 225, 400, 210);
    box_is("in", 100, 325, 200, 10);

    /* ...AND THROUGH A CUSTOM PROPERTY, which is how the page that prompted
     * this actually writes it:
     *
     *     .bili-video-card__image--wrap { padding-top: var(--cover-radio) }
     *     --cover-radio: 56.25%
     *
     * A percentage that arrives by substitution has to reach the same place a
     * literal one does. Splitting the two is the difference between a fix that
     * works on a test and a fix that works on the web -- the literal form
     * passed here while the page it was written for did not move at all. */
    page("<html><head><style>body{margin:0}"
         "#cb2{width:400px;--cover-radio:56.25%}"
         "#var{height:0;padding-top:var(--cover-radio)}"
         "</style></head><body>"
         "<div id=cb2><div id=var></div></div>"
         "</body></html>", 800);
    box_is("var", 0, 0, 400, 225);
}

/* ---- calc() on a HEIGHT ---------------------------------------------------
 *
 * Upstream LibCSS gives calc() to `width` and to NO OTHER length property.
 * Every other one -- height, min/max on both axes, all four margins and
 * paddings, top/right/bottom/left -- dropped a calc() at CASCADE time, so the
 * declaration never reached the computed style and the property read as
 * absent. `height` is fixed (select_config.py, marked as a local patch); the
 * rest are still in that state and are named in the commit.
 *
 * It is invisible until it is not, and then it is invisible AGAIN: an absent
 * height leaves the box empty, and layout floors an empty box at font_px --
 * so the result is a small plausible number, not a zero. bilibili's video-card
 * titles are `height: calc(2 * var(--title-line-height))` with
 * overflow:hidden, which came out as one clipped line instead of two, with no
 * error anywhere.
 *
 * All three forms, because they are three different paths through the
 * calculator: a number times a dimension, the same reversed, and a sum. The
 * var() form is the one the page writes and the one that also exercises the
 * substitution in page(). */
static void t_calc_height(void)
{
    printf("-- calc() resolves on a height, not only on a width\n");
    page("<html><head><style>body{margin:0}"
         "#r{--lh:22px}"
         "#lit{height:calc(2 * 22px)}"
         "#rev{height:calc(22px * 2)}"
         "#sum{height:calc(20px + 24px)}"
         "#varm{height:calc(2 * var(--lh))}"
         "#w{width:calc(100px + 40px);height:30px}"
         "</style></head><body><div id=r>"
         "<div id=lit>a</div><div id=rev>a</div><div id=sum>a</div>"
         "<div id=varm>a</div><div id=w>a</div>"
         "</div></body></html>", 400);
    box_is("lit",  0,   0, 400, 44);
    box_is("rev",  0,  44, 400, 44);
    box_is("sum",  0,  88, 400, 44);
    box_is("varm", 0, 132, 400, 44);
    /* width's calc has always worked; asserted beside the others so a future
     * change that breaks one and not the other is not read as both. The
     * height is 30 and not 10 deliberately: layout floors an empty box at
     * font_px, so a 10 here would be asserting that floor rather than this
     * calc, and the failure would name the wrong thing. */
    box_is("w",    0, 176, 140, 30);
}

/* ---- <canvas> reserves a box ----------------------------------------------
 *
 * It used to fall through to the empty-block path and take NO SPACE, so a page
 * that drew a chart reserved nothing for it and the surrounding text closed
 * over the hole. The default is CSS's 300x150 -- the same one <video> uses,
 * and where <video>'s came from.
 *
 * The two sizes are DIFFERENT QUANTITIES and the order they resolve in has to
 * match on both sides: the width/height ATTRIBUTES size the backing bitmap
 * (js_canvas.c) and CSS sizes the box it is drawn into (here). A canvas laid
 * out at one size and rendered at another is the bug this pins. */
static void t_canvas_box(void)
{
    printf("-- <canvas> is a replaced element with CSS's 300x150 default\n");
    page("<html><head><style>body{margin:0}"
         "#css{width:80px;height:20px}"
         "</style></head><body>"
         "<canvas id=att width=200 height=100></canvas>"
         "<canvas id=def></canvas>"
         "<canvas id=css width=200 height=100></canvas>"
         "</body></html>", 800);
    box_is("att", 0, 0, 200, 100);
    /* The default follows the first on the same line, so its x is 200. */
    box_is("def", 200, 0, 300, 150);
    /* CSS beats the attributes for the BOX -- the attributes still own the
     * bitmap, which is why 80x20 here and 200x100 there is correct and not a
     * contradiction. */
    box_is("css", 500, 0, 80, 20);
}

int main(void)
{
    /* ---------------------------------------------------------------- 1
     * The whole point: a bare sized <div> that paints nothing. Before the
     * table these four numbers were 0,0,0,0 and getComputedStyle on the same
     * element correctly said width:120px. */
    t_pct_padding();
    t_calc_height();
    t_canvas_box();

    printf("-- a box nobody painted\n");
    page("<html><head><style>body{margin:0}"
         "#a{width:120px;height:40px}"
         "#b{width:60px;height:30px;background:red}"
         "#c{width:80px;height:25px}"
         "</style></head><body>"
         "<div id=a></div><div id=b></div><div id=c></div>"
         "</body></html>", 800);
    box_is("a", 0,  0, 120, 40);
    box_is("b", 0, 40,  60, 30);
    box_is("c", 0, 70,  80, 25);
    {
        struct node *e = find_id(g_root, "a");
        int x,y,w,h;
        CHECK(e && layout_node_box(e,&x,&y,&w,&h), "the unpainted div has a box");
    }

    printf("-- display:none generates nothing\n");
    page("<html><head><style>body{margin:0}"
         "#v{width:50px;height:10px}#h{display:none;width:50px;height:10px}"
         "</style></head><body><div id=v></div><div id=h></div></body></html>", 800);
    box_is("v", 0, 0, 50, 10);
    nobox("h");

    /* ---------------------------------------------------------------- 2
     * The border box is the BORDER box: content + padding + border, and its
     * origin is the border edge, not the content edge. */
    printf("-- the box is the border box\n");
    page("<html><head><style>body{margin:0}"
         "#p{width:200px;padding:10px;border:5px solid black}"
         "#in{height:20px}"
         "#bb{box-sizing:border-box;width:200px;padding:10px;border:5px solid black}"
         "</style></head><body>"
         "<div id=p><div id=in></div></div><div id=bb></div>"
         "</body></html>", 800);
    box_is("p",  0, 0, 230, 50);      /* 200 + 2*10 + 2*5 */
    box_is("in", 15, 15, 200, 20);    /* at the content edge */
    box_is("bb", 0, 50, 200, 30);     /* border-box sizing: 200 total */

    /* ---------------------------------------------------------------- 3
     * Scrollable overflow. This is the number an ink union cannot approximate
     * at all, because the overflowing child paints nothing either. */
    printf("-- scrollable overflow\n");
    page("<html><head><style>body{margin:0}"
         "#s{width:100px;height:50px;overflow:hidden}"
         "#big{width:400px;height:300px}"
         "#tight{width:100px;height:50px;overflow:hidden}"
         "#small{width:20px;height:10px}"
         "#padded{width:100px;padding:10px;border:2px solid black}"
         "#kid{width:30px;height:15px}"
         "</style></head><body>"
         "<div id=s><div id=big></div></div>"
         "<div id=tight><div id=small></div></div>"
         "<div id=padded><div id=kid></div></div>"
         "</body></html>", 800);
    scroll_is("s", 400, 300);         /* the child's box, not its ink */
    /* A box whose content fits reports its own padding box, never less. */
    scroll_is("tight", 100, 50);
    /* Padding box, not border box: 100 content + 20 padding = 120 wide. */
    scroll_is("padded", 120, 35);

    /* ---------------------------------------------------------------- 4
     * <body>'s padding. Reported as "body{margin:0;padding:4000px} puts the
     * child at (0,0), and the same padding on a plain <div> works". It did:
     * layout_page placed body's CONTENT at body's MARGIN edge. */
    printf("-- <body> is a box like any other\n");
    page("<html><head><style>body{margin:0;padding:40px}"
         "#c{width:10px;height:10px}"
         "</style></head><body><div id=c></div></body></html>", 800);
    box_is("c", 40, 40, 10, 10);
    {   /* the same padding one level down, which always worked -- the two
         * must now agree, which is the actual claim */
        page("<html><head><style>body{margin:0}#o{padding:40px}"
             "#c{width:10px;height:10px}"
             "</style></head><body><div id=o><div id=c></div></div></body></html>", 800);
        box_is("c", 40, 40, 10, 10);
    }
    printf("-- <body>'s border and its own box\n");
    page("<html><head><style>body{margin:7px;padding:11px;border:3px solid black}"
         "#c{width:10px;height:10px}"
         "</style></head><body><div id=c></div></body></html>", 500);
    box_is("c", 7+3+11, 7+3+11, 10, 10);
    {   /* <body> itself: its border box starts at its margin edge and is the
         * canvas minus both margins. documentElement and body geometry is read
         * on nearly every checkLayout test in the corpus. */
        struct node *body = g_root->doc ? dom_doc_body(g_root->doc) : 0;
        int x=-1,y=-1,w=-1,h=-1;
        CHECK(body && layout_node_box(body,&x,&y,&w,&h), "<body> has a box");
        EQ(x, 7, "body border box x = its left margin");
        EQ(y, 7, "body border box y = its top margin");
        EQ(w, 500-14, "body border box width = canvas - both margins");
    }

    /* ---------------------------------------------------------------- 5
     * position:absolute. Reported as "left:30px;top:70px lands at (-10,30)",
     * which was body{padding:40px} plus an anchor reconstructed from the
     * PARENT rather than from the nearest positioned ancestor. */
    printf("-- position:absolute is placed against its containing block\n");
    page("<html><head><style>body{margin:0;padding:40px}"
         "#c{position:absolute;left:30px;top:70px;width:10px;height:10px}"
         "</style></head><body><div id=c></div></body></html>", 800);
    box_is("c", 30, 70, 10, 10);       /* the ICB: body is static */

    page("<html><head><style>body{margin:0}"
         "#o{margin:50px;padding:20px;height:200px}"
         "#c{position:absolute;left:30px;top:70px;width:10px;height:10px}"
         "</style></head><body><div id=o><div id=c></div></div></body></html>", 800);
    box_is("c", 30, 70, 10, 10);       /* a STATIC ancestor is not a containing block */

    page("<html><head><style>body{margin:0}"
         "#o{position:relative;left:5px;top:5px;width:300px;height:300px;"
         "   padding:10px;border:4px solid black}"
         "#c{position:absolute;left:30px;top:70px;width:10px;height:10px}"
         "</style></head><body><div id=o><div id=c></div></div></body></html>", 800);
    /* #o's border box lands at (5,5) after the relative offset; its PADDING
     * box, which is the containing block, starts 4px inside that. */
    box_is("c", 5+4+30, 5+4+70, 10, 10);

    printf("-- the nearest positioned ancestor, not the nearest ancestor\n");
    page("<html><head><style>body{margin:0}"
         "#rel{position:relative;width:400px;padding:6px}"
         "#mid{margin:25px;padding:15px}"
         "#c{position:absolute;left:10px;top:20px;width:10px;height:10px}"
         "</style></head><body>"
         "<div id=rel><div id=mid><div id=c></div></div></div>"
         "</body></html>", 800);
    /* #rel's PADDING box starts at its border edge, and it has no border, so
     * the containing block origin is (0,0) -- its own 6px padding is INSIDE
     * the containing block, not outside it. #mid's 25+15 must not appear. */
    box_is("c", 10, 20, 10, 10);

    printf("-- right/bottom anchor the far edge\n");
    page("<html><head><style>body{margin:0}"
         "#rel{position:relative;width:400px;height:300px}"
         "#c{position:absolute;right:30px;top:0;width:10px;height:10px}"
         "#d{position:absolute;left:0;bottom:20px;width:10px;height:10px}"
         "</style></head><body>"
         "<div id=rel><div id=c></div><div id=d></div></div>"
         "</body></html>", 800);
    box_is("c", 400-30-10, 0, 10, 10);
    box_is("d", 0, 300-20-10, 10, 10);

    /* An absolute box is itself a containing block for its own absolute kids. */
    printf("-- an absolute box is a containing block\n");
    page("<html><head><style>body{margin:0}"
         "#a{position:absolute;left:100px;top:50px;width:200px;height:100px}"
         "#b{position:absolute;left:10px;top:10px;width:5px;height:5px}"
         "</style></head><body><div id=a><div id=b></div></div></body></html>", 800);
    box_is("a", 100, 50, 200, 100);
    box_is("b", 110, 60, 5, 5);

    /* ---------------------------------------------------------------- 6
     * Replaced elements had exact boxes in the display list all along -- the
     * CSSOM could not read them because it only recognises IT_RECT. */
    printf("-- replaced elements and form controls\n");
    page("<html><head><style>body{margin:0}</style></head><body>"
         "<img id=i src='x.png' width='40' height='30'>"
         "</body></html>", 800);
    { struct node *e = ID("i"); int x,y,w,h;
      if (e) { CHECK(layout_node_box(e,&x,&y,&w,&h), "an <img> has a box");
               EQ(w, 40, "img box width"); EQ(h, 30, "img box height"); } }

    /* ---------------------------------------------------------------- 7
     * Flex and grid items, which are moved after they are laid out. A record
     * that did not follow its display-list range reports the pre-alignment
     * position, which is the failure mode shift_boxes() exists to prevent. */
    printf("-- a flex item moved by justify-content\n");
    page("<html><head><style>body{margin:0}"
         "#f{display:flex;justify-content:flex-end;width:400px}"
         "#i1{width:50px;height:20px}"
         "</style></head><body><div id=f><div id=i1></div></div></body></html>", 800);
    { struct node *e = ID("i1"); int x=0,y=0,w=0,h=0;
      if (e) { CHECK(layout_node_box(e,&x,&y,&w,&h), "a flex item has a box");
               EQ(w, 50, "flex item width");
               CHECK(x > 300, "the record followed justify-content to the far end"); } }

    printf("-- a table cell\n");
    page("<html><head><style>body{margin:0}table{width:400px}</style></head><body>"
         "<table><tr><td id=t1>a</td><td id=t2>b</td></tr></table></body></html>", 800);
    { struct node *e = ID("t1"); int x,y,w,h;
      if (e) CHECK(layout_node_box(e,&x,&y,&w,&h), "a <td> has a box"); }

    /* ---------------------------------------------------------------- 8
     * An undecorated inline. <span> with no background is the single most
     * common element on the web that used to answer 0. */
    printf("-- an undecorated inline\n");
    page("<html><head><style>body{margin:0}p{margin:0;font-size:16px}</style></head>"
         "<body><p>hello <span id=s>world</span> tail</p></body></html>", 800);
    { struct node *e = ID("s"); int x=0,y=0,w=0,h=0;
      if (e) { CHECK(layout_node_box(e,&x,&y,&w,&h), "a bare <span> has a box");
               CHECK(w > 0, "the span's box has a width");
               CHECK(x > 0, "the span starts after the text before it"); } }

    /* ---------------------------------------------------------------- 9
     * THE CLASS COUNT. This is the assertion the negative control is aimed at:
     * a table populated only where an IT_RECT already existed makes `tabled`
     * equal `inked`, every other assertion in this file that happens to use a
     * background keeps passing, and the line is silently not done. */
    printf("-- the table is not the display list\n");
    page("<html><head><style>body{margin:0}"
         ".plain{width:50px;height:10px}"
         ".ink{width:50px;height:10px;background:#ccc}"
         "</style></head><body>"
         "<div class=plain></div><div class=plain></div><div class=plain></div>"
         "<div class=plain></div><div class=plain></div><div class=plain></div>"
         "<div class=ink></div><div class=ink></div>"
         "</body></html>", 800);
    { int elems=0, inked=0, tabled=0;
      class_counts(g_root, &elems, &inked, &tabled);
      printf("   elements=%d  own IT_RECT=%d  box record=%d\n", elems, inked, tabled);
      CHECK(inked <= 3, "only the backgrounded divs reach the display list");
      CHECK(tabled >= inked + 6,
            "the table holds boxes the display list does not -- if this fails, "
            "the table is the display list with extra steps"); }

    printf("\nlayout_box_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
