#define main select_state_original_main
#include "select_state_test.c"
#undef main
static void gc_now(void){JS_RunGC(JS_GetRuntime(js_page_ctx()));}
int main(void){
 const char *html="<body><button id='b'>0</button><div id='holder'></div></body>";
 struct node *root=dom_parse(html,strlen(html));js_page_open(root);js_page_eval("void 0",6,"<warmup>",0);
 ck("write ordinary Symbol expando","(function(){document.getElementById('b')[Symbol.for('handler')]=function(){this.textContent='1'};})();true");
 gc_now();
 ck("connected Symbol expando survives wrapper release","typeof document.getElementById('b')[Symbol.for('handler')]==='function'");
 ck("delegated composedPath preserves node expandos","document.addEventListener('click',function(e){var p=e.composedPath();for(var i=0;i<p.length;i++){var h=p[i][Symbol.for('handler')];if(h){h.call(p[i]);break}}});document.getElementById('b').dispatchEvent(new Event('click',{bubbles:true}));document.getElementById('b').textContent==='1'");
 ck("class instance expando survives reacquisition","(function(){class State{constructor(){this.n=7}get(){return this.n}}document.getElementById('b').state=new State})();document.getElementById('b').state.get()===7");
 ck("remove keeps JS-retained node identity","var saved=document.getElementById('b');saved.remove();saved.state.get()===7");
 gc_now();ck("reattach keeps Symbol and class state","document.body.appendChild(saved);saved=null;document.getElementById('b').state.get()===7&&typeof document.getElementById('b')[Symbol.for('handler')]==='function'");
 ck("prepare detached subtree state","var island=document.createElement('div');island.innerHTML='<i></i><b></b>';island.firstChild.memo=41;var tail=island.lastChild;island=null;true");
 gc_now();ck("retained descendant keeps sibling expando","tail.previousSibling.memo===41");
 ck("release detached subtree","tail=null;true");gc_now();unsigned baseline=js_dom_wrapper_count();
 ck("create detached cycles","(function(){for(var i=0;i<300;i++){var p=document.createElement('section'),c=document.createElement('i');p.appendChild(c);p.child=c;c.parent=p;p=null;c=null;}})();true");
 unsigned peak=js_dom_wrapper_count();gc_now();unsigned after=js_dom_wrapper_count();checks++;
 printf("wrapper GC: baseline=%u before=%u after=%u\n",baseline,peak,after);
 if(after!=baseline){puts("FAIL detached wrapper cycles are collected");failures++;}
 ck("prepare destroyed wrapper","var victim=document.createElement('div');victim.id='victim';document.body.appendChild(victim);victim.memo=9;true");
 struct node *victim=dom_get_element_by_id(root->doc,"victim");dom_destroy_subtree(victim);
 ck("recycled wrapper becomes safe inert handle","victim.getAttribute('id')===null");
 ck("new slot cannot inherit old expandos","var replacement=document.createElement('div');replacement.memo===undefined");
 ck("release stale handles","victim=replacement=null;true");gc_now();
 const char *fh="<body><button id='foreign'>foreign</button></body>";struct node *foreign=dom_parse(fh,strlen(fh));
 struct node *fn=dom_get_element_by_id(foreign->doc,"foreign");JSContext *ctx=js_page_ctx();JSValue g=JS_GetGlobalObject(ctx);
 JS_SetPropertyStr(ctx,g,"foreignNode",js_dom_wrap_node(ctx,fn));JS_FreeValue(ctx,g);
 dom_free(foreign);
 ck("foreign arena close invalidates before finalizer","foreignNode.getAttribute('id')===null");
 ck("release foreign wrapper after arena close","foreignNode=null;true");gc_now();
 ck("foreign DOMParser import honors shallow and deep copies","var importedDoc=new DOMParser().parseFromString('<body><section id=copy data-k=v><i>child</i></section></body>','text/html');var shallow=document.importNode(importedDoc.body.firstChild,false),deep=document.importNode(importedDoc.body.firstChild,true);shallow.ownerDocument===document&&shallow.getAttribute('data-k')==='v'&&shallow.firstChild===null&&deep.firstChild.tagName==='I'&&deep.textContent==='child'&&importedDoc.body.firstChild.parentNode===importedDoc.body");
 ck("foreign DOMParser adoption copies the full node and detaches the source","var parsed=new DOMParser().parseFromString('<body><b id=foreign><i data-v=7>text</i></b></body>','text/html');var parsedChild=parsed.body.firstChild;var adoptedForeign=document.adoptNode(parsedChild);adoptedForeign.ownerDocument===document&&adoptedForeign.getAttribute('id')==='foreign'&&adoptedForeign.firstChild.getAttribute('data-v')==='7'&&adoptedForeign.textContent==='text'&&parsedChild.parentNode===null");
 ck("DOMParser source wrapper survives its document variable release","parsed=null;parsedChild.textContent==='text'");gc_now();
 ck("release foreign DOMParser wrappers","parsedChild=adoptedForeign=importedDoc=shallow=deep=null;true");gc_now();
 ck("same-document adoption retains own state","var adopted=document.adoptNode(document.getElementById('b'));adopted.state.get()===7");
 ck("release adopted wrapper","adopted=null;true");gc_now();
 js_page_close();checks++;if(js_dom_wrapper_count()!=0){puts("FAIL page close releases every wrapper");failures++;}
 if(!js_page_open(root))return 2;ck("same arena reopened without stale JS pointers","document.body.nodeType===1");js_page_close();dom_free(root);printf("DOM wrapper lifetime: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
