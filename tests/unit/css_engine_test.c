/* Host test for the LibCSS-backed CSS engine (user/css_engine.c).
 * Compiles css_engine.c + net/dom.c + all of LibCSS natively and checks that
 * the produced struct cstyle matches expectations (UA + author + inline). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dom.h"
#include "css.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

static struct cstyle *st(struct node *n) { return (struct cstyle *)n->style; }
static struct node *find(struct node *n, const char *tag)
{
    if (n->type == N_ELEM && strcmp(n->tag, tag) == 0) return n;
    for (struct node *c = n->first_child; c; c = c->next) {
        struct node *r = find(c, tag);
        if (r) return r;
    }
    return NULL;
}

static struct node *find_id(struct node *n, const char *id)
{
    if (n->type == N_ELEM) {
        const char *v = dom_attr(n, "id");
        if (v && strcmp(v, id) == 0) return n;
    }
    for (struct node *c = n->first_child; c; c = c->next) {
        struct node *r = find_id(c, id);
        if (r) return r;
    }
    return NULL;
}

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails = 1; } else printf("ok: %s\n", m); } while (0)

int main(void)
{
    const char *html =
        "<html><head><style>.hi{color:#ff0000} p{font-size:20px}</style></head>"
        "<body><h1>Title</h1><p class='hi'>para</p>"
        "<a href='/y'>link</a>"
        "<div style='background:#00ff00;width:200px'>box</div>"
        "<pre>code</pre></body></html>";
    struct node *root = dom_parse(html, (int)strlen(html));
    if (!root) { printf("FAIL: dom_parse\n"); return 1; }

    css_init();
    const char *author = ".hi{color:#ff0000} p{font-size:20px}";
    css_apply(root, author, (int)strlen(author));

    struct node *h1 = find(root, "h1"), *p = find(root, "p"),
                *a = find(root, "a"), *div = find(root, "div"), *pre = find(root, "pre");

    CHECK(h1 && st(h1), "h1 has computed style");
    CHECK(st(h1)->display == DISP_BLOCK, "h1 display:block (UA)");
    CHECK(st(h1)->font_px == 32, "h1 font-size 32 (UA)");
    CHECK(st(h1)->bold, "h1 bold (UA)");
    CHECK(st(p) && st(p)->display == DISP_BLOCK, "p display:block (UA)");
    CHECK(st(p)->font_px == 20, "p font-size 20 (author)");
    CHECK(st(p)->color == 0xff0000, "p.hi color red (author class)");
    CHECK(st(a) && st(a)->color == 0x1a0dab, "a link color blue (UA)");
    CHECK(st(a)->underline, "a underline (UA)");
    CHECK(st(div) && st(div)->has_bg && st(div)->background == 0x00ff00, "div bg green (inline)");
    CHECK(st(div)->has_w && st(div)->width == 200, "div width 200px (inline)");
    CHECK(st(pre) && st(pre)->mono, "pre monospace (UA)");

    /* ---- quirks mode ----
     * The doctype (or its absence) decides. No doctype -> QM_QUIRKS, and
     * css_apply must then (a) hand LibCSS allow_quirks so unitless lengths and
     * hashless hex colours parse instead of being dropped, and (b) append the
     * quirks UA sheet so <table> stops inheriting the body font. */
    const char *qhtml =                      /* no doctype == quirks mode */
        "<body style='font-size:40px'>"
        "<div id='q' style='width:150;color:FF0000'>x</div>"
        "<table><tr><td>cell</td></tr></table>"
        "</body>";
    struct node *qroot = dom_parse(qhtml, (int)strlen(qhtml));
    CHECK(dom_doc_quirks(qroot->doc) == QM_QUIRKS, "no doctype -> quirks mode");
    css_apply(qroot, "", 0);
    struct node *qd = find(qroot, "div"), *qt = find(qroot, "table");
    CHECK(qd && st(qd)->has_w && st(qd)->width == 150, "quirks: unitless width:150 -> 150px");
    CHECK(qd && st(qd)->color == 0xff0000, "quirks: hashless colour FF0000 parsed");
    CHECK(qt && st(qt)->font_px == 16, "quirks: table does not inherit body font-size");

    /* The same markup with a standards doctype must do NEITHER: the quirky
     * declarations are dropped (so the div keeps its defaults) and the table
     * inherits normally. If this passes while the block above fails, the
     * quirks flag is stuck on. */
    const char *shtml =
        "<!DOCTYPE html><body style='font-size:40px'>"
        "<div id='q' style='width:150;color:FF0000'>x</div>"
        "<table><tr><td>cell</td></tr></table>"
        "</body>";
    struct node *sroot = dom_parse(shtml, (int)strlen(shtml));
    CHECK(dom_doc_quirks(sroot->doc) == QM_NO_QUIRKS, "<!DOCTYPE html> -> no quirks");
    css_apply(sroot, "", 0);
    struct node *sd = find(sroot, "div"), *stb = find(sroot, "table");
    CHECK(sd && !st(sd)->has_w, "standards: unitless width:150 dropped");
    CHECK(sd && st(sd)->color != 0xff0000, "standards: hashless colour FF0000 dropped");
    CHECK(stb && st(stb)->font_px == 40, "standards: table inherits the body font-size");

    /* ---- units ----
     * Everything except px used to fall through len_px's default and be taken
     * as a bare number, so `width:100vw` was 100px and `font-size:12pt` was
     * 12px. The viewport is pinned here so vw/vh/vmin/vmax are checkable.
     * html{font-size:20px} makes `rem` distinguishable from `em`. */
    css_viewport(1000, 500);
    const char *uhtml =
        "<!DOCTYPE html><html><body>"
        "<div id='vw'></div><div id='vh'></div><div id='vmin'></div><div id='vmax'></div>"
        "<div id='pt'></div><div id='in'></div><div id='cm'></div><div id='mm'></div>"
        "<div id='pc'></div><div id='rem'></div><div id='em'></div>"
        "<div id='calc1'></div><div id='calc2'></div>"
        "</body></html>";
    const char *ucss =
        "html{font-size:20px}"
        "#vw{width:50vw}#vh{width:10vh}#vmin{width:10vmin}#vmax{width:10vmax}"
        "#pt{width:12pt}#in{width:1in}#cm{width:2.54cm}#mm{width:10mm}#pc{width:1pc}"
        "#rem{width:2rem}#em{font-size:32px;width:2em}"
        "#calc1{width:calc(100% - 20px)}#calc2{width:calc(50% + 10px)}";
    struct node *uroot = dom_parse(uhtml, (int)strlen(uhtml));
    css_apply(uroot, ucss, (int)strlen(ucss));
    struct cstyle *u;
