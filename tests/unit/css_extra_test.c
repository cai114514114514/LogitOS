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

    if (fail) { printf("FAILURES\n"); return 1; }
    printf("ALL PASS\n");
    return 0;
}
