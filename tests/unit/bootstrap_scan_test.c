/* Startup must initialize real details/iframe nodes without manufacturing JS
 * wrappers for thousands of unrelated elements. DOM_ST_WRAP is incremented by
 * the production DOM core, not by the new scanner: removing the scanner must
 * therefore turn the cost assertion red while all behavior remains green. */
#define main semantics_existing_main
#include "semantics_test.c"
#undef main

static unsigned long long clock_value=1000;
static unsigned long long bootstrap_clock(void){return clock_value;}

int main(void)
{
    char *html=malloc(200000);int n=0;
    n+=sprintf(html+n,"<!doctype html><html><body>");
    for(int i=0;i<1200;i++)n+=sprintf(html+n,"<section><span>unrelated %d</span></section>",i);
    n+=sprintf(html+n,"<details id=first name=group open><summary>first</summary></details>"
        "<details id=second name=group open><summary>second</summary></details>"
        "<details id=other name=other open></details>"
        "<iframe id=frame srcdoc=\"<p id=inside>frame-ready</p>\"></iframe></body></html>");
    struct node *root=dom_parse(html,n);free(html);
    if(!root)return 1;
    js_page_set_clock(bootstrap_clock);js_page_set_location("http://example.com/");
    dom_stat_reset();
    if(!js_page_open(root)){dom_free(root);return 1;}
    unsigned long long wrapped=dom_stat[DOM_ST_WRAP];
    printf("bootstrap-scan: startup wrappers=%llu unrelated_elements=2400\n",wrapped);
    checks++;
    if(wrapped>=200){printf("FAIL bootstrap-scan: unrelated tree was wrapped during startup (%llu)\n",wrapped);fails++;}
    g_ctx=js_page_ctx();
    ckjs("document.getElementById('first').hasAttribute('open')&&!document.getElementById('second').hasAttribute('open')&&document.getElementById('other').hasAttribute('open')",
        "initial named-details exclusivity preserves first open in document order");
    for(int i=0;i<20;i++){clock_value+=20;js_page_run_due();}
    ckjs("document.getElementById('frame').contentDocument.getElementById('inside').textContent==='frame-ready'",
        "parser iframe srcdoc initialized through actual platform hook");
    ckjs("document.getElementById('second').setAttribute('open','');!document.getElementById('first').hasAttribute('open')&&document.getElementById('second').hasAttribute('open')",
        "dynamic named-details behavior remains live");
    js_page_close();dom_free(root);
    printf("bootstrap-scan: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
