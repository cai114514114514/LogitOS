/* css_extra (border-radius capture) + css_viewport (@media) host test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "css.h"
#include "dom.h"

void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }

static int fail;
#define CST(n) ((struct cstyle *)(n)->style)
#define CHECK(c,m) do{ if(!(c)){printf("FAIL: %s\n",m);fail=1;} else printf("ok: %s\n",m);}while(0)

static struct node *find_by_class(struct node *n, const char *cls)
{
    if (n->type == N_ELEM) {
        const char *c = dom_attr(n, "class");
        if (c && strstr(c, cls)) return n;
    }
    for (struct node *c = n->first_child; c; c = c->next) {
        struct node *r = find_by_class(c, cls);
        if (r) return r;
    }
    return NULL;
}

static struct node *find_by_tag(struct node *n, const char *tag)
{
    if (n->type == N_ELEM && !strcmp(n->tag, tag)) return n;
    for (struct node *c = n->first_child; c; c = c->next) {
        struct node *r = find_by_tag(c, tag);
        if (r) return r;
    }
    return NULL;
}

/* substring search in a bounded, NOT NUL-terminated span (grid_raw points into
 * the compiled sheet, which is neither NUL-terminated nor ours to modify). */
static int strstr_n(const char *h, int n, const char *needle)
{
    int m = (int)strlen(needle);
    for (int i = 0; i + m <= n; i++) if (!memcmp(h + i, needle, (size_t)m)) return 1;
    return 0;
}

