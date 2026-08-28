/* tests/unit/ua_default_test.c -- the UA stylesheet against the HTML Standard's
 * rendering section: is the FLOOR right?
 *
 * WHY THIS MATTERS, stated in CLAUDE.md and worth repeating here: when author
 * CSS is dropped, missing, or has not arrived (which -- measured on the Bing
 * search results page, tests/fixtures/webapi/bing -- is common), what a person
 * sees IS the UA default stylesheet in c/apps/browser/css_engine.c's UA_CSS.
 * A thin or wrong UA sheet makes every such page worse than it has to be.
 *
 * WHAT THIS CHECKS, against https://html.spec.whatwg.org/multipage/rendering.html:
 *   - display defaults for elements UA_CSS did not cover: hr, address, aside,
 *     dl/dt/dd, fieldset/legend, details/summary, and the obsolete block trio
 *     listing/plaintext/xmp (which already had white-space:pre but not
 *     display:block).
 *   - hr additionally gets a real visible line (block box + a border), because
 *     display:block alone leaves a zero-size, invisible box -- and an <hr>
 *     that draws nothing is indistinguishable from one whose CSS never arrived,
 *     which is exactly the ambiguity this whole investigation exists to close.
 *   - the CASCADE ORIGIN control: an author rule at equal specificity still
 *     wins over the UA rule. A UA sheet "improved" by outranking author CSS
 *     would make real pages worse in a way that reads as a rendering bug
 *     forever after.
 *
 * THE TRAP THIS FILE IS ALSO A GUARD AGAINST: making the UA sheet richer makes
 * an unstyled page look better and can therefore MASK a dropped author
 * stylesheet. t_ua_does_not_shadow_drop_reporting() checks that filling these
 * gaps does not change css_sheet_parses() or otherwise fake an author parse
 * when there is no author CSS at all -- the UA sheet is parsed once at
 * css_init(), not per-page, and is a completely different code path from
 * author_sheet()'s cache/drop accounting.
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
int text_measure(const char *s, int len, int px, int mono){ (void)s;(void)mono; return len*(px/2); }
int res_fetch(const char *u, uint8_t **b, int *l){ (void)u;(void)b;(void)l; return -1; }
void img_free(struct image *o){ (void)o; }
int img_decode(const uint8_t *p, int n, struct image *o){ (void)p;(void)n;(void)o; return -1; }

static int checks, fails;
#define CHECK(c,m) do{ checks++; if(!(c)){ printf("  FAIL: %s\n",(m)); fails++; } }while(0)
#define EQ(got,want,m) do{ checks++; if((got)!=(want)){ \
        printf("  FAIL: %s -- got %d, want %d\n",(m),(int)(got),(int)(want)); fails++; } }while(0)

static int tag_is(const char *t, const char *lit){ int i=0; for(;lit[i];i++) if(t[i]!=lit[i]) return 0; return t[i]==0; }
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

/* Same order the browser uses: collect <style>, expand vars, css_apply, layout.
 * (No <style> to collect here on purpose -- most of this file's cases are the
 * NO-AUTHOR-CSS floor. t_author_wins_at_equal_specificity is the one exception
 * and builds its own sheet text directly.) */
static void page_css(const char *html, const char *css, int cw){
    static char exp[32768];
    g_root = dom_parse(html, (int)strlen(html));
    int cl = css ? (int)strlen(css) : 0;
    int el = css_expand_vars(css ? css : "", cl, exp, (int)sizeof exp);
    css_apply(g_root, exp, el);
    layout_page(g_root, cw);
}
static void page(const char *html, int cw){ page_css(html, "", cw); }

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

