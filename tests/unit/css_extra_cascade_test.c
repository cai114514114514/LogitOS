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



static void run(const char *label,const char *rules,const char *inl,const char *want,int gap){
 char html[4096];snprintf(html,sizeof html,"<!doctype html><style>%s</style><div id=n class='a b' style='%s'>box</div>",rules,inl);
 page(html,640);struct cstyle *st=ID("n")->style;
 if(want) CHECK(st->xraw[XR_TRANSFORM] && st->xrawlen[XR_TRANSFORM]==strlen(want) && !memcmp(st->xraw[XR_TRANSFORM],want,strlen(want)),label);
 if(gap>=0) EQ(st->grid_gap_x,gap,label);
 dom_free(g_root);g_root=NULL;
}
int main(void){
 run("ID specificity beats later class","#n{transform:translateX(1px)}.a{transform:translateX(2px)}","","translateX(1px)",-1);
 run("compound specificity beats later type",".a.b{transform:translateX(1px)}div{transform:translateX(2px)}","","translateX(1px)",-1);
 run("equal specificity keeps source order",".a{transform:translateX(1px)}.b{transform:translateX(2px)}","","translateX(2px)",-1);
 run("only matched selector list arm contributes specificity","#other,.a{transform:translateX(1px)}div.b{transform:translateX(2px)}","","translateX(2px)",-1);
 run("highest matched selector list arm contributes specificity","#n,.a{transform:translateX(1px)}div.b{transform:translateX(2px)}","","translateX(1px)",-1);
 run("pseudo target cannot donate specificity","#n::before,.a{transform:translateX(1px)}div.b{transform:translateX(2px)}","","translateX(2px)",-1);
 run("important beats higher normal specificity",".a{transform:translateX(1px)!important}#n{transform:translateX(2px)}","","translateX(1px)",-1);
 run("important rules retain specificity","#n{transform:translateX(1px)!important}.a{transform:translateX(2px)!important}","","translateX(1px)",-1);
 run("normal inline beats normal ID","#n{transform:translateX(1px)}","transform:translateX(2px)","translateX(2px)",-1);
 run("stylesheet important beats normal inline",".a{transform:translateX(1px)!important}","transform:translateX(2px)","translateX(1px)",-1);
 run("important inline beats stylesheet important","#n{transform:translateX(1px)!important}","transform:translateX(2px)!important","translateX(2px)",-1);
 run("same block important survives later normal",".a{transform:translateX(1px)!important;transform:translateX(2px)}","","translateX(1px)",-1);
 run("same block important uses declaration order",".a{transform:translateX(1px)!important;transform:translateX(2px)!important}","","translateX(2px)",-1);
 run("important is per property",".a{transform:translateX(1px)!important;gap:3px}#n{transform:translateX(2px);gap:7px}","","translateX(1px)",7);
 run("inline declaration important survives later normal","","transform:translateX(1px)!important;transform:translateX(2px)","translateX(1px)",-1);
 run("case insensitive important and trivia",".a{transform:translateX(1px) ! IMPORTANT /*ok*/}#n{transform:translateX(2px)}","","translateX(1px)",-1);
 run("later shorthand overrides previous longhand",".a{column-gap:3px;gap:7px}","",NULL,7);
 run("later longhand overrides previous shorthand",".a{gap:7px;column-gap:3px}","",NULL,3);
 run("important longhand survives shorthand",".a{column-gap:3px!important;gap:7px}","",NULL,3);
 run("unrelated rule leaves captured property","#n{transform:translateX(1px)}.a{gap:7px}","","translateX(1px)",7);
 printf("css-extra-cascade: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
