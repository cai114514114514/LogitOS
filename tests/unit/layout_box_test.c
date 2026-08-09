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
    static char css[16384];
    g_root = dom_parse(html, (int)strlen(html));
    int cl = collect_style(g_root, css, 0, (int)sizeof css);
    css_apply(g_root, css, cl);
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

int main(void)
{
    /* ---------------------------------------------------------------- 1
     * The whole point: a bare sized <div> that paints nothing. Before the
     * table these four numbers were 0,0,0,0 and getComputedStyle on the same
     * element correctly said width:120px. */
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
    box_is("c", 6+10, 6+20, 10, 10);   /* #mid's 25+15 must not appear */

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
