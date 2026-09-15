/* Use the existing real page/DOM fixture and its transport-only stubs. */
#define main iface_fixture_main
#include "dom_iface_test.c"
#undef main
int main(void)
{
    struct node *root = dom_parse(HTML, (int)strlen(HTML));
    js_page_set_location("http://example.com/");
    if (!root || !js_page_open(root)) return 2;
    g_ctx = js_page_ctx(); js_page_eval("void 0;", 7, "warmup", 0);
    ckjs("(function(){var p=document.createElement('div'),c=document.createElement('b'),q=document.createElement('div');document.body.appendChild(p);document.body.appendChild(q);p.appendChild(c);var a,b,e=new Event('path',{bubbles:true});p.addEventListener('path',function(e){a=e.composedPath();q.appendChild(c);b=e.composedPath()},true);c.dispatchEvent(e);return a.length===b.length&&a.every(function(n,i){return n===b[i]})&&a.indexOf(p)>=0&&b.indexOf(q)<0})()", "path frozen across reparent");
    ckjs("(function(){var c=document.createElement('b'),e=new Event('path');if(e.composedPath().length)return false;c.dispatchEvent(e);return e.composedPath().length===0})()", "path empty outside dispatch");
    ckjs("(function(){var c=document.createElement('b'),a=[];c.addEventListener('x',function(){a.push('bubble')});c.addEventListener('x',function(){a.push('capture')},true);c.dispatchEvent(new Event('x'));return a.join(',')==='capture,bubble'})()", "target capture precedes bubble regardless registration");
    ckjs("(function(){var c=document.createElement('b'),e=new Event('x',{cancelable:true});e.preventDefault();return c.dispatchEvent(e)===false&&e.defaultPrevented})()", "dispatch preserves canceled flag");
    ckjs("(function(){var h=document.createElement('div');document.body.appendChild(h);var r=h.attachShadow({mode:'open'}),c=document.createElement('b');r.appendChild(c);var seen=0,inside=0;h.addEventListener('x',function(){seen++});r.addEventListener('x',function(){inside++});c.dispatchEvent(new Event('x',{bubbles:true}));return seen===0&&inside===1})()", "noncomposed event stops at shadow root");
    ckjs("(function(){var h=document.createElement('div');document.body.appendChild(h);var r=h.attachShadow({mode:'open'}),c=document.createElement('b');r.appendChild(c);var target,path;h.addEventListener('x',function(e){target=e.target;path=e.composedPath()});c.dispatchEvent(new Event('x',{bubbles:true,composed:true}));return target===h&&path.indexOf(c)>=0&&path.indexOf(r)>=0})()", "open shadow retarget retains visible path");
    ckjs("(function(){var h=document.createElement('div');document.body.appendChild(h);var r=h.attachShadow({mode:'closed'}),c=document.createElement('b');r.appendChild(c);var out,inside;h.addEventListener('x',function(e){out=e.composedPath()});r.addEventListener('x',function(e){inside=e.composedPath()});c.dispatchEvent(new Event('x',{bubbles:true,composed:true}));return out.indexOf(c)<0&&out.indexOf(r)<0&&out[0]===h&&inside.indexOf(c)>=0})()", "closed shadow path visibility follows listener");
    ckjs("(function(){var p=document.createElement('div'),c=document.createElement('b'),a=[];p.appendChild(c);p.addEventListener('x',function(e){p.removeChild(c);a.push('capture')},true);p.addEventListener('x',function(){a.push('bubble')});c.dispatchEvent(new Event('x',{bubbles:true}));return a.join(',')==='capture,bubble'})()", "detachment preserves captured ancestor invocation");
    js_page_close(); dom_free(root);
    printf("event-path: %d checks, %d failures\n",checks,fails); return fails?1:0;
}