/* ---- display:block, one element at a time, each proven by GEOMETRY ----
 *
 * The proof shape: lay the element out ALONE in a 400px, zero-margin body and
 * measure its own box WIDTH. This is the one signal that cannot be faked by
 * how surrounding siblings happen to flow -- CSS's initial `display: inline`
 * shrink-wraps a box to its content ("hi" measures 16px in this harness's
 * monospace approximation, see text_measure() above), while `display: block`
 * fills the containing block (400px). An earlier version of this test tried
 * to infer block-ness from whether a trailing sibling got pushed onto its own
 * line, and that measurement was WRONG: a block sibling AFTER an inline run
 * always starts a new line regardless of what came before it, so the earlier
 * assertion passed identically whether the element under test was block or
 * still inline -- it was measuring nothing. Caught only by cross-checking
 * against known-good display:block / display:inline / display:inline-block
 * controls before trusting the harness (rule 1: suspect the apparatus first).
 *
 * `plaintext` is deliberately NOT in this loop. Per the HTML tokenizer's own
 * PLAINTEXT state (correctly implemented here -- see html_tokenizer.c), once
 * <plaintext> is seen every byte after it, INCLUDING what looks like a
 * closing tag, becomes that element's raw text content and there is no way
 * back to markup. So "does #after exist as a sibling" is not a question this
 * element can answer; it is covered by its own rule in UA_CSS (grouped with
 * listing/xmp, which already share list its white-space:pre rule) but not by
 * a geometry check here. */
static void t_block_defaults(void)
{
    printf("-- elements missing from UA_CSS's display:block group now have one\n");
    const char *tags[] = { "address", "aside", "dl", "fieldset", "details", "listing", "xmp" };
    for (unsigned i = 0; i < sizeof(tags)/sizeof(tags[0]); i++) {
        char html[256];
        snprintf(html, sizeof html, "<body style='margin:0'><%s id=mid>hi</%s></body>", tags[i], tags[i]);
        page(html, 400);
        struct node *mid = ID("mid");
        int mx=0,my=0,mw=0,mh=0;
        int ok = mid && layout_node_box(mid,&mx,&my,&mw,&mh);
        checks++;
        if (!ok) { printf("  FAIL: <%s> generated no box at all\n", tags[i]); fails++; continue; }
        checks++;
        /* Block: the full 400px containing block. Inline: shrink-to-fit
         * ("hi" -> 16px in this harness). 350 sits nowhere near either, so
         * it distinguishes them with room on both sides. */
        if (mw < 350) {
            printf("  FAIL: <%s> box width=%d (x=%d), want >=350 (still display:inline, shrink-to-fit)\n",
                   tags[i], mw, mx);
            fails++;
        }
    }
}

/* Calibration for the width-based test above: proves 400/16/400 really are
 * what block/inline/inline-block look like in THIS harness before trusting
 * any assertion built on them. If this fails, t_block_defaults's threshold is
 * wrong, not the elements it tests. */
static void t_calibrate_width_signal(void)
{
    printf("-- calibration: block vs inline really do differ by BOX WIDTH here\n");
    page("<body style='margin:0'><div id=mid style='display:block'>hi</div></body>", 400);
    struct node *mid = ID("mid"); int x=0,y=0,w=0,h=0;
    if (mid && layout_node_box(mid,&x,&y,&w,&h)) EQ(w, 400, "display:block width");
    page("<body style='margin:0'><div id=mid style='display:inline'>hi</div></body>", 400);
    mid = ID("mid");
    if (mid && layout_node_box(mid,&x,&y,&w,&h)) CHECK(w < 350, "display:inline width should shrink-to-fit");
}

/* dt/dd/li-style children need the SAME test shape but nested inside their
 * required parent, because <dt>/<dd> outside a <dl> and <li> outside a list
 * are exactly the malformed-but-real-web case the tree builder still has to
 * cope with -- testing them in place is testing what a real page does. */
