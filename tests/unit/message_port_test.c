/* Uses the shipping platform installer and page queue. No replacement channel,
 * serializer or timer: a native counter proves old-page callbacks never run.
 * Full installer teardown also catches any retained native JS hook at close. */
#define main old_semantics_main
#include "semantics_test.c"
#undef main
static unsigned long long port_now=1000;
static unsigned long long port_clock(void){return port_now;}
static int old_calls;
static JSValue record_old(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)
{(void)ctx;(void)self;(void)argc;(void)argv;old_calls++;return JS_UNDEFINED;}
static void run(const char *s){ckjs(s,"script setup");}
static void turn(void){port_now++;js_page_run_due();}
int main(void){
 const char *html="<html><body><p id=result>ready</p></body></html>";
 struct node *root=dom_parse(html,strlen(html));js_page_set_clock(port_clock);
 if(!js_page_open(root))return 1;g_ctx=js_page_ctx();
 run("var c=new MessageChannel(),received=[];c.port2.onmessage=function(e){received.push(e.data)};var data={nested:{n:1},bytes:new Uint8Array([3,4])};data.self=data;c.port1.postMessage(data);data.nested.n=9;data.bytes[0]=8;true");turn();
 ckjs("received.length===1&&received[0].nested.n===1&&received[0].bytes[0]===3&&received[0].self===received[0]&&received[0]!==data","message clone is taken before sender mutation");
 run("var cloneFailures=0;[function(){},Symbol('x'),Promise.resolve(1),new WeakMap(),document.body,c.port1].forEach(function(x){try{c.port1.postMessage({x:x})}catch(e){if(e.name==='DataCloneError')cloneFailures++}});true");
 ckjs("cloneFailures===6","uncloneable payloads throw DataCloneError synchronously");
 run("var transferRefused=false;try{c.port1.postMessage(1,[new ArrayBuffer(4)])}catch(e){transferRefused=e.name==='DataCloneError'};true");ckjs("transferRefused","unsupported transfer cannot silently copy");
 run("var savedClone=structuredClone;globalThis.structuredClone=function(){throw Error('page override')};c.port1.postMessage({safe:1});globalThis.structuredClone=savedClone;true");turn();ckjs("received[1].safe===1","port uses captured clone implementation");
 run("var q=new MessageChannel(),order=[];q.port2.addEventListener('message',function(e){order.push(e.data)});q.port1.postMessage(1);q.port1.postMessage(2);true");turn();ckjs("order.length===0","listener-only port waits for start");run("q.port2.start();true");ckjs("order.length===0","start never dispatches synchronously");turn();turn();ckjs("order.join(',')==='1,2'","queued messages retain FIFO order");
 run("var closeCount=0,closed=new MessageChannel();closed.port2.onmessage=function(){closeCount++};closed.port1.postMessage(1);closed.port2.close();closed.port2.onmessage=function(){closeCount++};closed.port2.start();closed.port1.postMessage(2);true");turn();turn();ckjs("closeCount===0","closed destination never receives queued or new messages");
 run("var sent=new MessageChannel(),senderClosed=0;sent.port2.onmessage=function(e){senderClosed=e.data};sent.port1.postMessage(7);sent.port1.close();true");turn();ckjs("senderClosed===7","sender close preserves already queued receiver message");
 run("var protoSource=JSON.parse('{\"__proto__\":{\"flag\":1}}'),protoReceived;var pc=new MessageChannel();pc.port2.onmessage=function(e){protoReceived=e.data};pc.port1.postMessage(protoSource);true");turn();ckjs("Object.prototype.hasOwnProperty.call(protoReceived,'__proto__')&&Object.getPrototypeOf(protoReceived)===Object.prototype","clone preserves proto key as own data");
 JSValue global=JS_GetGlobalObject(g_ctx);JS_SetPropertyStr(g_ctx,global,"recordOld",JS_NewCFunction(g_ctx,record_old,"recordOld",0));JS_FreeValue(g_ctx,global);
 run("var stale=new MessageChannel();stale.port2.onmessage=recordOld;stale.port1.postMessage('old');true");
 js_page_close();dom_free(root);root=dom_parse(html,strlen(html));if(!js_page_open(root))return 1;g_ctx=js_page_ctx();turn();
 checks++;if(old_calls){fails++;printf("FAIL navigation canceled old-page port delivery\n");}else printf("ok: navigation canceled old-page port delivery\n");
 js_page_close();dom_free(root);printf("message-port: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
