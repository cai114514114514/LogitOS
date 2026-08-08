/* semantics_test.c -- the HTML element interfaces (c/apps/browser/js_semantics.c).
 *
 * WHAT IT MEASURES, and why it is not a duplicate of test-wpt. The corpus
 * measures "does html/semantics pass"; it is minutes long and every one of its
 * failures is reported against its own stated assertion. That is the right
 * gate and the wrong microscope. This is seconds long and asserts the
 * PROPERTIES the corpus cannot isolate:
 *
 *   - a collection is LIVE. WPT has hundreds of tests that a static snapshot
 *     would fail, but each fails for its own reason, so no WPT run can ever
 *     tell you "your collections are snapshots". Insert a row, re-read
 *     `rows.length`, and it can.
 *   - the attr-associated element (`button.popoverTargetElement = el`) is not
 *     an id lookup. The distinction is invisible until the target is out of
 *     the tree, which is precisely the case WPT asserts and an id-based
 *     implementation gets right by accident for every OTHER case.
 *   - `command`'s getter filters and its setter does not, so an invalid value
 *     round-trips through getAttribute and reads back as "".
 *
 * GROUPS. Group 1 is the LIVENESS group -- the assertions that must go red
 * under -DSEMANTICS_STATIC_COLLECTIONS. They are counted separately so that a
 * negative control failing for an unrelated reason is visible as such rather
 * than passing as evidence. Group 2 is everything else, and must pass in BOTH
 * builds; an assertion that failed in both would make the control meaningless.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "dom.h"
#include "css.h"
#include "js_dom.h"
#include "js_page.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

/* The link stubs tests/unit/wpt_test.c and dom_iface_test.c both carry, and
 * for the reason they give: this links the SHIPPING browser files, two of
 * which reach for a fetch and an image registry that do not exist off the
 * machine. Stubbing those is not stubbing the DOM. */
__attribute__((__weak__)) void img_register(void *d) { (void)d; }
__attribute__((__weak__)) void img_register_anim(void *a, void *b, void *c)
{ (void)a; (void)b; (void)c; }
int bfetch_resolve(const char *base, const char *ref, char *out, int max)
{ (void)base; if (!ref || !out || max <= 0) return 0; snprintf(out, (size_t)max, "%s", ref); return 1; }
int bfetch_sync(const char *ref, unsigned char **out, int *outlen)
{ (void)ref; (void)out; (void)outlen; return 0; }
int text_measure(const char *s, int len, int px, int mono)
{ (void)s; (void)mono; return len * (px / 2); }
int res_fetch(const char *u, unsigned char **b, int *l)
{ (void)u; (void)b; (void)l; return -1; }

static int fails, checks, live_fails, live_checks, in_live;
static JSContext *g_ctx;

static void ckjs(const char *expr, const char *what)
{
    checks++;
    if (in_live) live_checks++;
    JSValue v = JS_Eval(g_ctx, expr, strlen(expr), "<sem>", JS_EVAL_TYPE_GLOBAL);
    int bad = 0;
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(g_ctx);
        const char *m = JS_ToCString(g_ctx, e);
        printf("FAIL %s\n     threw: %s\n     expr: %s\n", what, m ? m : "?", expr);
        if (m) JS_FreeCString(g_ctx, m);
        JS_FreeValue(g_ctx, e);
        bad = 1;
    } else if (!JS_ToBool(g_ctx, v)) {
        printf("FAIL %s\n     expr: %s\n", what, expr);
        bad = 1;
    } else {
        printf("ok: %s\n", what);
    }
    if (bad) { fails++; if (in_live) live_fails++; }
    JS_FreeValue(g_ctx, v);
}

static const char *HTML =
    "<!DOCTYPE html><html><head><title>t</title></head><body>"
    "<table id=t><caption>cap</caption><thead><tr id=hr><th>H</th></tr></thead>"
    "<tbody id=tb><tr id=r0><td>a</td><td>b</td></tr><tr id=r1><td>c</td></tr></tbody>"
    "<tfoot><tr id=fr><td>F</td></tr></tfoot></table>"
    "<form id=f><input id=i1 name=q><select id=s>"
    "<option id=o0 value=x>One</option><option id=o1>  Two  </option></select>"
    "<button id=b1 type=button>go</button><textarea id=ta></textarea></form>"
    "<dialog id=dlg>d<button id=dbtn>ok</button></dialog>"
    "<details id=det><summary id=sum>s</summary>body</details>"
    "<div id=pop popover>pop</div>"
    "<button id=inv popovertarget=pop>toggle</button>"
    "<button id=cinv commandfor=dlg command=show-modal>open</button>"
    "<template id=tpl><span>in</span></template>"
    "<h2 id=h2>heading</h2><p id=p1>para</p>"
    "</body></html>";

