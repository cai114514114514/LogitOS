#define main iface_fixture_main
#include "dom_iface_test.c"
#undef main
int main(void)
{
    struct node *root=dom_parse(HTML,(int)strlen(HTML));
    js_page_set_location("https://example.com/");
    if(!root||!js_page_open(root))return 2;g_ctx=js_page_ctx();
    ckjs("var text=document.getElementById('d').firstChild,records=[],plain=[];var observer=new MutationObserver(function(r){records=records.concat(r)});observer.observe(text,{characterDataOldValue:true});var other=new MutationObserver(function(r){plain=plain.concat(r)});other.observe(text,{characterData:true});true", "native text observers installed");
    struct node *t=dom_get_element_by_id(root->doc,"d")->first_child;
    if(!dom_text_replace(t,0,dom_text_length(t),"native",6))return 2;
    ckjs("records.length===0", "native mutation does not synchronously call JS");
    js_page_pump();
    ckjs("records.length===1&&records[0].type==='characterData'&&records[0].target===text&&records[0].oldValue==='text'", "native text edit delivers one record");
    ckjs("plain.length===1&&plain[0].oldValue===null&&plain[0]!==records[0]", "oldValue filtering creates independent observer records");
    ckjs("text.data='assigned';var r=observer.takeRecords();r.length===1&&r[0].oldValue==='native'", "takeRecords flushes native setter exactly once");
    js_page_pump();
    ckjs("records.length===1", "taken records do not also invoke callback");
    ckjs("text.data='discard';observer.disconnect();true", "disconnect fixture");
    js_page_pump();ckjs("records.length===1", "disconnect discards queued records");
    ckjs("other.disconnect();text.data='before';var fresh=[];observer=new MutationObserver(function(r){fresh=fresh.concat(r)});observer.observe(text,{characterDataOldValue:true});true", "new registration fixture");
    js_page_pump();ckjs("fresh.length===0", "new observer cannot see earlier mutation");
    ckjs("var subtree=[];var sub=new MutationObserver(function(r){subtree=subtree.concat(r)});sub.observe(document.getElementById('d'),{subtree:true,characterDataOldValue:true});text.data='move';document.getElementById('sec').appendChild(text);true", "mutation then reparent fixture");
    js_page_pump();ckjs("subtree.length===1&&subtree[0].target===text&&subtree[0].oldValue==='before'", "subtree eligibility follows mutation-time ancestry");
    ckjs("var invalid=0;try{new MutationObserver(function(){}).observe(text,{})}catch(e){if(e.name==='TypeError')invalid++}try{new MutationObserver(function(){}).observe(text,{characterData:false,characterDataOldValue:true})}catch(e){if(e.name==='TypeError')invalid++}invalid===2", "invalid observe options reject");
    ckjs("var host=document.createElement('div');host.id='native-split';host.textContent='abcd';document.body.appendChild(host);var splitObserver=new MutationObserver(function(){});splitObserver.observe(host,{subtree:true,childList:true,characterDataOldValue:true});true", "split observer installed");
    struct node *sp=dom_get_element_by_id(root->doc,"native-split");
    if(!sp||!dom_text_split(sp->first_child,2))return 2;
    ckjs("var first=host.firstChild,tail=host.lastChild,sr=splitObserver.takeRecords();sr.length===2&&sr[0].type==='childList'&&sr[0].target===host&&sr[0].addedNodes[0]===tail&&sr[0].previousSibling===first&&sr[0].nextSibling===null&&sr[1].type==='characterData'&&sr[1].target===first&&sr[1].oldValue==='abcd'", "splitText insertion and truncation each notify once in order");
    /* takeRecords empties records, not the already queued notification job.
     * End the preceding fixture's turn before testing this new job's order. */
    js_page_pump();
    ckjs("var order=[],orderHost=document.createElement('div');orderHost.textContent='a';document.body.appendChild(orderHost);var orderText=orderHost.firstChild;var orderMO=new MutationObserver(function(){order.push('MO')});orderMO.observe(orderText,{characterData:true});Promise.resolve().then(function(){order.push('P1')});orderText.data='b';Promise.resolve().then(function(){order.push('P2')});true", "mutation microtask order fixture");
    js_page_pump();ckjs("order.join(',')==='P1,MO,P2'", "native MO retains mutation-time microtask position");
    ckjs("orderMO.disconnect();var unionMO=new MutationObserver(function(){});unionMO.observe(orderHost,{subtree:true,childList:true});unionMO.observe(orderText,{characterData:true});orderText.data='c';unionMO.takeRecords().length===1", "observer unions matching registrations by mutation type");
    ckjs("unionMO.disconnect();var unionOld=new MutationObserver(function(){});unionOld.observe(orderHost,{subtree:true,characterData:true});unionOld.observe(orderText,{characterDataOldValue:true});orderText.data='d';var unionRecords=unionOld.takeRecords();unionRecords.length===1&&unionRecords[0].oldValue==='c'", "oldValue requested by any matching registration is retained");
    js_page_pump();
    ckjs("unionOld.disconnect();var mixed=[],mixMO=new MutationObserver(function(){mixed.push('MO')});mixMO.observe(orderHost,{subtree:true,characterData:true,attributes:true});Promise.resolve().then(function(){mixed.push('P1')});orderText.data='e';orderHost.setAttribute('data-x','x');Promise.resolve().then(function(){mixed.push('P2')});true", "mixed producer queue fixture");
    js_page_pump();ckjs("mixed.join(',')==='P1,MO,P2'", "JS and native producers share one notification job");
    ckjs("mixMO.disconnect();var rounds=[],roundMO=new MutationObserver(function(r){rounds.push(r[0].oldValue);if(rounds.length===1)orderText.data='g'});roundMO.observe(orderText,{characterDataOldValue:true});orderText.data='f';true", "callback mutation fixture");
    js_page_pump();ckjs("rounds.join(',')==='e,f'", "callback mutation schedules the next notification job");
    js_page_close();dom_free(root);
    printf("native-mo: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
