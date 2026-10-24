/* Real HTML parser, native cascade and extension producer. No author-side
 * implementation is used as the oracle. Host fields settle parse/cascade;
 * the paired ordinary HTML specimen settles geometry/pixels in the guest. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dom/tree.h"
#include "css/style.h"
#include "css/background.h"
#include "css/nesting.h"
#include <libcss/errors.h>
extern int css_extra_matcher_error(void *);
extern void *css_extra_matcher_create(const char *,int,int,unsigned char *);
extern int css_extra_matcher_match(void *,struct node *,unsigned char *);
extern void css_extra_matcher_destroy(void *);

static int allocation_fault=-1,allocation_count;
void *kmalloc(unsigned long n)
{int index=allocation_count++;return index==allocation_fault?NULL:malloc(n);}
void kfree(void *p){free(p);}
int text_measure(const char *s,int n,int px,int mono)
{(void)s;(void)mono;return n*(px/2);}
static int checks,failures;
static struct node *root;
static void check(int condition,const char *name)
{checks++;printf("%s: %s\n",condition?"ok":"FAIL",name);if(!condition)failures++;}
static struct node *find(struct node *n,const char *id)
{
    const char *v=n->type==N_ELEM?dom_attr(n,"id"):NULL;
    if(v&&!strcmp(v,id))return n;
    for(struct node *c=n->first_child;c;c=c->next){struct node *r=find(c,id);if(r)return r;}
    return NULL;
}
static struct cstyle *style(const char *id)
{struct node *n=find(root,id);return n?(struct cstyle *)n->style:NULL;}
static void page(const char *rules,const char *body)
{
    if(root)dom_free(root);
    root=dom_parse(body,(int)strlen(body));
    css_apply(root,rules,(int)strlen(rules));css_extra_apply(root,rules,(int)strlen(rules));
}
static void width_is(const char *id,int width,const char *label)
{struct cstyle *s=style(id);check(s&&s->has_w&&s->width==width,label);}
static void ratio_is(const char *id,double w,double h,int automatic,const char *label)
{struct cstyle *s=style(id);check(s&&s->aspect_w==w&&s->aspect_h==h&&s->aspect_auto==automatic,label);}
static void nesting(void)
{
    page(".parent{width:10px;&.active{width:20px}.child{width:30px}}",
         "<div id=p class='parent active'><span id=c class=child></span></div><span id=x class=child></span>");
    width_is("p",20,"nesting: ampersand selects parent compound");
    width_is("c",30,"nesting: implicit descendant stays in parent");
    check(!style("x")->has_w,"nesting: implicit descendant excludes outside sibling");
    page(".parent{> .child,+ .next{width:31px}}",
         "<div class=parent><span id=c class=child></span></div><span id=n class=next></span>");
    width_is("c",31,"nesting: relative child list arm");width_is("n",31,"nesting: relative adjacent list arm");
    page("#unmatched,.parent{.child{width:40px}}.parent .child{width:50px}",
         "<div class=parent><span id=c class=child></span></div>");
    width_is("c",40,"nesting: unmatched parent id supplies maximum specificity");
    page("article{width:10px;&{width:20px}width:30px}","<article id=c></article>");
    width_is("c",30,"nesting: declaration after child keeps source order");
    page("#unmatched,article{width:10px;&{width:20px}width:30px}","<article id=c></article>");
    width_is("c",20,"nesting: bare trailing declaration keeps original arm specificity");
    page("main > .parent{.child{&.active{width:41px}}}",
         "<main><div class=parent><span id=c class='child active'></span></div></main><div class=parent><span id=x class='child active'></span></div>");
    width_is("c",41,"nesting: complex parent and multilevel child");
    check(!style("x")->has_w,"nesting: complex parent excludes unmatched ancestry");
    page(".parent{&&{width:42px}&[data-x='&']{height:43px}}",
         "<div id=c class=parent data-x='&'></div>");
    width_is("c",42,"nesting: repeated ampersands retain both components");
    check(style("c")->has_h&&style("c")->height==43,"nesting: quoted attribute ampersand stays literal");
    page(".parent{&suffix{width:44px} @container (width>1px){width:45px}}", "<div id=c class=parent></div>");
    check(!style("c")->has_w,"nesting: unsupported concatenation and nested container refuse");
    page(".parent,:made-up{&{width:46px}}", "<div id=c class=parent></div>");
    check(!style("c")->has_w,"nesting: invalid parent list cannot revive through forgiving is");
    page(".parent{@media(min-width:1px){width:47px;&.active{height:48px}}}","<div id=c class='parent active'></div>");
    width_is("c",47,"nesting: conditional bare declaration uses parent context");
    check(style("c")->has_h&&style("c")->height==48,"nesting: nested style in conditional keeps parent context");
    char *one=NULL,*two=NULL;int a=0,b=0;
    const char *input=".p{width:10px;>.c{width:20px}height:30px}";
    int ok=css_normalize_stylesheet(input,(int)strlen(input),&one,&a);
    int again=ok>=0?css_normalize_stylesheet(one?one:input,a,&two,&b):-1;
    check(ok>=0&&again>=0&&a==b&&!memcmp(one?one:input,two?two:(one?one:input),(unsigned)a),"nesting: normalization is idempotent");
    free(one);free(two);
}
static void selectors(void)
{
    page(":is(.a > .b .c){width:51px}",
         "<div class=a><div class=b><div><div class=b><span id=c class=c></span></div></div></div></div>");
    width_is("c",51,"selector: complex alternative backtracks farther ancestor");
    page(":is(main > .c,aside + .c){width:52px}:where(main > .c){height:53px}",
         "<main><span id=c class=c></span></main><aside></aside><span id=n class=c></span><span id=x class=c></span>");
    width_is("c",52,"selector: complex child alternative");width_is("n",52,"selector: complex adjacent alternative");
    check(!style("x")->has_w,"selector: complex alternative excludes unmatching sibling");
    check(style("c")->has_h&&style("c")->height==53,"selector: complex where matches");
    page(".c{width:54px}:where(#c,.a > .c){width:55px}","<div class=a><span id=c class=c></span></div>");
    width_is("c",54,"selector: where complex alternative has zero specificity");
    page(":is(.c::before,.valid){width:56px}:is(.c::before){height:57px}",
         "<span id=c class=c></span><span id=v class=valid></span>");
    check(!style("c")->has_w&&!style("c")->has_h,"selector: pseudo-element alternatives never select originating element");
    width_is("v",56,"selector: forgiving list keeps valid arm after pseudo-element refusal");
    page(":not(.c::before){width:58px}:is(:made-up,.valid){height:59px}",
         "<span id=c class=c></span><span id=v class=valid></span>");
    check(!style("c")->has_w&&!style("v")->has_w,"selector: not pseudo-element arm rejects whole strict list");
    check(style("v")->has_h&&style("v")->height==59,"selector: forgiving list drops unknown pseudo arm");
    page(":not(.a > .c){width:60px}","<div class=a><span id=c class=c></span></div><span id=x class=c></span>");
    check(!style("c")->has_w,"selector: complex not negates matching arm");width_is("x",60,"selector: complex not accepts unmatched arm");
}
static void sizing(void)
{
    page("#c{aspect-ratio:auto 1.5 / 2e0}#d{aspect-ratio:3/2 auto}","<div id=c></div><div id=d></div>");
    ratio_is("c",1.5,2,1,"ratio: decimals exponent and leading auto");ratio_is("d",3,2,1,"ratio: trailing auto");
    page("#c{aspect-ratio:2/1;aspect-ratio:1.;aspect-ratio:1.e2;aspect-ratio:-1;aspect-ratio:auto auto}","<div id=c></div>");
    ratio_is("c",2,1,0,"ratio: invalid later declarations preserve valid winner");
    page("#c{aspect-ratio:auto/**/3/**// /**/2}","<div id=c></div>");
    ratio_is("c",3,2,1,"ratio: comments are token trivia");
    page("#c{aspect-ratio:0/2}","<div id=c></div>");ratio_is("c",0,2,0,"ratio: degenerate zero is valid syntax");
    page("#p{aspect-ratio:3/2}#c{aspect-ratio:inherit}#d{aspect-ratio:initial}","<div id=p><div id=c></div><div id=d></div></div>");
    ratio_is("c",3,2,0,"ratio: inherit resolves parent first");ratio_is("d",0,0,1,"ratio: initial restores auto");
    page(".c{aspect-ratio:3/2!important}#c{aspect-ratio:1/1}","<div id=c class=c style='aspect-ratio:4/1'></div>");
    ratio_is("c",3,2,0,"ratio: importance beats id and inline normal");
    page(".c{aspect-ratio:3/2!important}","<div id=c class=c style='aspect-ratio:4/1!important'></div>");
    ratio_is("c",4,1,0,"ratio: inline important wins author important");
    page("#c{justify-content:normal;align-items:normal}#s{justify-content:start;align-items:stretch}#t{justify-content:stretch}","<div id=c></div><div id=s></div><div id=t></div>");
    check(style("c")->justify==JC_NORMAL&&style("c")->align_items==AL_NORMAL,"ratio: normal alignment remains distinct");
    check(style("s")->justify==JC_START&&style("s")->align_items==AL_STRETCH,"ratio: explicit start and stretch remain distinct");
    check(style("t")->justify==JC_STRETCH,"ratio: justify stretch remains distinct");
}
static void supports(void)
{
    check(css_supports_decl("aspect-ratio",-1,"auto 16/9",-1),"supports: ratio parser affirms valid value");
    check(!css_supports_decl("aspect-ratio",-1,"1.",-1)&&!css_supports_decl("aspect-ratio",-1,"1.e2",-1),"supports: ratio parser refuses trailing decimal dot");
    check(!css_supports_decl("aspect-ratio",-1,"-1/2",-1),"supports: ratio parser refuses negative value");
    check(css_supports_decl("background-image",-1,"url('a.png')",-1),"supports: bitmap background uses real producer grammar");
    check(css_supports_decl("background-image",-1,"linear-gradient(red,blue)",-1),"supports: gradient background uses real producer grammar");
    /* Old boundary: lists were absent. All layers now load/paint through the
     * same used-list road; the separately sampled image gates prove the yes. */
    check(css_supports_decl("background-image",-1,"url(a),url(b)",-1),"supports: background multilayer uses the real list producer");
    check(css_supports_decl("background-size",-1,"cover",-1)&&!css_supports_decl("background-size",-1,"made-up",-1),"supports: size value is validated before name-only canonicalizer");
    page("#c{width:61px;aspect-ratio:3/2}@supports(display:made-up){#c{width:62px;aspect-ratio:1/1;--x:63px}}#d{width:var(--x,64px)}", "<div id=c></div><div id=d></div>");
    /* Exercise the real vars entrance too, since native-only @supports never
     * proved the old pre-pass and extension scanner respected the condition. */
    const char *rules="#c{width:61px;aspect-ratio:3/2}@supports(display:made-up){#c{width:62px;aspect-ratio:1/1;--x:63px}}#d{width:var(--x,64px)}";
    char expanded[12000];int n=css_expand_vars(rules,(int)strlen(rules),expanded,sizeof expanded);
    check(n>0,"supports: shared normalization reaches variable collection");
    if(n>0){css_apply(root,expanded,n);css_extra_apply(root,expanded,n);}
    width_is("c",61,"supports: false condition excludes native width");ratio_is("c",3,2,0,"supports: false condition excludes extension ratio");
    width_is("d",64,"supports: false condition excludes custom property");
    page("#c{@supports(aspect-ratio:2/1){width:65px;aspect-ratio:2/1}&{@supports(background-size:cover){height:66px}}}","<div id=c></div>");
    width_is("c",65,"supports: ratio condition inside nesting uses shared value parser");
    check(style("c")->has_h&&style("c")->height==66,"supports: background condition inside nested style uses shared value parser");
    ratio_is("c",2,1,0,"supports: accepted nested condition reaches ratio producer");
    char canon[128];int length=0;
    check(css_specified_canon("aspect-ratio",-1,"2/1",-1,canon,sizeof canon,&length)==CSS_SPEC_OK,
          "supports: CSSOM specified ratio uses producer grammar");
    check(css_specified_canon("background-size",-1,"made-up",-1,canon,sizeof canon,&length)==CSS_SPEC_INVALID,
          "supports: CSSOM refuses invalid geometry before canonicalization");
    int found=0;
    for(int i=0;i<css_known_prop_count();i++)if(!strcmp(css_known_prop_at(i,NULL),"aspect-ratio"))found++;
    check(found==1,"supports: producer name enters property universe once");
    page("#c{aspect-ratio:auto 0/0}","<div id=c></div>");
    length=css_computed_text(find(root,"c"),css_prop_lookup("aspectRatio",-1),canon,sizeof canon);
    check(length>0&&!strcmp(canon,"auto 0 / 0"),"supports: computed degenerate ratio retains explicit syntax");
}
static void resources(void)
{
    page(".p{width:1px;&suffix{width:2px}@container(width>1px){width:3px}}", "<div id=c class=p></div>");
    width_is("c",1,"resource: unsupported child syntax preserves valid parent declarations");
    const char *input=".p{width:1px;>.c{width:2px}}@supports(width:1px){.p{height:3px}}";
    char *owned=NULL;int length=0;
    allocation_count=0;
    int result=css_normalize_stylesheet(input,(int)strlen(input),&owned,&length);
    int total=allocation_count;free(owned);
    check(result>=0&&total>=4,"resource: actual bridge allocates parent child condition and output");
    int refused=1;
    for(int i=0;i<total;i++) {
        allocation_count=0;allocation_fault=i;owned=NULL;
        result=css_normalize_stylesheet(input,(int)strlen(input),&owned,&length);
        allocation_fault=-1;
        if(result!=-1||owned)refused=0;
        free(owned);
    }
    check(refused,"resource: every normalization allocation failure refuses whole sheet");
    char deep[1200];int at=0;
    for(int i=0;i<40;i++)at+=snprintf(deep+at,sizeof deep-(unsigned)at,".p{");
    at+=snprintf(deep+at,sizeof deep-(unsigned)at,"width:1px;");
    for(int i=0;i<40;i++)deep[at++]='}';deep[at]=0;
    owned=NULL;result=css_normalize_stylesheet(deep,at,&owned,&length);
    check(result==-1&&!owned,"resource: nesting depth ceiling refuses whole sheet");free(owned);
    char html[1500],selector[500];int h=0,q=0;
    for(int i=0;i<33;i++)h+=snprintf(html+h,sizeof html-(unsigned)h,"<div class=n>");
    h+=snprintf(html+h,sizeof html-(unsigned)h,"<span id=target class=n></span>");
    for(int i=0;i<33;i++)h+=snprintf(html+h,sizeof html-(unsigned)h,"</div>");
    page("",html);
    q+=snprintf(selector+q,sizeof selector-(unsigned)q,":not(:is(.missing");
    for(int i=0;i<17;i++)q+=snprintf(selector+q,sizeof selector-(unsigned)q," .n");
    q+=snprintf(selector+q,sizeof selector-(unsigned)q,")){z-index:0}");
    unsigned char accepted=0,matched=0;
    void *matcher=css_extra_matcher_create(selector,q,1,&accepted);
    int selected=matcher?css_extra_matcher_match(matcher,find(root,"target"),&matched):1;
    check(matcher&&accepted&&!selected&&css_extra_matcher_error(matcher)==CSS_NOMEM&&!matched,
          "resource: shared alternative work ceiling is NOMEM not negation success");
    css_extra_matcher_destroy(matcher);
}
static void boundaries(void)
{
    page("#c::before{content:'x';width:81px;@media(min-width:1px){height:82px}}", "<div id=c></div>");
    struct node *before=style("c")->generated[0];
    struct cstyle *pseudo=before?before->style:NULL;
    check(pseudo&&pseudo->has_w&&pseudo->width==81&&pseudo->has_h&&pseudo->height==82,
          "boundary: conditional bare run preserves pseudo-element target");
    page(".p{.\\26 {width:83px}}", "<div class=p><span id=c class='&'></span></div>");
    width_is("c",83,"boundary: escaped ampersand identifier is not a nesting selector");
    page("#c{width:84px;BACKGROUND-IMAGE:url('data:image/png;base64,abc;def')}", "<div id=c></div>");
    const char *data_url="url('data:image/png;base64,abc;def')";
    check(style("c")->xraw[XR_BG_IMAGE]&&style("c")->xrawlen[XR_BG_IMAGE]==(int)strlen(data_url)&&
          !memcmp(style("c")->xraw[XR_BG_IMAGE],data_url,strlen(data_url)),
          "boundary: uppercase declaration and URL semicolon remain one value");
    page("#c{background-image:url(a.png);background:red}","<div id=c></div>");
    struct bg_layers reset;
    /* Shorthand provenance now retains the whole stable span, rather than
     * copying the literal `none` into this one raw slot. Check its used image
     * reset instead: red must never revive the earlier URL layer. */
    check(css_background_layers(style("c"),&reset)&&reset.count==1&&reset.layer[0].kind==BG_NONE,
          "boundary: color shorthand resets prior URL extension");
    page("#c{font-size:20px;background-size:2em 3em}#d{font-size:10px;background-size:inherit}","<div id=c><div id=d></div></div>");
    check(style("d")->background_font[0]==20,"boundary: inherited background geometry keeps parent computed em basis");
    /* Each normalizer output is freed before these raw spans are read. Mutating
     * and restyling one caller buffer must refresh both private sheet copies. */
    char rules[256]=".p{.c{width:85px;background-image:url(first.png)}}";
    page(rules,"<div class=p><span id=c class=c></span></div>");
    struct cstyle *s=style("c");
    check(s->xraw[XR_BG_IMAGE]&&s->xrawlen[XR_BG_IMAGE]==14&&!memcmp(s->xraw[XR_BG_IMAGE],"url(first.png)",14),
          "boundary: extension spans survive freed normalization buffer");
    strcpy(rules,".p{.c{width:86px;background-image:url(next.png)}}");
    css_apply(root,rules,(int)strlen(rules));css_extra_apply(root,rules,(int)strlen(rules));
    width_is("c",86,"boundary: mutable same pointer reparses changed normalized bytes");
    s=style("c");
    check(s->xraw[XR_BG_IMAGE]&&s->xrawlen[XR_BG_IMAGE]==13&&!memcmp(s->xraw[XR_BG_IMAGE],"url(next.png)",13),
          "boundary: extension cache replaces changed normalized bytes");
}
int main(int argc,char **argv)
{
    css_viewport(800,600);
    const char *group=argc>1?argv[1]:"all";
    if(!strcmp(group,"all")||!strcmp(group,"nesting"))nesting();
    if(!strcmp(group,"all")||!strcmp(group,"selector"))selectors();
    if(!strcmp(group,"all")||!strcmp(group,"ratio"))sizing();
    if(!strcmp(group,"all")||!strcmp(group,"supports"))supports();
    if(!strcmp(group,"all")||!strcmp(group,"resource"))resources();
    if(!strcmp(group,"all")||!strcmp(group,"boundary"))boundaries();
    if(root)dom_free(root);
    printf("css-nesting: %d checks, %d failures\n",checks,failures);
    return failures?1:checks?0:2;
}