int main(void)
{
    css_init();
    const char *html =
        "<body>"
        "<div class='card'>a</div>"
        "<div class='avatar'>b</div>"
        "<span id='pill'>c</span>"
        "<em class='none'>d</em>"
        "<b class='inl' style='border-radius:9px'>e</b>"
        "</body>";
    const char *css =
        ".card { border-radius: 6px; color: #111 }"
        ".avatar { border-radius: 50% }"
        "#pill { border-radius:12px }"
        "div.card:hover { border-radius: 8px }";   /* pseudo stripped; .card already 6 -> stays 6 (later rule wins on .card match though) */
    struct node *root = dom_parse(html, (int)strlen(html));
    CHECK(root != NULL, "dom_parse");
    css_apply(root, css, (int)strlen(css));
    css_extra_apply(root, css, (int)strlen(css));

    struct node *card = find_by_class(root, "card");
    struct node *avatar = find_by_class(root, "avatar");
    struct node *inl = find_by_class(root, "inl");
    struct node *none = find_by_class(root, "none");
    CHECK(card && CST(card) && (CST(card)->radius == 6 || CST(card)->radius == 8),
          "class selector radius px");
    CHECK(avatar && CST(avatar) && CST(avatar)->radius_pct == 50, "percent radius");
    CHECK(inl && CST(inl) && CST(inl)->radius == 9, "inline style= radius");
    CHECK(none && CST(none) && CST(none)->radius == 0 && CST(none)->radius_pct == 0,
          "unmatched element keeps radius 0");

    /* @media responds to css_viewport */
    const char *html2 = "<body><div class='mq'>x</div></body>";
    const char *css2 = "@media (min-width: 1000px) { .mq { color: #112233 } }";
    struct node *r2 = dom_parse(html2, (int)strlen(html2));
    css_viewport(1180, 620);
    css_apply(r2, css2, (int)strlen(css2));
    struct node *mq = find_by_class(r2, "mq");
    CHECK(mq && CST(mq) && CST(mq)->color == 0x112233, "media min-width hits at 1180");
    struct node *r3 = dom_parse(html2, (int)strlen(html2));
    css_viewport(700, 500);
    css_apply(r3, css2, (int)strlen(css2));
    mq = find_by_class(r3, "mq");
    CHECK(mq && CST(mq) && CST(mq)->color != 0x112233, "media min-width misses at 700");

    /* visibility / opacity -> hidden flag (layout keeps space, paint skips) */
    const char *html4 =
        "<body><p class='v'>a</p><p class='o'>b</p><p class='k'>c</p>"
        "<div class='sr'>d</div><span hidden>e</span><noscript>f</noscript><template>g</template></body>";
    const char *css4 =
        ".v { visibility: hidden }"
        ".o { opacity: 0 }"
        ".k { visibility: visible }"
        ".sr { position:absolute; width:1px; height:1px; clip-path:inset(50%); overflow:hidden }";
    struct node *r4 = dom_parse(html4, (int)strlen(html4));
    css_apply(r4, css4, (int)strlen(css4));
    css_extra_apply(r4, css4, (int)strlen(css4));
    struct node *v = find_by_class(r4, "v");
    struct node *op = find_by_class(r4, "o");
    struct node *k = find_by_class(r4, "k");
    struct node *sr = find_by_class(r4, "sr");
    CHECK(v && CST(v) && CST(v)->hidden == 1, "visibility:hidden sets hidden");
    CHECK(op && CST(op) && CST(op)->hidden == 1, "opacity:0 sets hidden");
    CHECK(k && CST(k) && CST(k)->hidden == 0, "visible element stays unhidden");
    CHECK(sr && CST(sr) && CST(sr)->display == DISP_NONE, "clip-path visually-hidden -> display none");
    struct node *hspan = find_by_tag(r4, "span");
    struct node *noscr = find_by_tag(r4, "noscript");
    struct node *tmpl = find_by_tag(r4, "template");
    CHECK(hspan && CST(hspan) && CST(hspan)->display == DISP_NONE, "[hidden] attribute -> display none");
    CHECK(noscr && CST(noscr) && CST(noscr)->display == DISP_NONE, "noscript -> display none");
    CHECK(tmpl && CST(tmpl) && CST(tmpl)->display == DISP_NONE, "template -> display none");

    /* grid tracks: repeat(N,1fr) + grid-gap, px/fr lists, and @media tiers */
    const char *html5 =
        "<body><div class='gd'>x</div><div class='gl'>y</div>"
        "<div class='gm'>z</div></body>";
    const char *css5 =
        ".gd { display:grid; grid-template-columns:repeat(4,1fr); grid-gap:20px }"
        ".gl { display:grid; grid-template-columns:100px 1fr 2fr; column-gap:8px; row-gap:4px }"
        "@media (min-width:1140px) and (max-width:1299.9px) {"
        "  .gm { display:grid; grid-template-columns:repeat(4,1fr) } }"
        "@media (max-width:1139.9px) {"
        "  .gm { display:grid; grid-template-columns:repeat(2,1fr) } }";
    struct node *r5 = dom_parse(html5, (int)strlen(html5));
    css_viewport(1180, 620);
    css_apply(r5, css5, (int)strlen(css5));
    css_extra_apply(r5, css5, (int)strlen(css5));
    struct node *gd = find_by_class(r5, "gd");
    struct node *gl = find_by_class(r5, "gl");
    struct node *gm = find_by_class(r5, "gm");
    CHECK(gd && CST(gd) && CST(gd)->display == DISP_GRID, "display:grid mapped");
    CHECK(gd && CST(gd) && CST(gd)->grid_cols == 4 && CST(gd)->grid_tracks[0] == -10,
          "repeat(4,1fr) parsed to 4 fr tracks");
    CHECK(gd && CST(gd) && CST(gd)->grid_gap_x == 20 && CST(gd)->grid_gap_y == 20,
          "grid-gap px parsed");
    CHECK(gl && CST(gl) && CST(gl)->grid_cols == 3 && CST(gl)->grid_tracks[0] == 100 &&
          CST(gl)->grid_tracks[1] == -10 && CST(gl)->grid_tracks[2] == -20,
          "px/fr track list parsed");
    CHECK(gl && CST(gl) && CST(gl)->grid_gap_x == 8 && CST(gl)->grid_gap_y == 4,
          "column-gap/row-gap parsed");
    CHECK(gm && CST(gm) && CST(gm)->grid_cols == 4, "media tier 1140-1299.9 applies at 1180");
    struct node *r5b = dom_parse(html5, (int)strlen(html5));
    css_viewport(1000, 620);
    css_apply(r5b, css5, (int)strlen(css5));
    css_extra_apply(r5b, css5, (int)strlen(css5));
    gm = find_by_class(r5b, "gm");
    CHECK(gm && CST(gm) && CST(gm)->grid_cols == 2, "media tier max-1139.9 applies at 1000");
    css_viewport(1180, 620);

    /* THE `grid-template` SHORTHAND, in the exact shape Wikipedia's
     * Vector-2022 skin writes it -- which is how this gap was found. Its
     * narrow default writes the LONGHAND (one column) and its desktop rule,
     * inside a min-width media query, writes ONLY the shorthand. A scan that
     * reads longhands alone therefore keeps the mobile one-column layout at
     * every width: the table of contents lost its 196 px track, collapsed to
     * about one character per line and painted over the article. The
     * scoreboard called that page PAINTED for months, and Chrome's COMPUTED
     * style says `grid-template-columns: 248px 1220px`, so only reading the
     * STYLESHEET shows it.
     *
     * The columns half is what layout reads (grid_raw[GR_TEMPL_COLS], the
     * text layout_grid.c parses -- rem and minmax() live there, not in the
     * coarse grid_cols summary). Asserted on the raw text because that is the
     * value with the meaning; grid_cols stays 0 here precisely because
     * minmax() is not in the summary parser's vocabulary, and that is fine:
     * display:grid + grid_spec() is the path a real page takes. */
    const char *html7 =
        "<body><div class='wk'><div class='s'>side</div><div class='m'>main</div></div></body>";
    const char *css7 =
        ".wk{grid-template-columns:minmax(0,1fr);grid-template-areas:'a' 'b'}"
        "@media screen and (min-width:1120px){"
        ".wk{display:grid;column-gap:24px;"
        "grid-template:min-content 1fr min-content / 12.25rem minmax(0,1fr)}}";
    struct node *r7 = dom_parse(html7, (int)strlen(html7));
    css_apply(r7, css7, (int)strlen(css7));
    css_extra_apply(r7, css7, (int)strlen(css7));
    struct node *wk = find_by_class(r7, "wk");
    CHECK(wk && CST(wk) && CST(wk)->display == DISP_GRID, "shorthand block: display:grid mapped");
    CHECK(wk && CST(wk) && CST(wk)->grid_raw[GR_TEMPL_COLS] &&
          CST(wk)->grid_rawlen[GR_TEMPL_COLS] > 0 &&
          strstr_n(CST(wk)->grid_raw[GR_TEMPL_COLS], CST(wk)->grid_rawlen[GR_TEMPL_COLS],
                   "12.25rem"),
          "grid-template shorthand: the COLUMNS half reaches grid_raw");
    CHECK(wk && CST(wk) && CST(wk)->grid_raw[GR_TEMPL_COLS] &&
          !strstr_n(CST(wk)->grid_raw[GR_TEMPL_COLS], CST(wk)->grid_rawlen[GR_TEMPL_COLS],
                    "min-content"),
          "grid-template shorthand: the ROWS half is not mistaken for columns");
    CHECK(wk && CST(wk) && CST(wk)->grid_raw[GR_TEMPL_ROWS] &&
          strstr_n(CST(wk)->grid_raw[GR_TEMPL_ROWS], CST(wk)->grid_rawlen[GR_TEMPL_ROWS],
                   "min-content"),
          "grid-template shorthand: the rows half lands in GR_TEMPL_ROWS");
    CHECK(wk && CST(wk) && CST(wk)->grid_gap_x == 24, "column-gap beside the shorthand");

    /* AN APPROXIMATED SELECTOR MUST NOT END IN A BARE TAG.
     *
     * This file matches a descendant/sibling chain on its LAST compound only.
     * That over-matches by design and is harmless while the compound names a
     * class -- and catastrophic when it does not. Wikipedia hides the label
     * inside an icon-only button with
     *
     *   .cdx-button.cdx-button--icon-only span + span { ...clip:rect(1px...) }
     *
     * reduced here to `span`, which matched EVERY classless <span> on the
     * page; because that rule carries the visually-hidden pattern this file
     * turns into display:none, twelve of the thirteen entries in Wikipedia's
     * table of contents painted as EMPTY BOXES. `(Top)`, whose text is not
     * wrapped in a span, was the one that came out fine -- which is exactly
     * how the bug looked on screen before anyone knew what it was.
     *
     * Both directions are asserted: the far span must survive, and a selector
     * with NO combinator must still hide, because the fix refuses the
     * APPROXIMATION, not the pattern. The minified spelling (`span+span`, no
     * spaces) is the one real sheets use and is the case the fix had to learn
     * to cut. */
    const char *html8 =
        "<body>"
        "<button class='cdx-button cdx-button--icon-only'>"
        "  <span class='icon'></span><span>Toggle subsection</span></button>"
        "<div class='toc'><a><div class='txt'>"
        "  <span class='numb'>1</span><span>Definition and purpose</span>"
        "</div></a></div>"
        "<i>plain i</i>"
        "</body>";
    const char *css8 =
        ".cdx-button.cdx-button--icon-only span+span{position:absolute;clip:rect(1px,1px,1px,1px);width:1px}"
        "i{clip:rect(1px,1px,1px,1px)}";
    struct node *r8 = dom_parse(html8, (int)strlen(html8));
    css_apply(r8, css8, (int)strlen(css8));
    css_extra_apply(r8, css8, (int)strlen(css8));
    struct node *far = find_by_class(r8, "txt");
    struct node *lbl = far ? far->first_child : 0;
    while (lbl && !(lbl->type == N_ELEM && lbl->tag && !strcmp(lbl->tag, "span") &&
                    !dom_attr(lbl, "class")))
        lbl = lbl->next;
    CHECK(lbl && CST(lbl) && CST(lbl)->display != DISP_NONE,
          "a `.c span+span` sr-only rule does NOT hide unrelated classless spans");
    struct node *ital = find_by_tag(r8, "i");
    CHECK(ital && CST(ital) && CST(ital)->display == DISP_NONE,
          "an UN-approximated bare-tag rule still hides (the pattern is not refused)");

    /* opacity:0 + animation -> visible end state (hidden cleared);
     * opacity:0 + transition on opacity -> visible (scroll-reveal pattern);
     * opacity:0 + visibility:hidden + transition -> stays hidden (hover menu);
     * animation:none -> hidden. */
    const char *html6 =
        "<body><p class='fa'>a</p><p class='tr'>b</p><p class='an'>c</p>"
        "<p class='vh'>d</p><p class='sh'>e</p></body>";
    const char *css6 =
        ".fa { opacity:0; animation-name:fade-up }"
        ".tr { opacity:0; transition:opacity .3s }"
        ".an { opacity:0; animation:none !important }"
        ".vh { opacity:0; visibility:hidden; transition:opacity .3s }"
        ".sh { opacity:0; animation:.3s ease-in 1s forwards slide-in }";
    struct node *r6 = dom_parse(html6, (int)strlen(html6));
    css_apply(r6, css6, (int)strlen(css6));
    css_extra_apply(r6, css6, (int)strlen(css6));
    struct node *fa = find_by_class(r6, "fa");
    struct node *tr = find_by_class(r6, "tr");
    struct node *an = find_by_class(r6, "an");
    struct node *vh = find_by_class(r6, "vh");
    struct node *sh = find_by_class(r6, "sh");
    CHECK(fa && CST(fa) && CST(fa)->hidden == 0, "opacity:0 + animation-name -> visible");
    CHECK(sh && CST(sh) && CST(sh)->hidden == 0, "opacity:0 + animation shorthand -> visible");
    CHECK(tr && CST(tr) && CST(tr)->hidden == 0, "opacity:0 + transition:opacity -> visible (scroll-reveal)");
    CHECK(vh && CST(vh) && CST(vh)->hidden == 1, "opacity:0 + visibility:hidden + transition stays hidden");
    CHECK(an && CST(an) && CST(an)->hidden == 1, "opacity:0 + animation:none stays hidden");

    if (fail) { printf("FAILURES\n"); return 1; }
    printf("ALL PASS\n");
    return 0;
}
