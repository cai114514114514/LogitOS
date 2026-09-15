/* The live GitHub observer sends a >100-alternative comma list twice, and
 * lazy component registration repeatedly asks for absent compound selectors.
 * This smaller deterministic document measures those same two work shapes. */
#define main dom_iface_existing_main
#include "dom_iface_test.c"
#undef main
extern unsigned long js_dom_profile_attr_reads, js_dom_profile_wrap_calls;
extern unsigned long js_dom_profile_simple_queries, js_dom_profile_simple_candidates;
static void work_check(int ok, const char *label)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s\n", label); }
    else printf("ok: %s\n", label);
}
int main(void)
{
    const char *html="<!doctype html><html><body><main id='scope'></main></body></html>";
    struct node *root=dom_parse(html,(int)strlen(html));
    if(!root||!js_page_open(root))return 2;
    g_ctx=js_page_ctx();
    ckjs("var scope=document.getElementById('scope'), nodes=[],parts=[];"
         "for(var i=0;i<600;i++){var e=document.createElement('section');e.className='row';"
         "scope.appendChild(e);nodes.push(e);}"
         "nodes[2].className='row hit';nodes[4].setAttribute('data-pick','yes');"
         "nodes[8].className='row hit';nodes[8].setAttribute('data-pick','yes');"
         "for(var i=0;i<110;i++)parts.push('.absent'+i);"
         "parts.push('.hit.row','[data-pick=yes]','.hit');var selector=parts.join(',');true",
         "observer workload contains 600 real nodes and 113 alternatives");
    js_dom_profile_attr_reads=js_dom_profile_wrap_calls=0;
    js_dom_profile_simple_queries=js_dom_profile_simple_candidates=0;
    ckjs("var result=scope.querySelectorAll(selector);result.length===3&&"
         "result[0]===nodes[2]&&result[1]===nodes[4]&&result[2]===nodes[8]",
         "union results preserve document order and remove duplicate matches");
    printf("selector-batch union: native-queries=%lu candidates=%lu attr-reads=%lu wraps=%lu\n",
           js_dom_profile_simple_queries, js_dom_profile_simple_candidates,
           js_dom_profile_attr_reads, js_dom_profile_wrap_calls);
    work_check(js_dom_profile_attr_reads<1000 && js_dom_profile_wrap_calls<100,
               "selector union rejects impossible nodes before JS matching");
    /* The necessary-literal heuristic picks the first class of equal rank.
     * A broad first class may legitimately admit all nodes; that is a correct
     * fallback cost, not evidence of an incorrect union or selective filter. */
    ckjs("scope.querySelectorAll('.row.hit, [data-pick=yes]').length===3",
         "a broad necessary literal still returns exact compound results");
    js_dom_profile_attr_reads=js_dom_profile_wrap_calls=0;
    ckjs("var missing=0;for(var i=0;i<40;i++)"
         "if(scope.querySelector('lazy-part[part-name=missing'+i+']')===null)missing++;missing===40",
         "forty absent compound queries return null");
    work_check(js_dom_profile_attr_reads==0 && js_dom_profile_wrap_calls<100,
               "absent first-match queries avoid full JS traversal");
    js_dom_profile_simple_candidates=js_dom_profile_wrap_calls=0;
    ckjs("scope.querySelector('section.row')===nodes[0]",
         "broad first-match selector returns its earliest node");
    work_check(js_dom_profile_simple_candidates<=1 && js_dom_profile_wrap_calls<10,
               "broad first-match selector stops without collecting the tail");
    ckjs("nodes[0].setAttribute('data-pick','no');nodes[1].setAttribute('data-pick','yes');"
         "scope.querySelector('[data-pick=yes]')===nodes[1]",
         "first candidate may fail before the next candidate matches");
    ckjs("nodes[0].className='hit';scope.querySelectorAll(selector)[0]===nodes[0]&&result.length===3",
         "mutation changes the next query while old NodeList remains static");
    ckjs("scope.querySelector('.absent, [data-pick=yes]')===nodes[1]",
         "first-match union preserves order across alternatives");
    ckjs("scope.querySelectorAll('.absent, :not(.missing)').length===600",
         "an alternative without a positive literal retains complete matching");
    ckjs("scope.querySelector('main .row')===nodes[1]&&scope.querySelector('main > .row')===nodes[1]",
         "ancestor combinators remain the full matcher's responsibility");
    ckjs("var caught=false;try{scope.querySelectorAll('.hit,')}catch(e){caught=e.name==='SyntaxError'}caught",
         "invalid list throws before native admission");
    ckjs("var many=parts.slice();for(var i=0;i<300;i++)many.push('.never'+i);"
         "scope.querySelectorAll(many.join(',')).length===5",
         "oversize filter list falls back without losing results");
    js_page_close();dom_free(root);
    printf("selector-batch: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