static void t_dl_children(void)
{
    printf("-- dt/dd default to display:block inside dl\n");
    page("<body style='margin:0'><dl style='margin:0'>"
         "<dt id=t>Term</dt>"
         "<dd id=d>Definition</dd>"
         "</dl></body>", 400);
    struct node *t = ID("t");
    int tx=0,ty=0,tw=0,th=0;
    checks++;
    if (!(t && layout_node_box(t,&tx,&ty,&tw,&th))) { printf("  FAIL: no #t (dt) box\n"); fails++; return; }
    checks++;
    if (tw < 350) { printf("  FAIL: <dt> box width=%d, want >=350 (still display:inline)\n", tw); fails++; }
    /* dd indents from its dl's edge -- the one numeric default real pages
     * lean on (a FAQ/glossary with no CSS at all still reads as term/answer,
     * not two flush-left runs). Matches this file's own convention (28px,
     * the same indent ul/ol already use) rather than the spec's 40px, for
     * visual consistency with the list rules already in UA_CSS. */
    struct node *dd = ID("d");
    int dx=0,dy=0,dw=0,dh=0;
    checks++;
    if (!(dd && layout_node_box(dd,&dx,&dy,&dw,&dh))) { printf("  FAIL: no #d box\n"); fails++; return; }
    EQ(dx, 28, "dd left edge (indent from dl)");
}

/* ---- hr: display:block is not enough -- it must be VISIBLE ----
 *
 * Before this fix hr had NO rule in UA_CSS at all, so it kept CSS's initial
 * display:inline and, being non-replaced, took no box whatsoever (m_replaced
 * only affects margin collapsing, not whether a box is generated at all --
 * see layout.c's is_block()). A page whose only horizontal rule comes from a
 * dropped external stylesheet and one whose <hr> the engine never draws are
 * now the SAME pixels, which is exactly the ambiguity this investigation
 * exists to close. So: hr gets a real box (nonzero height from its border),
 * full width by default, sitting on its own line. */
static void t_hr_is_visible(void)
{
    printf("-- <hr> generates a real, visible box\n");
    page("<body style='margin:0'><hr id=r></body>", 400);
    struct node *r = ID("r");
    int rx=0,ry=0,rw=0,rh=0;
    int ok = r && layout_node_box(r,&rx,&ry,&rw,&rh);
    checks++;
    if (!ok) { printf("  FAIL: <hr> generated no box at all\n"); fails++; return; }
    checks++;
    if (rh < 1) { printf("  FAIL: <hr> box height=%d, want >=1 (must be visible)\n", rh); fails++; }
    checks++;
    /* Block, full-width, same signal and threshold as t_block_defaults. */
    if (rw < 350) { printf("  FAIL: <hr> box width=%d, want >=350 (full-width default)\n", rw); fails++; }
}

/* legend: block, proven the same width-shrink-vs-fill way as everything
 * above. fieldset gets no border/padding embellishment here on purpose --
 * this file is about display DEFAULTS ("the ones that matter most", per the
 * brief), not fieldset's box-model fidelity, and adding a border would only
 * make the width arithmetic below fuzzy without testing anything new. */
static void t_legend_block(void)
{
    printf("-- fieldset/legend default to display:block\n");
    page("<body style='margin:0'><fieldset style='margin:0'>"
         "<legend id=lg>Options</legend></fieldset></body>", 400);
    struct node *lg = ID("lg");
    int lx=0,ly=0,lw=0,lh=0;
    checks++;
    if (!(lg && layout_node_box(lg,&lx,&ly,&lw,&lh))) { printf("  FAIL: <legend> generated no box\n"); fails++; return; }
    checks++;
    if (lw < 350) { printf("  FAIL: <legend> box width=%d, want >=350 (still display:inline)\n", lw); fails++; }
}

/* ---- THE CONTROL: cascade origin must stay correct ----
 *
 * If UA_CSS's new rules were somehow appended at a HIGHER precedence than
 * author CSS -- the "make the UA sheet better and it starts winning" failure
 * named explicitly in the brief -- this is what it would look like: an
 * author rule at EQUAL specificity (one type selector, same as UA_CSS's own
 * `address{display:block}`) would lose. WATCH THIS ONE FAIL: run it against
 * a build where g_ua_sheet is appended with CSS_ORIGIN_AUTHOR instead of
 * CSS_ORIGIN_UA (a one-word sabotage of css_init()) and it must go red. */