int main(void)
{
    struct node *root = dom_parse(HTML, (int)strlen(HTML));
    if (!root) { printf("FAIL: parse\n"); return 1; }
    js_page_set_location("http://example.com/");
    if (!js_page_open(root)) { printf("FAIL: js_page_open\n"); return 1; }
    g_ctx = js_page_ctx();
    /* One evaluation first, exactly as dom_iface_test does: js_dom_run_jobs
     * (and with it the compatibility bridge that moves js_select/js_platform's
     * installs off HTMLDivElement.prototype) has to have run before anything
     * below inspects a prototype. Every page gets this; a test that skipped it
     * would be measuring a state no page sees. */
    js_page_eval("var $ = function (i) { return document.getElementById(i); };", 60, "<warmup>");

    printf("== group 1: LIVENESS -- what the negative control must break ==\n");
    in_live = 1;

    /* A collection reflects the tree NOW. Each of these mutates and re-reads
     * through the SAME collection object, which is the only shape of assertion
     * a snapshot implementation cannot satisfy.
     *
     * Every one builds its OWN fixture rather than mutating the document's:
     * this group's whole job is to change the tree, and group 2 asserts indices
     * (`tr.rowIndex`) that a leftover mutation would silently move. That is not
     * hypothetical -- it is how this test failed the first time it ran. */
    ckjs("(function(){var t=document.createElement('table');"
         "t.innerHTML='<tbody><tr><td>a</td></tr></tbody>';"
         "var c=t.rows,n=c.length;"
         "t.tBodies[0].appendChild(document.createElement('tr'));"
         "return c.length === n + 1;})()",
         "table.rows is live: appending a <tr> grows the SAME collection object");
    ckjs("(function(){var s=document.createElement('select');"
         "s.appendChild(document.createElement('option'));"
         "var c=s.options,n=c.length;"
         "var o=document.createElement('option');o.textContent='Three';s.appendChild(o);"
         "return c.length === n + 1 && c[n] === o;})()",
         "select.options is live, and indexes the new option");
    ckjs("(function(){var r=document.createElement('tr');"
         "r.appendChild(document.createElement('td'));"
         "var c=r.cells,n=c.length;"
         "r.appendChild(document.createElement('td'));"
         "return c.length === n + 1;})()",
         "tr.cells is live");
    ckjs("(function(){var f=document.createElement('form');"
         "document.body.appendChild(f);"
         "var c=f.elements,n=c.length;"
         "var i=document.createElement('input');i.name='extra';f.appendChild(i);"
         "var live=c.length === n + 1;f.remove();return live;})()",
         "form.elements is live");
    ckjs("(function(){var t=document.createElement('table');"
         "var c=t.tBodies,n=c.length;"
         "t.createTBody();return c.length === n + 1;})()",
         "table.tBodies is live across createTBody()");
    ckjs("(function(){var t=document.createElement('table');"
         "t.innerHTML='<tbody><tr><td>a</td></tr><tr><td>b</td></tr></tbody>';"
         "var c=t.rows,n=c.length;"
         "t.deleteRow(0);return c.length === n - 1;})()",
         "a deletion shrinks the live collection too");

    in_live = 0;
    printf("== group 2: the interfaces themselves ==\n");

    /* ---- identity + shape ---- */
    ckjs("$('t').rows === $('t').rows", "table.rows has a stable identity");
    ckjs("typeof HTMLCollection !== 'function' || $('t').rows instanceof HTMLCollection",
         "a collection is an HTMLCollection");
    ckjs("Array.prototype.slice.call($('s').options).length === $('s').options.length",
         "a collection is array-like enough for Array.prototype.slice");

    /* ---- <table> ---- */
    ckjs("(function(){var t=document.createElement('table');"
         "t.innerHTML='<thead><tr id=x></tr></thead><tr id=y></tr><tbody><tr id=z></tr></tbody>"
         "<tfoot><tr id=w></tr></tfoot>';"
         "var ids=Array.prototype.map.call(t.rows,function(r){return r.id;}).join(',');"
         "return ids === 'x,y,z,w';})()",
         "table.rows is thead, then the table's own rows and tbodies, then tfoot");
    ckjs("(function(){var t=document.createElement('table');"
         "var r=t.insertRow();return r.tagName.toLowerCase()==='tr' && "
         "t.rows.length===1 && r.parentNode.tagName.toLowerCase()==='tbody';})()",
         "insertRow() on an empty table creates the <tbody> the spec requires");
    ckjs("(function(){var t=document.createElement('table');t.insertRow();"
         "try{t.insertRow(5);return false;}catch(e){return e.name==='IndexSizeError';}})()",
         "insertRow(out of range) throws IndexSizeError");
    ckjs("(function(){var t=document.createElement('table');var c=t.createCaption();"
         "return t.caption===c && t.createCaption()===c && t.firstChild===c;})()",
         "createCaption() is idempotent and inserts first");
    ckjs("(function(){var t=document.createElement('table');t.createCaption();"
         "t.deleteCaption();return t.caption===null;})()",
         "deleteCaption()");
    ckjs("(function(){var t=document.createElement('table');var h=t.createTHead();"
         "var f=t.createTFoot();return t.tHead===h && t.tFoot===f && "
         "t.lastChild===f && t.firstChild===h;})()",
         "createTHead/createTFoot place the sections correctly");
    ckjs("$('r0').rowIndex === 1 && $('hr').rowIndex === 0",
         "tr.rowIndex counts across sections in table.rows order");
    ckjs("$('r0').sectionRowIndex === 0 && $('hr').sectionRowIndex === 0",
         "tr.sectionRowIndex counts within the section");
    ckjs("(function(){var r=$('r1');var n=r.cells.length;var c=r.insertCell(0);"
         "return r.cells.length===n+1 && r.cells[0]===c && c.tagName.toLowerCase()==='td';})()",
         "tr.insertCell(0)");

    /* ---- <select> / <option> ---- */
    ckjs("$('s').type === 'select-one' && $('s').form === $('f')",
         "select.type and select.form");
    ckjs("$('o0').value === 'x' && $('o1').value === 'Two'",
         "option.value falls back to the stripped-and-collapsed text");
    ckjs("$('o1').text === 'Two'", "option.text strips and collapses whitespace");
    ckjs("$('s').selectedIndex === 0 && $('s').value === 'x'",
         "a single select with nothing selected selects its first option");
    ckjs("(function(){$('s').selectedIndex=1;return $('s').value==='Two' && $('o1').selected;})()",
         "setting selectedIndex moves the selection");
    ckjs("(function(){var s=document.createElement('select');"
         "var a=document.createElement('option');s.add(a);"
         "var b=document.createElement('option');s.add(b,a);"
         "return s.options[0]===b && s.options[1]===a;})()",
         "select.add(el, before)");

    /* ---- <form> ---- */
    ckjs("$('f').elements.namedItem('q') === $('i1')",
         "form.elements.namedItem finds by name");
    ckjs("$('b1').form === $('f') && $('ta').form === $('f')",
         "button.form and textarea.form");

    /* ---- <template> ---- */
    ckjs("(function(){var c=$('tpl').content;"
         "return c === $('tpl').content && c.nodeType === 11 && "
         "c.firstChild && c.firstChild.tagName.toLowerCase()==='span';})()",
         "template.content is a stable DocumentFragment holding the children");

    /* ---- popover reflection ---- */
    ckjs("$('pop').popover === 'auto' && document.createElement('div').popover === null",
         "the popover attribute reflects, and is null when absent");
    ckjs("(function(){var d=document.createElement('div');d.popover='garbage';"
         "return d.popover==='manual' && d.getAttribute('popover')==='garbage';})()",
         "an invalid popover value reads back as the invalid-value default 'manual'");
    ckjs("(function(){var d=document.createElement('div');d.popover='auto';d.popover=null;"
         "return d.popover===null && !d.hasAttribute('popover');})()",
         "popover = null removes the attribute (the IDL type is nullable)");
    ckjs("(function(){var d=document.createElement('div');d.popover='ManUaL';return d.popover==='manual';})()",
         "popover keywords are ASCII case-insensitive");

    /* ---- the attr-associated element: the assertion an id lookup fakes ---- */
    ckjs("$('inv').popoverTargetElement === $('pop')",
         "popoverTargetElement resolves the content attribute as an id");
    ckjs("(function(){var b=document.createElement('button');document.body.appendChild(b);"
         "var d=document.createElement('div');d.popover='auto';d.id='detached';"
         "b.popoverTargetElement=d;"
         "var attrIsEmpty = b.getAttribute('popovertarget')==='';"
         "var nullWhileDetached = b.popoverTargetElement===null;"
         "document.body.appendChild(d);"
         "var foundOnceInTree = b.popoverTargetElement===d;"
         "b.remove();d.remove();"
         "return attrIsEmpty && nullWhileDetached && foundOnceInTree;})()",
         "setting popoverTargetElement writes '' and the getter waits for the SAME TREE");
    ckjs("(function(){var b=document.createElement('button');document.body.appendChild(b);"
         "var d=document.createElement('div');d.id='other';document.body.appendChild(d);"
         "b.popoverTargetElement=d;b.setAttribute('popovertarget','nosuchid');"
         "var cleared = b.popoverTargetElement===null;"
         "b.remove();d.remove();return cleared;})()",
         "writing the content attribute clears the explicitly set element");
    ckjs("(function(){var b=document.createElement('button');"
         "b.popoverTargetAction='ShOw';"
         "return b.getAttribute('popovertargetaction')==='ShOw' && b.popoverTargetAction==='show';})()",
         "popoverTargetAction: the setter is verbatim, the getter canonicalises");
    ckjs("(function(){var b=document.createElement('button');b.popoverTargetAction=null;"
         "return b.getAttribute('popovertargetaction')==='null' && b.popoverTargetAction==='toggle';})()",
         "a non-nullable DOMString really does write the four characters 'null'");

    /* ---- command / commandfor ---- */
    ckjs("$('cinv').command === 'show-modal' && $('cinv').commandForElement === $('dlg')",
         "command and commandForElement");
    ckjs("(function(){var b=document.createElement('button');b.command='nonsense';"
         "return b.getAttribute('command')==='nonsense' && b.command==='';})()",
         "an invalid command reads back as the empty string but is stored verbatim");
    ckjs("(function(){var b=document.createElement('button');b.command='--custom';"
         "return b.command==='--custom';})()",
         "a custom command (leading --) passes through the getter");

    /* ---- the popover state machine ---- */
    ckjs("typeof HTMLElement.prototype.showPopover === 'function' && "
         "typeof HTMLElement.prototype.hidePopover === 'function' && "
         "typeof HTMLElement.prototype.togglePopover === 'function'",
         "showPopover / hidePopover / togglePopover exist on HTMLElement");
    ckjs("(function(){var p=$('pop');p.showPopover();"
         "var open=p.matches(':popover-open');p.hidePopover();"
         "return open && !p.matches(':popover-open');})()",
         ":popover-open follows the popover visibility state");
    ckjs("(function(){try{document.createElement('div').showPopover();return false;}"
         "catch(e){return e.name==='NotSupportedError';}})()",
         "showPopover on a non-popover throws NotSupportedError");
    ckjs("(function(){var p=$('pop');p.showPopover();"
         "var threw=false;try{p.showPopover();}catch(e){threw=e.name==='InvalidStateError';}"
         "p.hidePopover();return threw;})()",
         "showing an already-showing popover throws InvalidStateError");
    ckjs("(function(){var p=$('pop'),states=[];"
         "var h=function(e){states.push(e.oldState+'->'+e.newState);};"
         "p.addEventListener('beforetoggle',h);p.showPopover();p.hidePopover();"
         "p.removeEventListener('beforetoggle',h);"
         "return states.join(',')==='closed->open,open->closed';})()",
         "beforetoggle carries oldState/newState both ways");
    ckjs("(function(){var p=$('pop');"
         "var h=function(e){e.preventDefault();};"
         "p.addEventListener('beforetoggle',h);p.showPopover();"
         "p.removeEventListener('beforetoggle',h);"
         "return !p.matches(':popover-open');})()",
         "cancelling beforetoggle cancels the show");
    ckjs("typeof ToggleEvent === 'function' && "
         "new ToggleEvent('toggle',{oldState:'closed',newState:'open'}).newState === 'open' && "
         "new ToggleEvent('toggle') instanceof Event",
         "ToggleEvent is a real Event subclass");
    ckjs("typeof CommandEvent === 'function' && "
         "new CommandEvent('command',{command:'x'}).command === 'x'",
         "CommandEvent exists");

    /* ---- click() and activation ---- */
    ckjs("typeof HTMLElement.prototype.click === 'function'",
         "HTMLElement.prototype.click exists at all");
    ckjs("(function(){var n=0,b=$('b1');var h=function(){n++;};"
         "b.addEventListener('click',h);b.click();b.removeEventListener('click',h);"
         "return n===1;})()",
         "click() dispatches exactly one click event");
    ckjs("(function(){var p=$('pop');$('inv').click();"
         "var open=p.matches(':popover-open');$('inv').click();"
         "return open && !p.matches(':popover-open');})()",
         "clicking a popovertarget button toggles the popover");
    ckjs("(function(){var b=document.createElement('button');b.setAttribute('popovertarget','pop');"
         "b.setAttribute('popovertargetaction','show');document.body.appendChild(b);"
         "b.click();var a=$('pop').matches(':popover-open');b.click();"
         "var still=$('pop').matches(':popover-open');$('pop').hidePopover();b.remove();"
         "return a && still;})()",
         "action=show shows and then leaves the popover showing");
    ckjs("(function(){var n=0,b=$('b1');"
         "var h=function(e){e.preventDefault();n++;};"
         "b.addEventListener('click',h);b.click();b.removeEventListener('click',h);"
         "return n===1;})()",
         "a canceled click still dispatched (activation is what is suppressed)");
    ckjs("(function(){var b=document.createElement('button');b.disabled=true;"
         "var n=0;b.addEventListener('click',function(){n++;});b.click();return n===0;})()",
         "click() on a disabled control does nothing");

    /* ---- <dialog> ---- */
    ckjs("typeof $('dlg').show === 'function' && typeof $('dlg').showModal === 'function' && "
         "typeof $('dlg').close === 'function'",
         "the dialog methods exist");
    ckjs("(function(){var d=$('dlg');d.show();var o=d.open;d.close();"
         "return o && !d.open && !d.hasAttribute('open');})()",
         "dialog.show()/close() drive the open attribute");
    ckjs("(function(){var d=$('dlg');d.showModal();"
         "var m=d.matches(':modal');d.close();return m && !d.matches(':modal');})()",
         ":modal follows showModal()");
    ckjs("(function(){var d=$('dlg');d.showModal();var t=false;"
         "try{d.show();}catch(e){t=e.name==='InvalidStateError';}d.close();return t;})()",
         "show() on a modal dialog throws InvalidStateError");
    ckjs("(function(){var d=$('dlg'),rv=null;"
         "d.addEventListener('close',function(){rv=d.returnValue;},{once:true});"
         "d.show();d.close('done');return rv==='done' && d.returnValue==='done';})()",
         "close(value) sets returnValue before the close event fires");
    ckjs("(function(){var d=$('dlg');$('cinv').click();"
         "var open=d.open && d.matches(':modal');d.close();return open;})()",
         "a command=show-modal button opens the dialog it names");

    /* ---- <details> ---- */
    ckjs("(function(){var d=$('det');var was=d.open;$('sum').click();"
         "var now=d.open;if(now)d.removeAttribute('open');return !was && now;})()",
         "clicking a <summary> opens its <details>");
    ckjs("$('det').matches(':open') === false && "
         "(function(){$('det').setAttribute('open','');var r=$('det').matches(':open');"
         "$('det').removeAttribute('open');return r;})()",
         ":open follows the details open attribute");

    /* ---- focus reaches beyond <input> ---- */
    ckjs("typeof HTMLElement.prototype.focus === 'function'",
         "focus() is on HTMLElement, not only on HTMLInputElement");
    ckjs("typeof HTMLSelectElement.prototype.focus === 'function' && "
         "typeof HTMLButtonElement.prototype.focus === 'function'",
         "so <select> and <button> inherit it");

    /* ---- :heading ---- */
    ckjs("$('h2').matches(':heading') && !$('p1').matches(':heading')",
         ":heading matches h1-h6 and nothing else");
    ckjs("document.querySelectorAll(':heading').length === 1",
         ":heading is queryable rather than a syntax error");

    printf("\n%d checks, %d failed  (liveness group: %d checks, %d failed)\n",
           checks, fails, live_checks, live_fails);
    if (!live_checks) {
        printf("FAIL: the liveness group is empty, so the negative control "
               "cannot mean anything\n");
        return 1;
    }
    return fails ? 1 : 0;
}
