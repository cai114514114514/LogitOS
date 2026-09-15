/* The test counts actual native work, not host wall time. The semantic half
 * prevents a "skip the feature" optimization: names still become globals,
 * only newly connected scripts reach the real sink, and query order/scope
 * survive. Existing selector cases provide the broad matcher coverage. */
#define main dom_iface_fixture_main
#include "dom_iface_test.c"
#undef main

extern unsigned long js_dom_profile_named_visits;
extern unsigned long js_dom_profile_script_visits;
extern unsigned long js_dom_profile_collection_builds;
static struct node *queued[32];
static int queue_count, offers;

static void script_sink(struct node *n)
{
    offers++;
    /* Like browser.c: queue once, mark DONE only when drained. Reoffering an
     * existing sibling before the drain must not be hidden by its DONE bit. */
    for (int i = 0; i < queue_count; i++) if (queued[i] == n) return;
    if (queue_count < 32) queued[queue_count++] = n;
}
static void count_check(int ok, const char *what)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s\n", what); }
    else printf("ok: %s\n", what);
}
int main(void)
{
    struct node *root = dom_parse(HTML, (int)strlen(HTML));
    if (!root) return 2;
    struct node *parent = dom_get_element_by_id(root->doc, "d");
    for (int i = 0; i < 512; i++) dom_append_child(parent, dom_create_element(root->doc, "span", 4));
    if (!js_page_open(root)) return 2;
    g_ctx = js_page_ctx();
    js_page_eval("void 0", 6, "<warmup>", 0);
    js_dom_set_script_sink(script_sink);
    ckjs("var execution=[];var parent=document.getElementById('d');"
         "var anchor=document.createElement('i');parent.appendChild(anchor);"
         "var old=document.createElement('script');old.textContent=\"execution.push('old')\";"
         "parent.appendChild(old);true", "one existing script remains queued before fragment commits");
    count_check(offers == 1 && queue_count == 1, "existing script initially offered once");
    js_dom_profile_named_visits = js_dom_profile_script_visits = 0;
    ckjs("for(var i=0;i<8;i++){var f=document.createDocumentFragment();"
         "var e=document.createElement('section');e.id='batch'+i;f.appendChild(e);"
         "var s=document.createElement('script');s.textContent='execution.push('+i+')';f.appendChild(s);"
         "if(i<4)parent.appendChild(f);else parent.insertBefore(f,anchor);}true",
         "fragment append and insertBefore both connect their complete contents");
    printf("traversal work: eight commits into 512 existing spans: named=%lu scripts=%lu offers=%d\n",
           js_dom_profile_named_visits, js_dom_profile_script_visits, offers);
    count_check(js_dom_profile_named_visits <= 64, "fragment named-access work is bounded by inserted nodes");
    count_check(js_dom_profile_script_visits <= 64, "fragment script-discovery work is bounded by inserted nodes");
    count_check(offers == 9, "fragment insertion never reoffers existing sibling scripts");
    ckjs("batch0===document.getElementById('batch0')&&batch7===document.getElementById('batch7')",
         "new fragment descendants still publish live named globals");
    count_check(queue_count == 9, "all eight newly inserted scripts reach the production sink");
    for (int i = 0; i < queue_count; i++) {
        struct node *n = queued[i], *t = n->first_child;
        dom_script_mark_done(n);
        if (!t || !js_page_eval(t->text, t->textlen, "<inserted-script>", n)) return 2;
    }
    ckjs("execution.join(',')==='old,0,1,2,3,4,5,6,7'", "queued scripts execute once in insertion order");
    js_dom_profile_collection_builds = 0;
    ckjs("var queried=parent.querySelectorAll('section[id]');"
         "Array.from(queried).map(function(n){return n.id}).join(',')==='batch4,batch5,batch6,batch7,batch0,batch1,batch2,batch3'",
         "querySelectorAll preserves document order after fragment insertion");
    printf("traversal work: query over 500+ nodes: child collections=%lu\n", js_dom_profile_collection_builds);
    count_check(js_dom_profile_collection_builds <= 4, "query traversal avoids a child collection per visited element");
    ckjs("parent.querySelector('section')===batch4&&parent.getElementsByTagName('section').length===8",
         "first-match and element-collection query paths preserve results");
    ckjs("var detached=document.createDocumentFragment();var da=document.createElement('a');"
         "var db=document.createElement('b');da.appendChild(db);detached.appendChild(da);"
         "detached.querySelector('b')===db&&detached.querySelectorAll('a > b').length===1",
         "detached fragment queries still descend in tree order");
    ckjs("var shadowHost=document.createElement('div');document.body.appendChild(shadowHost);"
         "var shadow=shadowHost.attachShadow({mode:'open'});var inside=document.createElement('b');"
         "inside.setAttribute('class','shadow-only');shadow.appendChild(inside);"
         "document.querySelectorAll('.shadow-only').length===0&&shadow.querySelector('.shadow-only')===inside",
         "query traversal preserves the existing shadow boundary");
    js_dom_set_script_sink(NULL);
    js_page_close(); dom_free(root);
    printf("traversal-work: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
