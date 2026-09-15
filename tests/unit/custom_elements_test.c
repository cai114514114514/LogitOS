/* Runs the production prelude and native DOM. A detached controller is consumed
 * before insertion, the same operation that stopped the real page bootstrap;
 * checking only instanceof after appendChild misses that failure completely. */
#define main platform_existing_main
#include "webapi_platform_test.c"
#undef main
int main(void)
{
    struct node *root = dom_parse(PAGE, (int)strlen(PAGE));
    if (!root) return 1;
    js_page_set_clock(clock_fn);
    js_page_set_location("https://custom-elements.example/");
    if (!js_page_open(root)) { dom_free(root); return 1; }
    ctx = js_page_ctx();
    run("var made=0, connected=0, disconnected=0;"
        "class Delegate { requestErrored() { return 'handled'; } }"
        "class Component extends HTMLElement {"
        " constructor(){ super(); ++made; this.delegate=new Delegate(); }"
        " connectedCallback(){++connected;}"
        " disconnectedCallback(){++disconnected;}"
        "} customElements.define('x-controller',Component);");
    ckjs("(function(){var el=document.createElement('x-controller');"
         "return Object.getPrototypeOf(el.delegate).requestErrored.call(el.delegate)==='handled' && el instanceof Component && !el.parentNode;})()",
         "detached createElement controller is usable before insertion");
    run("var host=document.getElementById('wrap'), el=new Component(); var beforeMade=made; host.appendChild(el);");
    ckjs("connected===1 && made===beforeMade", "direct new connects once without reconstructing");
    run("host.removeChild(el); host.appendChild(el);");
    ckjs("connected===2 && disconnected===1", "remove and reinsert reconnects");
    run("customElements.upgrade(host); customElements.upgrade(host);");
    ckjs("connected===2 && made===beforeMade", "explicit repeated upgrade is not a connection");
    run("var box=document.createElement('div'), d=new Component(); box.appendChild(d); box.removeChild(d);");
    ckjs("connected===2 && disconnected===1", "detached removal does not disconnect");
    run("var frag=document.createDocumentFragment(); frag.appendChild(d); host.appendChild(frag);");
    ckjs("connected===3 && d.parentNode===host", "fragment insertion connects transferred children");
    run("var host2=document.createElement('div'); document.body.appendChild(host2); host2.appendChild(d);");
    ckjs("connected===4 && disconnected===2", "moving a connected component disconnects then reconnects");
    run("host2.replaceChild(el,d);");
    ckjs("connected===5 && disconnected===4", "replace disconnects replaced and moved components");
    run("var seen=[]; class Nested extends HTMLElement { constructor(){super();seen.push(this);this.inner=document.createElement('x-controller');} } customElements.define('x-nested',Nested);var nest=document.createElement('x-nested');");
    ckjs("seen.length===1 && seen[0]===nest && nest.inner instanceof Component && nest!==nest.inner", "nested construction preserves each upgrade handoff");
    ckjs("document.createElementNS('http://www.w3.org/1999/xhtml','x-controller') instanceof Component", "HTML namespace creation upgrades synchronously");
    ckjs("!(document.createElementNS('http://www.w3.org/2000/svg','x-controller') instanceof Component)", "SVG name does not use HTML custom element registry");
    ckjs("(function(){var c=connected,d=disconnected,before=el.parentNode;try{host.insertBefore(el,document.createElement('div'));}catch(e){}return connected===c && disconnected===d && el.parentNode===before;})()", "failed native insertion cannot emit lifecycle reactions");
    js_page_close(); ctx = NULL; dom_free(root);
    printf("custom elements: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