static void t_author_wins_at_equal_specificity(void)
{
    printf("-- CONTROL: author CSS at equal specificity still beats the UA sheet\n");
    /* address is UA_CSS's `display:block`; the author sheet says
     * display:inline for the same tag, same specificity (0,0,1). If author
     * wins (the correct, must-stay-true outcome) #mid shrink-wraps to its
     * text -- the WRONG-looking, but CORRECT, narrow box. If the UA sheet
     * were ever wired in at higher precedence than CSS_ORIGIN_UA (the
     * one-word sabotage this control exists to catch), #mid would stay the
     * full-width block box t_block_defaults expects, which is backwards for
     * a control: this assertion is deliberately the MIRROR of that test's. */
    page_css("<body style='margin:0'><address id=mid>addr</address></body>",
             "address{display:inline}", 400);
    struct node *mid = ID("mid");
    int mx=0,my=0,mw=0,mh=0;
    checks++;
    if (!(mid && layout_node_box(mid,&mx,&my,&mw,&mh))) { printf("  FAIL: no #mid box\n"); fails++; return; }
    checks++;
    if (mw >= 350) {
        printf("  FAIL: author's `address{display:inline}` was OUTRANKED by the UA sheet "
               "(#mid width=%d, want <350/shrink-to-fit) -- cascade origin is broken\n", mw);
        fails++;
    }
}

/* ---- THE OTHER CONTROL, run the same way in the other direction: an
 * unstyled document still gets the real UA defaults (this is the "control
 * that watches the floor itself", separate from the origin check above). */
static void t_no_author_css_gets_ua_defaults(void)
{
    printf("-- CONTROL: zero author CSS still renders UA defaults (the floor itself)\n");
    page("<body style='margin:0'><aside id=mid>side content</aside></body>", 400);
    struct node *mid = ID("mid");
    int mx=0,my=0,mw=0,mh=0;
    checks++;
    if (!(mid && layout_node_box(mid,&mx,&my,&mw,&mh))) { printf("  FAIL: no #mid box\n"); fails++; return; }
    EQ(mw >= 350, 1, "no-author-CSS document still gets <aside>'s UA display:block");
}

/* ---- THE ANTI-MASKING CHECK: filling in UA gaps must not touch the
 * author-stylesheet parse/drop accounting css_report.c relies on. The UA
 * sheet is built once in css_init(), entirely off author_sheet()'s cache --
 * this asserts that stays true by checking css_sheet_parses() (the author
 * cache's own parse counter, exposed for exactly this kind of test) does not
 * move when a document has NO author CSS at all and only the (now-richer) UA
 * sheet is in play. If UA rules were somehow being routed through
 * author_sheet(), a thin UA sheet would start reading as "author CSS
 * present" -- the mask the brief warns about, from the opposite direction. */
static void t_ua_does_not_shadow_drop_reporting(void)
{
    printf("-- UA defaults do not masquerade as author CSS in the drop/parse accounting\n");
    int before = css_sheet_parses();
    page("<hr><address>a</address><aside>s</aside><dl><dt>t</dt><dd>d</dd></dl>"
         "<fieldset><legend>l</legend></fieldset><details><summary>x</summary></details>",
         400);
    int after = css_sheet_parses();
    /* An empty author sheet ("") still counts as ONE parse (of nothing) the
     * first time it's seen and then caches -- what must NOT happen is the
     * richer UA sheet inflating that count as if each UA rule were an author
     * parse. So the only legal outcomes are "no new parse" (cached from an
     * earlier call in this run) or exactly one (first time "" is seen). */
    checks++;
    if (after - before > 1) {
        printf("  FAIL: css_sheet_parses() moved by %d for a no-author-CSS page -- "
               "UA rules are leaking into the author accounting\n", after - before);
        fails++;
    }
}

int main(void)
{
    css_init();
    t_calibrate_width_signal();
    t_block_defaults();
    t_dl_children();
    t_hr_is_visible();
    t_legend_block();
    t_author_wins_at_equal_specificity();
    t_no_author_css_gets_ua_defaults();
    t_ua_does_not_shadow_drop_reporting();
    printf("\nua_default_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