#define BYID(name) (u = st(find_id(uroot, name)))
    CHECK(BYID("vw") && u->has_w && !u->w_pct && u->width == 500, "50vw of a 1000px viewport = 500px");
    CHECK(BYID("vh") && u->width == 50, "10vh of a 500px viewport = 50px");
    CHECK(BYID("vmin") && u->width == 50, "10vmin picks the smaller axis (500)");
    CHECK(BYID("vmax") && u->width == 100, "10vmax picks the larger axis (1000)");
    CHECK(BYID("pt") && u->width == 16, "12pt = 16px (96/72)");
    CHECK(BYID("in") && u->width == 96, "1in = 96px");
    CHECK(BYID("cm") && u->width == 96, "2.54cm = 96px");
    CHECK(BYID("mm") && u->width == 38, "10mm = 38px (37.8 rounded)");
    CHECK(BYID("pc") && u->width == 16, "1pc = 16px");
    CHECK(BYID("rem") && u->width == 40, "2rem uses the ROOT font-size (20px), not a hardcoded 16");
    CHECK(BYID("em") && u->width == 64, "2em uses the element's own font-size (32px)");
    /* calc() is stored unresolved by LibCSS, which reports it as `auto`.
     * css_engine probes the used value at two available widths and recovers the
     * linear model, so the percentage and the px addend both survive. */
    CHECK(BYID("calc1") && u->has_w && u->w_pct && u->width == 100 && u->w_off == -20,
          "calc(100% - 20px) -> 100% with a -20px addend");
    CHECK(BYID("calc2") && u->has_w && u->w_pct && u->width == 50 && u->w_off == 10,
          "calc(50% + 10px) -> 50% with a +10px addend");
#undef BYID

    /* ---- the properties convert() used to throw away ---- */
    const char *phtml =
        "<!DOCTYPE html><html><body>"
        "<div id='bb'>a</div><pre id='pre'>b</pre><div id='nw'>c</div>"
        "<div id='flexc'><div id='fi'>d</div></div>"
        "<div id='mm2'>e</div><div id='zz'>f</div><div id='fl'>g</div>"
        "<div id='ov'>h</div><div id='td'>i</div><div id='op'>j</div>"
        "<ol id='ro'><li>k</li></ol>"
        "</body></html>";
    const char *pcss =
        "#bb{box-sizing:border-box}"
        "#nw{white-space:nowrap}"
        "#flexc{display:flex;flex-direction:column-reverse;flex-wrap:wrap-reverse;"
        "justify-content:space-evenly;align-items:flex-end}"
        "#fi{flex-grow:2;flex-shrink:3;flex-basis:40%;order:-2;align-self:center}"
        "#mm2{min-width:10px;max-width:20px;min-height:30px;max-height:40px}"
        "#zz{position:fixed;z-index:-3;right:11px;bottom:12px}"
        "#fl{float:right;clear:both}"
        "#ov{overflow-x:hidden;overflow-y:scroll}"
        "#td{text-decoration:line-through overline}"
        "#op{opacity:0.5;background:rgba(0,0,0,0.25)}"
        "#ro{list-style-type:upper-roman}";
    struct node *proot = dom_parse(phtml, (int)strlen(phtml));
    css_apply(proot, pcss, (int)strlen(pcss));
    struct cstyle *pv;
