/* Native DOM/QuickJS contracts. A constant empty array passes the fallback
 * checks, so named/default contents, mutation and closed-root controls are
 * asserted independently. CSS/layout integration has its own native fixture. */
#define main cssom_original_main
#include "cssom_test.c"
#undef main

static void slot_tests(void)
{
    CK(eq("typeof HTMLSlotElement.prototype.assignedElements", "function"), "SLOT interface is installed on its actual prototype");
    CK(eq("'assignedElements' in document.createElement('div')", "false"), "ordinary elements do not acquire slot methods");
    CK(eq("var h=document.createElement('div');document.body.appendChild(h);"
          "h.innerHTML='<i id=a slot=title>A</i>text<b id=b>B</b><!--ignored--><u slot=missing>M</u>';"
          "var sr=h.attachShadow({mode:'open'});sr.innerHTML='<slot name=title></slot><slot></slot><slot name=title><em>fallback</em></slot>';"
          "var title=sr.firstChild,body=title.nextSibling,duplicate=sr.lastChild;"
          "title.assignedElements().length===1 && title.assignedElements()[0]===h.firstChild", "true"), "SLOT named distribution returns the real direct child");
    CK(eq("body.assignedNodes().map(function(n){return n.nodeType}).join(',')", "3,1"), "default slot includes text and elements, not comments");
    CK(eq("body.assignedElements()[0].id", "b"), "element filter excludes default-slot text");
    CK(eq("h.firstChild.assignedSlot===title && h.childNodes[1].assignedSlot===body", "true"), "Element and Text assignedSlot agree with assigned lists");
    CK(eq("duplicate.assignedNodes().length", "0"), "first matching slot wins a duplicate name");
    CK(eq("duplicate.assignedElements({flatten:true})[0].tagName", "EM"), "flatten selects fallback only when assignment is empty");
    CK(eq("var snapshot=title.assignedNodes();h.firstChild.setAttribute('slot','');"
          "snapshot.length===1 && title.assignedNodes().length===0 && body.assignedNodes().length===3", "true"), "SLOT name mutation recomputes distribution without changing old result arrays");
    CK(eq("h.firstChild.setAttribute('slot','title');sr.insertBefore(duplicate,title);"
          "duplicate.assignedNodes()[0]===h.firstChild && title.assignedNodes().length===0", "true"), "slot reorder changes the first-match winner");
    CK(eq("duplicate.name='other';title.assignedNodes()[0]===h.firstChild && duplicate.getAttribute('name')==='other'", "true"), "slot name IDL setter changes native assignment");
    CK(eq("var a=h.firstChild;a.remove();a.assignedSlot===null && title.assignedNodes().length===0", "true"), "removing a slottable removes its assignment immediately");
    CK(eq("h.appendChild(a);a.assignedSlot===title", "true"), "reinserting a retained slottable restores its assignment");
    CK(eq("var outside=document.createElement('slot');outside.innerHTML='<i>outside</i>';"
          "outside.assignedNodes().length===0 && outside.assignedNodes({flatten:true}).length===0", "true"), "slot outside a shadow root has no distribution, even with fallback");
    CK(eq("var leaf=document.createElement('i');h.firstChild.appendChild(leaf);leaf.assignedSlot", "null"), "only host direct children are slottable into this root");
    CK(eq("var closedHost=document.createElement('div');closedHost.innerHTML='<b>C</b>';"
          "var closedRoot=closedHost.attachShadow({mode:'closed'});closedRoot.innerHTML='<slot></slot>';"
          "closedRoot.firstChild.assignedNodes()[0]===closedHost.firstChild && closedHost.firstChild.assignedSlot===null", "true"), "closed root can distribute internally without exposing assignedSlot");
    CK(eq("var manualHost=document.createElement('div');manualHost.innerHTML='<b>M</b>';"
          "var manualRoot=manualHost.attachShadow({mode:'open',slotAssignment:'manual'});manualRoot.innerHTML='<slot><em>F</em></slot>';"
          "manualRoot.firstChild.assignedNodes().length===0 && manualHost.firstChild.assignedSlot===null", "true"), "manual mode does not silently use automatic assignment");
    CK(eq("var outer=document.createElement('div');outer.innerHTML='<b slot=forward id=forwarded>F</b>';"
          "var root=outer.attachShadow({mode:'open'});root.innerHTML='<div><slot name=forward></slot></div>';"
          "var inner=root.firstChild,forward=inner.firstChild;var innerRoot=inner.attachShadow({mode:'open'});innerRoot.innerHTML='<slot></slot>';"
          "var receiver=innerRoot.firstChild;receiver.assignedNodes()[0]===forward && receiver.assignedElements({flatten:true})[0]===outer.firstChild", "true"), "SLOT flatten follows forwarded slots across nested hosts");
    CK(eq("forward.name='empty';forward.innerHTML='<span>Nested fallback</span>';"
          "receiver.assignedElements({flatten:true})[0]===forward.firstChild", "true"), "forwarded slot falls back to its own children");
    CK(eq("var plainHost=document.createElement('div');plainHost.appendChild(outside);"
          "var plainRoot=plainHost.attachShadow({mode:'open'});plainRoot.innerHTML='<slot></slot>';"
          "plainRoot.firstChild.assignedElements({flatten:true})[0]===outside", "true"), "a forwarded slot outside a shadow tree is not flattened away");
    CK(eq("var htmlSlot=document.createElement('slot'),svgSlot=document.createElementNS('http://www.w3.org/2000/svg','slot');"
          "var wrong=0;for(var x of [{},svgSlot]){try{HTMLSlotElement.prototype.assignedNodes.call(x)}catch(e){if(e instanceof TypeError)wrong++}}wrong", "2"), "slot methods require a real HTML slot receiver");
    CK(eq("(function(){try{title.assignedNodes(1);return false}catch(e){return e instanceof TypeError}})()", "true"), "options follow dictionary conversion, not arbitrary boxing");
    CK(eq("var reads=0;title.assignedNodes({get flatten(){reads++;return false}});reads", "1"), "flatten option is read exactly once");
    CK(eq("(function(){try{title.assignedNodes({get flatten(){throw Error('option')}});return false}catch(e){return e.message==='option'}})()", "true"), "options getter exceptions are not swallowed");
    CK(eq("title.assignedNodes(null).length===title.assignedNodes().length", "true"), "null dictionary uses defaults");
    CK(eq("title.name='n\\u0000x';a.setAttribute('slot','n\\u0000y');title.assignedNodes().length", "0"), "slot names compare full bytes, not C-string prefixes");
    CK(eq("a.setAttribute('slot','n\\u0000x');title.assignedNodes()[0]===a", "true"), "matching embedded-NUL names retain their exact identity");
    CK(eq("var deepHost=document.createElement('div');var deep=deepHost.attachShadow({mode:'open'}),top=document.createElement('slot'),bottom=top;deep.appendChild(top);"
          "for(var i=0;i<600;i++){var next=document.createElement('slot');bottom.appendChild(next);bottom=next}"
          "var end=document.createElement('b');bottom.appendChild(end);top.assignedElements({flatten:true})[0]===end", "true"), "flatten uses bounded native stack space for deep fallback trees");
    JS_RunGC(JS_GetRuntime(ctx));
    CK(eq("title.assignedNodes()[0]===a && snapshot[0]===a", "true"), "result arrays retain original DOM wrapper identities across GC");
}

int main(void)
{
    const char *html="<!doctype html><html><body></body></html>";
    for (int turn=0;turn<2;turn++) {
        g_root=dom_parse(html,(int)strlen(html));if(!g_root)return 2;
        js_page_set_clock(clk);if(!js_page_open(g_root))return 2;ctx=js_page_ctx();
        slot_tests();js_page_close();dom_free(g_root);
    }
    printf("slot-assignment: %d checks, %s\n",checks,fails?"FAIL":"PASS");return fails;
}
