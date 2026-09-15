/* Reuse the real passive-context link fixture, not the WebAPI fixture that
 * deliberately omits css_extra context ownership. No JS dirty flag can hide
 * a missing preference invalidation in these document caches. */
#define main passive_context_original_main
#include "passive_layout_context_test.c"
#undef main
int main(void)
{
    const char *html="<!doctype html><body><div id=box>motion</div>";
    const char *sheet="#box{color:#ff0000}@media(prefers-reduced-motion:reduce){#box{color:#0000ff}}";
    css_init();css_set_reduced_motion(1);
    struct css_context *child=css_context_create();
    CHECK(child!=0,"isolated motion document allocated");if(!child)return 1;
    struct css_context *previous=css_context_activate(child);css_init();
    struct node *root=dom_parse(html,(int)strlen(html)),*n=id(root,"box");
    css_apply(root,sheet,(int)strlen(sheet));
    css_set_reduced_motion(0);css_ensure_styled(n);
    CHECK(n&&n->style&&(((struct cstyle*)n->style)->color&0xffffff)==0xff0000,
          "motion change invalidates an already cached stylesheet");
    /* Establish the same red cache in the broken build too, so the inactive
     * check is independent of the earlier active-document failure. */
    css_apply(root,sheet,(int)strlen(sheet));
    css_context_activate(previous);css_set_reduced_motion(1);
    css_context_activate(child);css_ensure_styled(n);
    CHECK(n&&n->style&&(((struct cstyle*)n->style)->color&0xffffff)==0x0000ff,
          "motion change reaches a previously inactive document cache");
    int flushes=css_style_flushes();css_set_reduced_motion(1);css_ensure_styled(n);
    CHECK(css_style_flushes()==flushes,"unchanged preference does not recascade the document");
    css_context_activate(previous);css_context_destroy(child);dom_free(root);
    printf("motion cache: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
