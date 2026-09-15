/* Real DOM -> LibCSS ordinary cascade -> css_extra matching, in the same
 * order as browser.c. These checks observe captured extension declarations,
 * not merely parser acceptance. The control compiles the former matcher and
 * must visibly mis-style ancestors/states/pseudo targets. Host capture proves
 * the common CSS cause; real-page pixels/input are a separate guest gate. */
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
    css_extra_apply(g_root, exp, el); /* browser.c:2040; grid/logical producers */
    layout_page(g_root, cw);
}



extern int css_extra_rejected_rules(void);
static void selector_case(const char *label, const char *selector, const char *body, int want)
{
    char html[4096];
    snprintf(html, sizeof html, "<!doctype html><style>%s{transform:translateX(12px)}</style>%s", selector, body);
    page(html, 640);
    struct node *n = ID("n");
    CHECK(n && !!((struct cstyle *)n->style)->xrawlen[XR_TRANSFORM] == want, label);
    dom_free(g_root); g_root = NULL;
}
int main(void)
{
    selector_case("ancestor attribute must match", "body[data-mode=dark] .nav", "<body><a id=n class=nav>News</a></body>", 0);
    selector_case("matching ancestor attribute is kept", "body[data-mode=dark] .nav", "<body data-mode=dark><a id=n class=nav>News</a></body>", 1);
    selector_case("ancestor class cannot be dropped", ".panel .nav", "<a id=n class=nav>News</a>", 0);
    selector_case("descendant through intermediate element", ".panel .nav", "<div class=panel><p><a id=n class=nav>News</a></p></div>", 1);
    selector_case("child combinator rejects grandchild", ".panel > .nav", "<div class=panel><p><a id=n class=nav>News</a></p></div>", 0);
    selector_case("child combinator accepts direct child", ".panel > .nav", "<div class=panel><a id=n class=nav>News</a></div>", 1);
    selector_case("adjacent sibling rejects gap", ".lead + .nav", "<b class=lead></b><i></i><a id=n class=nav>News</a>", 0);
    selector_case("general sibling accepts gap", ".lead ~ .nav", "<b class=lead></b><i></i><a id=n class=nav>News</a>", 1);
    selector_case("hover state is not fabricated", ".nav:hover", "<a id=n class=nav>News</a>", 0);
    selector_case("focus state is not fabricated", ".nav:focus", "<a id=n class=nav>News</a>", 0);
    selector_case("legacy pseudo element never styles link", ".nav:hover:before", "<a id=n class=nav>News</a>", 0);
    selector_case("pseudo element never styles origin", ".nav::before", "<a id=n class=nav>News</a>", 0);
    CHECK(css_extra_rejected_rules() == 1, "pseudo-only rule counted as refused");
    selector_case("mixed list keeps real element arm", ".nav::before, .nav", "<a id=n class=nav>News</a>", 1);
    selector_case("mixed list does not leak pseudo arm", ".nav::before, .other", "<a id=n class=nav>News</a>", 0);
    selector_case("unknown pseudo is refused", ".nav:logit-unsupported", "<a id=n class=nav>News</a>", 0);
    CHECK(css_extra_rejected_rules() == 1, "unsupported selector counted as refused");
    selector_case("third class cannot be ignored", ".a.b.c", "<a id=n class='a b'>News</a>", 0);
    selector_case("three classes match", ".a.b.c", "<a id=n class='a b c'>News</a>", 1);
    selector_case("not condition rejects excluded node", ".nav:not(.excluded)", "<a id=n class='nav excluded'>News</a>", 0);
    selector_case("not condition keeps included node", ".nav:not(.excluded)", "<a id=n class=nav>News</a>", 1);
    selector_case("first-child rejects later child", ".nav:first-child", "<div><i></i><a id=n class=nav>News</a></div>", 0);
    selector_case("nth-child matches real position", ".nav:nth-child(2)", "<div><i></i><a id=n class=nav>News</a></div>", 1);
    selector_case("attribute value must match", ".nav[data-mode=dark]", "<a id=n class=nav data-mode=light>News</a>", 0);
    selector_case("attribute value matches", ".nav[data-mode=dark]", "<a id=n class=nav data-mode=dark>News</a>", 1);
    selector_case("checked state accepts checked control", "input:checked", "<input id=n type=checkbox checked>", 1);
    selector_case("checked state rejects unchecked control", "input:checked", "<input id=n type=checkbox>", 0);
    selector_case("disabled state accepts disabled control", "input:disabled", "<input id=n disabled>", 1);
    selector_case("disabled state rejects enabled control", "input:disabled", "<input id=n>", 0);
    selector_case("long selector list retains exact final arm", ".a,.b,.c,.d,.e,.f,.panel .nav", "<a id=n class=nav>News</a>", 0);
    selector_case("long selector list accepts final arm", ".a,.b,.c,.d,.e,.f,.panel .nav", "<div class=panel><a id=n class=nav>News</a></div>", 1);
    /* Cached compiled selector sheet is reused, but match results are never
     * reused across different DOMs (the normal cascade's node cache is separate). */
    selector_case("cached sheet matches first DOM", ".panel .nav", "<div class=panel><a id=n class=nav>News</a></div>", 1);
    selector_case("cached sheet rejects replacement DOM", ".panel .nav", "<a id=n class=nav>News</a>", 0);
    printf("extra selectors: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
