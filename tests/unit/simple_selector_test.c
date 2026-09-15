/* Uses browser_load.py's exact 600-row/10-query/60-fragment specimen, with
 * only its clocks replaced by zero. Native operation counts are evidence on
 * the host; elapsed performance belongs to the unmodified guest driver. */
#define main dom_iface_fixture_main
#include "dom_iface_test.c"
#undef main

extern unsigned long js_dom_profile_attr_reads, js_dom_profile_wrap_calls;
extern unsigned long js_dom_profile_simple_queries, js_dom_profile_simple_candidates;

static void native_check(int ok, const char *label)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s\n", label); }
    else printf("ok: %s\n", label);
}
static struct node *script_node(struct node *root)
{
    if (root->type == N_ELEM && !strcmp(root->tag, "script")) return root;
    for (struct node *n = root->first_child; n; n = n->next) {
        struct node *s = script_node(n); if (s) return s;
    }
    return NULL;
}
static int eval_span(const char *source, int length)
{
    /* A length does not make a prefix into a valid QuickJS buffer: the byte
     * after it must also be NUL. Keep this true for each measured phase. */
    char *copy = malloc((size_t)length + 1); if (!copy) return 0;
    memcpy(copy, source, (size_t)length); copy[length] = 0;
    int ok = js_page_eval(copy, length, "<selector-workload>", NULL);
    free(copy); return ok;
}
static int candidate_workload(const char *path)
{
    FILE *fp = fopen(path, "rb"); if (!fp) return 0;
    fseek(fp, 0, SEEK_END); long size = ftell(fp); rewind(fp);
    char *html = malloc((size_t)size + 1); if (!html) { fclose(fp); return 0; }
    if (fread(html, 1, (size_t)size, fp) != (size_t)size) { fclose(fp); free(html); return 0; }
    fclose(fp); html[size] = 0;
    struct node *root = dom_parse(html, (int)size);
    if (!root || !js_page_open(root)) return 0;
    g_ctx = js_page_ctx(); js_page_eval("void 0", 6, "<warmup>", NULL);
    struct node *script = script_node(root), *source = script ? script->first_child : NULL;
    if (!source || !source->text) return 0;
    char *start = strstr(source->text, "var candidateStart=0;"),
         *end = strstr(source->text, "var candidateEnd=0;");
    if (!start || !end || end <= start) return 0;
    if (!eval_span(source->text, (int)(start-source->text))) return 0;
    js_dom_profile_attr_reads = js_dom_profile_wrap_calls = 0;
    js_dom_profile_simple_queries = js_dom_profile_simple_candidates = 0;
    if (!eval_span(start, (int)(end-start))) return 0;
    printf("candidate-selector work: queries=%lu native-candidates=%lu JS-attribute-reads=%lu wraps=%lu\n",
           js_dom_profile_simple_queries, js_dom_profile_simple_candidates,
           js_dom_profile_attr_reads, js_dom_profile_wrap_calls);
    native_check(js_dom_profile_simple_queries == 30, "compound workload enters thirty native candidate walks");
    native_check(js_dom_profile_simple_candidates == 18000, "candidate walk still examines all 600 real rows per query");
    native_check(js_dom_profile_attr_reads == 360, "candidate filter avoids JS attribute reads on impossible matches");
    native_check(js_dom_profile_wrap_calls < 1500, "candidate filter does not wrap every rejected row");
    ckjs("candidateCount===150", "compound workload retains all five matches in all thirty queries");
    if (!eval_span(end, source->textlen-(int)(end-source->text))) return 0;
    ckjs("candidateStatic.length===5&&candidateRoot.querySelectorAll('.row[data-pick=\"yes\"]').length===6",
         "candidate results are static and the next query observes mutation");
    js_page_close(); dom_free(root); free(html); return 1;
}
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *fp = fopen(argv[1], "rb"); if (!fp) return 2;
    fseek(fp, 0, SEEK_END); long size = ftell(fp); rewind(fp);
    char *html = malloc((size_t)size + 1); if (!html) return 2;
    if (fread(html, 1, (size_t)size, fp) != (size_t)size) return 2;
    fclose(fp); html[size] = 0;
    struct node *root = dom_parse(html, (int)size);
    if (!root || !js_page_open(root)) return 2;
    g_ctx = js_page_ctx(); js_page_eval("void 0", 6, "<warmup>", NULL);
    struct node *script = script_node(root), *text = script ? script->first_child : NULL;
    if (!text || !text->text) return 2;
    char *boundary = strstr(text->text, "var q=0;"); if (!boundary) return 2;
    /* Split only at the existing guest measurement boundary. No pending jobs
     * are scheduled by the first phase, and both halves use the same global
     * environment/currentScript. This isolates query counts from mutations. */
    js_dom_profile_attr_reads = js_dom_profile_wrap_calls = 0;
    js_dom_profile_simple_queries = js_dom_profile_simple_candidates = 0;
    /* QuickJS requires a terminating NUL even when an explicit length is
     * supplied. A prefix view of the original script is not a valid input. */
    int prefix_len = (int)(boundary - text->text);
    char *prefix = malloc((size_t)prefix_len + 1);
    if (!prefix) return 2;
    memcpy(prefix, text->text, (size_t)prefix_len); prefix[prefix_len] = 0;
    if (!js_page_eval(prefix, prefix_len, "<guest-query-workload>", script)) return 2;
    free(prefix);
    printf("simple-selector work: queries=%lu native-candidates=%lu JS-attribute-reads=%lu wraps=%lu\n",
           js_dom_profile_simple_queries, js_dom_profile_simple_candidates,
           js_dom_profile_attr_reads, js_dom_profile_wrap_calls);
    native_check(js_dom_profile_simple_queries == 10, "same guest workload uses ten native simple queries");
    native_check(js_dom_profile_simple_candidates >= 6000 && js_dom_profile_simple_candidates < 6200,
                 "native query still examines the real document candidates");
    native_check(js_dom_profile_attr_reads == 0, "simple query avoids per-candidate JS attribute reads");
    ckjs("count===6000", "same guest workload returns all 600 matches on every query");
    if (!js_page_eval(boundary, text->textlen - (int)(boundary - text->text), "<guest-fragment-workload>", script)) return 2;
    ckjs("document.getElementById('dynamic59')!==null", "same guest workload completes sixty fragment commits");

    ckjs("document.querySelector('html')===document.documentElement&&document.documentElement.querySelector('html')===null",
         "document includes documentElement and element scope excludes itself");
    ckjs("var scope=document.createElement('div');document.body.appendChild(scope);"
         "var a=document.createElement('div'),b=document.createElement('div');a.id=b.id='duplicate';"
         "scope.appendChild(a);scope.appendChild(b);var ds=scope.querySelectorAll('#duplicate');"
         "ds.length===2&&ds[0]===a&&ds[1]===b&&scope.querySelector('#duplicate')===a&&a.querySelector('#duplicate')===null",
         "duplicate IDs preserve tree order and strict descendant scope");
    ckjs("scope.querySelector('DIV')===a&&scope.querySelector('d\\\\69 v')===a",
         "HTML tags fold ASCII case after CSS escape decoding");
    ckjs("var longName='x-'+Array(100).join('a')+'é';var longEl=document.createElement(longName);"
         "scope.appendChild(longEl);scope.querySelector(longName.slice(0,-1)+'\\\\e9')===longEl",
         "escaped long tag names retain their complete UTF-8 length");
    ckjs("scope.querySelector('#DUPLICATE')===null", "standards mode IDs remain case-sensitive");
    ckjs("a.setAttribute('class','one\\tneedle\\ntwo\\fthree\\rfour five');"
         "scope.querySelector('.needle')===a&&scope.querySelector('.need')===null",
         "class tokens use all five ASCII separators and exact token boundaries");
    ckjs("a.setAttribute('class','needle\\u0000tail');a.setAttribute('id','duplicate\\u0000tail');"
         "scope.querySelector('.needle')===null&&scope.querySelectorAll('#duplicate').length===1",
         "embedded NUL attribute bytes never become a truncated match");
    ckjs("a.setAttribute('class','café 🙂');"
         "scope.querySelector('.caf\\\\e9')===a&&scope.querySelector('.\\\\1f642')===a",
         "decoded non-ASCII and astral CSS identifiers match their full UTF-8 bytes");
    ckjs("var svg=document.createElementNS('http://www.w3.org/2000/svg','svg');"
         "var clip=document.createElementNS('http://www.w3.org/2000/svg','clipPath');"
         "svg.appendChild(clip);scope.appendChild(svg);"
         "scope.querySelector('clipPath')===clip&&scope.querySelector('clippath')===null",
         "foreign element names remain case-sensitive");
    ckjs("var df=document.createDocumentFragment(),de=document.createElement('em');df.appendChild(de);"
         "df.querySelector('em')===de&&df.querySelectorAll('*').length===1",
         "detached fragment queries include descendants and exclude the fragment");
    js_dom_profile_simple_candidates = 0;
    ckjs("df.appendChild(document.createElement('span'));df.querySelector('em')===de",
         "first-match query returns the first matching descendant");
    native_check(js_dom_profile_simple_candidates == 1,
                 "first-match native query stops before later siblings");
    ckjs("var sh=document.createElement('div');scope.appendChild(sh);var sr=sh.attachShadow({mode:'open'});"
         "var hidden=document.createElement('b');hidden.setAttribute('class','shadow-only');sr.appendChild(hidden);"
         "document.querySelector('.shadow-only')===null&&sr.querySelector('.shadow-only')===hidden",
         "native queries do not cross the shadow boundary");
    js_dom_profile_simple_queries = 0;
    ckjs("scope.querySelectorAll('svg > clipPath')[0]===clip&&scope.querySelectorAll('div[id]')[0]===a&&"
         "scope.querySelectorAll('em, svg').length===1&&scope.querySelector('*|svg')===svg",
         "compound, combinator and list selectors preserve the complete matcher");
    /* Previously this asserted zero native calls for every complex AST.
     * They still require the full matcher, but two queries now prefilter by
     * their rightmost literal. Lists/qualified types retain the old walk.
     * Correction 2026-09-11: the comma list now adds one native union walk;
     * the namespace-qualified type still uses the complete JS walk. */
    native_check(js_dom_profile_simple_queries == 3, "complex queries filter candidates without replacing the full matcher");
    ckjs("var cs=document.createElement('div');cs.className='owner';scope.appendChild(cs);"
         "cs.innerHTML='<span class=hot data-pick=no></span><span class=hot data-pick=yes></span><span class=cold></span>';"
         "var ca=cs.firstChild,cb=ca.nextSibling,cc=cb.nextSibling;"
         "cs.querySelectorAll('.hot[data-pick=yes]')[0]===cb&&cs.querySelectorAll(':scope > .hot[data-pick]').length===2&&"
         "cs.querySelectorAll('.owner > .hot[data-pick=yes]')[0]===cb",
         "first candidate can fail and scope/ancestor combinators remain authoritative");
    ckjs("var cl=cs.querySelectorAll('.cold, .hot[data-pick=yes]');cl.length===2&&cl[0]===cb&&cl[1]===cc&&"
         "cs.querySelectorAll(':not(.hot)')[0]===cc&&cs.querySelectorAll(':is(.hot,.cold)').length===3&&"
         "scope.querySelectorAll('div:has(> .cold)')[0]===cs",
         "lists retain tree order and pseudo arguments are never required result literals");
    ckjs("ca.setAttribute('data-pick','a b');ca.setAttribute('lang','EN-us');"
         "cs.querySelectorAll('[data-pick~=b]')[0]===ca&&cs.querySelectorAll('[lang|=en i]')[0]===ca&&"
         "cs.querySelectorAll('[lang|=en s]').length===0&&cs.querySelectorAll('[data-pick^=a]')[0]===ca&&"
         "cs.querySelectorAll('[data-pick$=b]')[0]===ca&&cs.querySelectorAll('[data-pick*=\" \" ]')[0]===ca",
         "attribute candidate presence does not replace operators or value case flags");
    /* setAttribute currently folds foreign names too, an older writer defect
     * also observed with candidate filtering disabled. setAttributeNS keeps
     * the requested spelling; use it to isolate this selector's contract. */
    ckjs("clip.setAttributeNS(null,'viewBox','0 0 1 1');a.setAttribute('data-case','ok');"
         "scope.querySelectorAll('[DATA-CASE=ok]')[0]===a&&scope.querySelectorAll('[viewBox]')[0]===clip&&"
         "scope.querySelectorAll('[viewbox]').length===0&&scope.querySelectorAll('[data-ca\\\\73 e=ok]')[0]===a",
         "attribute candidates retain HTML folding, foreign case and CSS escape decoding");
    ckjs("hidden.setAttribute('data-pick','yes');document.querySelectorAll('.shadow-only[data-pick]').length===0",
         "complex candidate queries preserve the document shadow boundary");
    ckjs("sr.querySelectorAll('.shadow-only[data-pick]')[0]===hidden",
         "complex candidate queries find descendants inside a shadow scope");
    ckjs("df.querySelectorAll('em:not(.missing)')[0]===de",
         "complex candidate queries preserve detached fragment boundaries");
    ckjs("var resources=document.createElement('div');scope.appendChild(resources);"
         "for(var ri=0;ri<200;ri++){var im=document.createElement('img');im.setAttribute('src','i'+ri);resources.appendChild(im);}"
         "for(var rs=0;rs<3;rs++){var sc=document.createElement('script');sc.setAttribute('src','s'+rs);resources.appendChild(sc);}true",
         "resource-selector fixture has many wrong-tag src attributes");
    ckjs("resources.appendChild(document.createElement('script'));resources.appendChild(document.createElement('script'));true",
         "resource-selector fixture includes inline scripts");
    js_dom_profile_wrap_calls = 0;
    ckjs("var scripts=resources.querySelectorAll('script[src]');scripts.length===3&&scripts[0].tagName==='SCRIPT'",
         "compound native candidates retain exact script src results");
    native_check(js_dom_profile_wrap_calls < 20,
                 "compound native conjunction rejects wrong-tag src nodes before wrapping");
    js_dom_profile_wrap_calls = 0;
    ckjs("var inlineScripts=resources.querySelectorAll('script:not([src])');inlineScripts.length===2&&"
         "inlineScripts[0].tagName==='SCRIPT'&&!inlineScripts[0].hasAttribute('src')",
         "negated presence compound retains exact inline script results");
    native_check(js_dom_profile_wrap_calls < 10,
                 "negated native conjunction rejects src scripts and wrong tags before wrapping");
    js_dom_profile_wrap_calls = js_dom_profile_simple_queries = 0;
    ckjs("cs.querySelector('.hot[data-pick]')===ca", "first-match compound query retains the first matching node");
    native_check(js_dom_profile_simple_queries <= 1 && js_dom_profile_wrap_calls < 10,
                 "first-match query does not eagerly materialize every native candidate");
    ckjs("var rejected=0;['#','div >','div,','svg|path'].forEach(function(s){try{document.querySelectorAll(s)}"
         "catch(e){if(e.name==='SyntaxError')rejected++}});rejected===4",
         "malformed selectors still throw SyntaxError before fast dispatch");
    ckjs("scope.querySelectorAll('div') instanceof NodeList", "fast query results retain NodeList interface");
    js_page_close(); dom_free(root); free(html);
    const char *quirks = "<html><body><div id=MiXeD class=Foo></div></body></html>";
    root = dom_parse(quirks, (int)strlen(quirks)); if (!root || !js_page_open(root)) return 2;
    g_ctx = js_page_ctx(); js_page_eval("void 0", 6, "<warmup>", NULL);
    ckjs("document.compatMode==='BackCompat'&&document.querySelector('.foo')===document.querySelector('#mixed')&&"
         "document.querySelector('#mixed')===document.getElementById('MiXeD')",
         "quirks mode retains ASCII-insensitive class and ID matching");
    ckjs("document.querySelectorAll('div.foo#mixed')[0]===document.getElementById('MiXeD')",
         "compound candidate IDs retain quirks folding");
    js_page_close(); dom_free(root);
    if (!candidate_workload(argv[2])) return 2;
    printf("simple-selector: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