#define BYID(name) (pv = st(find_id(proot, name)))
    CHECK(BYID("bb") && pv->box_sizing == BOX_BORDER, "box-sizing:border-box read");
    CHECK(BYID("pre") && pv->white_space == WS_PRE, "pre gets white-space:pre from the UA sheet");
    CHECK(BYID("nw") && pv->white_space == WS_NOWRAP, "white-space:nowrap read");
    CHECK(BYID("flexc") && pv->flex_dir == FDIR_COL_REV && pv->flex_wrap == FWRAP_WRAP_REV &&
          pv->justify == JC_EVENLY && pv->align_items == AL_END, "flex container properties read");
    CHECK(BYID("fi") && pv->flex_grow == 2048 && pv->flex_shrink == 3072 &&
          pv->has_fb && pv->fb_pct && pv->flex_basis == 40 && pv->order == -2 &&
          pv->align_self == AL_CENTER, "flex item properties read");
    CHECK(BYID("mm2") && pv->has_min_w && pv->min_w == 10 && pv->has_max_w && pv->max_w == 20 &&
          pv->has_min_h && pv->min_h == 30 && pv->has_max_h && pv->max_h == 40,
          "min/max width and height read");
    CHECK(BYID("zz") && pv->position == POS_FIXED && pv->has_z && pv->z_index == -3 &&
          pv->has_right && pv->right == 11 && pv->has_bottom && pv->bottom == 12,
          "position:fixed + negative z-index + right/bottom read");
    CHECK(BYID("fl") && pv->flt == FLT_RIGHT && pv->clr == CLR_BOTH, "float/clear read");
    CHECK(BYID("ov") && pv->overflow_x == OVF_HIDDEN && pv->overflow_y == OVF_SCROLL,
          "overflow-x/overflow-y read");
    CHECK(BYID("td") && pv->strike && pv->overline && !pv->underline,
          "text-decoration line-through + overline read (and no stray underline)");
    CHECK(BYID("op") && pv->opacity == 128 && !pv->hidden,
          "opacity:0.5 -> 128/255, still visible");
    CHECK(BYID("op") && pv->has_bg && pv->bg_alpha == 63,
          "rgba() alpha survives into bg_alpha");
    CHECK(BYID("ro") && pv->list_style == LST_UPPER_ROMAN, "list-style-type read");
    { struct node *li = find(proot, "li");
      CHECK(li && st(li)->list_style == LST_UPPER_ROMAN, "list-style-type inherits to the <li>"); }
    /* min-width:auto (the flex-item initial) must NOT look like an authored
     * `min-width:0` -- LibCSS's public accessor reports it as SET 0 for any
     * element that is not itself a flex container. */
    CHECK(BYID("bb") && !pv->has_min_w, "min-width:auto is not reported as an authored min-width");
#undef BYID

    /* ---- LibCSS node-data cache ----
     * Both handlers were no-ops, so the parent bloom was rebuilt (fully
     * saturated, i.e. useless) for every element and the per-element node_data
     * leaked. A styling pass over same-tag siblings must now register cache
     * hits, and the styles must still be right: :nth-child taints the sharing
     * candidate, so the second <li> must keep its own colour. */
    int styled0, hits0;
    css_stats(&styled0, &hits0);
    const char *chtml =
        "<!DOCTYPE html><html><body><ul>"
        "<li>a</li><li>b</li><li>c</li><li>d</li><li>e</li><li>f</li>"
        "</ul></body></html>";
    const char *ccss = "li{color:#010203}li:nth-child(2){color:#0a0b0c}";
    struct node *croot = dom_parse(chtml, (int)strlen(chtml));
    css_apply(croot, ccss, (int)strlen(ccss));
    int styled1, hits1;
    css_stats(&styled1, &hits1);
    CHECK(styled1 - styled0 == 10, "styled html+head+body+ul+6li");
    CHECK(hits1 - hits0 > 0, "node-data cache is live (parent bloom / sibling share hits)");
    { struct node *li = croot->first_child;
      int idx = 0; struct cstyle *c1 = 0, *c2 = 0, *c3 = 0;
      /* walk to the <ul> and read the first three <li> */
      struct node *ul = find(croot, "ul");
      for (struct node *k = ul ? ul->first_child : 0; k; k = k->next) {
          if (k->type != N_ELEM) continue;
          if (idx == 0) c1 = st(k);
          if (idx == 1) c2 = st(k);
          if (idx == 2) c3 = st(k);
          idx++;
      }
      (void)li;
      CHECK(c1 && c1->color == 0x010203, "first li keeps the plain rule");
      CHECK(c2 && c2->color == 0x0a0b0c, ":nth-child(2) still wins with sharing enabled");
      CHECK(c3 && c3->color == 0x010203, "third li did not inherit the shared nth-child style");
    }

    /* Free everything so the ASan/LSan run has a clean exit: the point of the
     * cache work is that LibCSS's per-element node_data no longer leaks, and a
     * leak report full of our own documents would hide that. */
    dom_free(root); dom_free(qroot); dom_free(sroot);
    dom_free(uroot); dom_free(proot); dom_free(croot);

    printf(fails ? "\nCSS ENGINE TEST FAILED\n" : "\nCSS ENGINE TEST PASSED\n");
    return fails;
}
