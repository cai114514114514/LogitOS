/* The real compiler/module map decides whether a speculative fetch is useful.
 * Count transport calls rather than timing this host or inventing a second
 * loaded-module registry in the fixture. Cycles and normalized aliases matter:
 * QuickJS will never consume prefetched bytes for a module it already owns. */
#define main module_budget_original_main
#define bfetch_prefetch budget_prefetch
#define res_fetch budget_fetch
#include "module_budget_test.c"
#undef main
#undef bfetch_prefetch
#undef res_fetch

static int shared_prefetch, root_prefetch, branch_prefetch;
void bfetch_prefetch(const char *url)
{
    if (strstr(url, "/shared.js")) shared_prefetch++;
    if (strstr(url, "/root.js")) root_prefetch++;
    if (strstr(url, "/branch")) branch_prefetch++;
}
int res_fetch(const char *url, unsigned char **out, int *len)
{
    const char *src = strstr(url, "/shared.js") ? "export const v=7;" :
        "import {v} from 'https://module.test/shared.js';import './root.js';"
        "globalThis.sum=(globalThis.sum||0)+v;";
    *len=(int)strlen(src);*out=malloc((size_t)*len+1);
    if(!*out)return -1;memcpy(*out,src,(size_t)*len+1);return 0;
}
int main(void)
{
    for(int round=0;round<2;round++) {
        struct node *root=dom_parse("<html><body></body></html>",26);
        js_page_set_clock(clock_ms);js_page_set_slice_ms(1000);
        if(!root||!js_page_open(root))return 2;
        js_module_reset();shared_prefetch=root_prefetch=branch_prefetch=0;
        char src[4096];int n=snprintf(src,sizeof src,"import './shared.js';");
        for(int i=0;i<48;i++)n+=snprintf(src+n,sizeof src-(size_t)n,"import './branch%d.js';",i);
        ck(js_module_eval(src,n,"https://module.test/root.js"),"shared cyclic graph evaluates");
        ck(value("sum===336"),"all 48 branch bodies execute exactly once");
        ck(branch_prefetch==48,"every uncompiled branch is offered to transport");
        ck(shared_prefetch==1,"compiled shared dependency is not prefetched again");
        ck(root_prefetch==0,"back edge never prefetches the compiling root");
        ck(js_module_eval("import './shared.js';",21,"https://module.test/late.js"),"later root reuses existing module");
        ck(shared_prefetch==1,"later module keeps the same authoritative module identity");
        js_page_close();dom_free(root);
    }
    printf("module-prefetch-loaded: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
