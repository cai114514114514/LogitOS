/* Actual ordinary cascade + extension producer, with constant values so this
 * fixture does not need variable substitution. Sites concatenate thousands of
 * media groups; the extension tier must use the same conditions as LibCSS. */
#define main cascade_existing_main
#include "css_extra_cascade_test.c"
#undef main

static void media_case(const char *sheet,const char *want,const char *label)
{
    const char *html="<!doctype html><div id=n>probe</div>";
    g_root=dom_parse(html,(int)strlen(html));
    css_viewport(1200,600);
    css_apply(g_root,sheet,(int)strlen(sheet));
    css_extra_apply(g_root,sheet,(int)strlen(sheet));
    struct cstyle *st=ID("n")->style;
    CHECK(st && st->xraw[XR_TRANSFORM] && st->xrawlen[XR_TRANSFORM]==strlen(want) &&
          !memcmp(st->xraw[XR_TRANSFORM],want,strlen(want)),label);
    dom_free(g_root);g_root=NULL;
}
int main(void)
{
    media_case("@media(max-width:10px){@media(min-width:1px){#n{transform:translateX(2px)}}}#n{transform:translateX(7px)}",
               "translateX(7px)","inactive outer media ends at its own closing brace");
    const char *prefix="@media(min-width:1px){.noise{gap:1px}}";
    size_t cap=strlen(prefix)*700+256;
    char *sheet=malloc(cap);if(!sheet)return 2;
    int p=0;
    for(int i=0;i<700;i++)p+=snprintf(sheet+p,cap-p,"%s",prefix);
    snprintf(sheet+p,cap-p,"#n{transform:translateX(3px)}@media(max-width:100px){#n{transform:translateX(9px)}}");
    media_case(sheet,"translateX(3px)","media after 512 earlier groups does not become unconditional");
    free(sheet);
    prefix="@media(min-width:2000px){.noise{gap:1px}}";
    cap=strlen(prefix)*700+256;sheet=malloc(cap);if(!sheet)return 2;p=0;
    for(int i=0;i<700;i++)p+=snprintf(sheet+p,cap-p,"%s",prefix);
    snprintf(sheet+p,cap-p,"#n{transform:translateX(3px)}@media(max-width:100px){#n{transform:translateX(9px)}}");
    media_case(sheet,"translateX(3px)","more than 512 inactive spans stay gated");free(sheet);
    char deep[4096];p=snprintf(deep,sizeof deep,"#n{transform:translateX(3px)}");
    for(int i=0;i<70;i++)p+=snprintf(deep+p,sizeof deep-p,"@media(min-width:1px){");
    p+=snprintf(deep+p,sizeof deep-p,"@media(max-width:10px){#n{transform:translateX(9px)}}");
    for(int i=0;i<70;i++)deep[p++]='}';deep[p]=0;
    media_case(deep,"translateX(3px)","active nesting beyond 64 retains inner media condition");
    media_case("/* @media(max-width:1px){ */ #n{transform:translateX(5px)}",
               "translateX(5px)","commented media prelude cannot suppress later declarations");
    media_case("#n{font-family:'} @media(max-width:1px){';transform:translateX(6px)}",
               "translateX(6px)","quoted braces cannot change extension rule boundaries");
    printf("css-media-regions: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
