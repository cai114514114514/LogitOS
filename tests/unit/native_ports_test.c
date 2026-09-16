/* Two genuine runtimes; no page globals, DOM or network fixture can conceal
 * foreign-context ownership. The only transport is the shipping port broker. */
#include "js_ports.h"
#include <stdio.h>
#include <string.h>
static int checks,failures;
static void ck(int ok,const char *name){checks++;if(!ok){failures++;printf("FAIL: %s\n",name);}else printf("ok: %s\n",name);}
static JSValue eval(JSContext *c,const char *s){return JS_Eval(c,s,strlen(s),"<port test>",JS_EVAL_TYPE_GLOBAL);}
static void run(JSContext *c,const char *s){JSValue v=eval(c,s);if(JS_IsException(v)){JSValue e=JS_GetException(c);const char *m=JS_ToCString(c,e);fprintf(stderr,"fixture exception: %s\n",m?m:"?");if(m)JS_FreeCString(c,m);JS_FreeValue(c,e);failures++;}JS_FreeValue(c,v);}
static void expect(JSContext *c,const char *s,const char *name){JSValue v=eval(c,s);ck(!JS_IsException(v)&&JS_ToBool(c,v)>0,name);if(JS_IsException(v))JS_FreeValue(c,JS_GetException(c));JS_FreeValue(c,v);}
static struct js_port_packet *packet(JSContext *c,const char *data,const char *ports)
{JSValue d=eval(c,data),x=eval(c,ports);struct js_port_packet *p=js_ports_prepare(c,d,x);JS_FreeValue(c,d);JS_FreeValue(c,x);return p;}
static int receive(JSContext *c,struct js_port_packet *p,const char *name)
{
    JSValue ports,data=js_ports_read(c,p,&ports);int ok=!JS_IsException(data);
    ck(ok,name);JS_FreeValue(c,data);if(!ok){JS_FreeValue(c,JS_GetException(c));JS_FreeValue(c,ports);return 0;}
    JSValue g=JS_GetGlobalObject(c);JS_SetPropertyStr(c,g,"received",ports);JS_FreeValue(c,g);return 1;
}
static void drain(JSContext *c){for(int i=0;i<12&&js_ports_pending(c);i++)js_ports_pump(c);}
int main(void)
{
    JSRuntime *ar=JS_NewRuntime(),*br=JS_NewRuntime();JSContext *a=JS_NewContext(ar),*b=JS_NewContext(br);
    ck(js_ports_install(a)&&js_ports_install(b),"independent port realms install");
    expect(a,"new MessageChannel() instanceof MessageChannel","MessageChannel construction retains its interface prototype");
    run(a,"var c=new MessageChannel(),seen=[],value={n:1};c.port2.onmessage=e=>seen.push([e.data.n,e.isTrusted,e.target===c.port2,e.currentTarget===c.port2]);c.port1.postMessage(value);value.n=9;");
    expect(a,"seen.length===0","local port delivery is asynchronous");
    ck(js_ports_pending(a)&&!js_ports_pending(b),"pending work belongs only to receiver realm");drain(b);
    expect(a,"seen.length===0","foreign realm pump cannot run owner callbacks");drain(a);
    expect(a,"JSON.stringify(seen)==='[[1,true,true,true]]'","message snapshot and native event ownership");
    run(a,"var once=0;c.port2.addEventListener('message',()=>once++,{once:true});c.port1.postMessage({n:2});c.port1.postMessage({n:3});");drain(a);
    expect(a,"once===1&&seen.length===3&&seen[1][0]===2&&seen[2][0]===3","FIFO and once listener survive separate turns");
    run(a,"c.port1.postMessage({n:4});c.port2.close();");drain(a);
    expect(a,"seen.length===3","closed port drops its queued callbacks");
    run(a,"var x=new MessageChannel(),answers=[];x.port1.onmessage=e=>{answers.push(e.data);if(e.ports.length)e.ports[0].postMessage(77)};x.port1.postMessage({n:7});");
    struct js_port_packet *p=packet(a,"'handoff'","[x.port2]");ck(p&&js_ports_commit(a,p),"port transfers commit after cloning");
    int delivered=p&&receive(b,p,"transferred endpoint attaches only in destination runtime");js_ports_discard(p);
    if(!delivered)goto done; /* owner-negative must fail as an assertion, not a foreign-runtime free */
    expect(b,"Object.isFrozen(received)&&received[0] instanceof MessagePort","transferred event ports are a frozen recipient-realm array");
    run(a,"x.port2.postMessage('old-wrapper');x.port2.close();");drain(a);
    expect(a,"answers.length===0","detached sender wrapper cannot send or close new owner");
    run(b,"var remote=received[0];remote.onmessage=e=>remote.postMessage(e.data.n*2);");
    ck(!js_ports_pending(a)&&js_ports_pending(b),"queued messages move with transferred endpoint");drain(b);drain(a);
    expect(a,"answers.length===1&&answers[0]===14","pre-transfer queued message reaches new owner");
    run(a,"x.port1.postMessage({n:8})");drain(b);drain(a);
    expect(a,"answers[1]===16","cross-runtime channel replies reach original peer");
    run(b,"var y=new MessageChannel(),back=[];y.port1.onmessage=e=>back.push(e.data);remote.postMessage('nested-channel',[y.port2]);");drain(a);drain(b);
    expect(b,"back.length===1&&back[0]===77","a channel can carry another channel endpoint");
    run(a,"var z=new MessageChannel(),zseen=[];z.port1.onmessage=e=>zseen.push(e.data);");
    p=packet(a,"function(){}","[z.port2]");ck(!p,"clone failure rejects before port detachment");JS_FreeValue(a,JS_GetException(a));js_ports_discard(p);
    p=packet(a,"0","[z.port2,z.port2]");ck(!p,"duplicate transfer is rejected atomically");JS_FreeValue(a,JS_GetException(a));js_ports_discard(p);
    run(a,"z.port2.postMessage('still-owned')");drain(a);
    expect(a,"zseen[0]==='still-owned'","failed serialization preserves sender endpoint");
    p=packet(a,"z.port2","[z.port2]");ck(!p,"unsupported port in data graph is an explicit clone failure");JS_FreeValue(a,JS_GetException(a));js_ports_discard(p);
    p=packet(a,"0","[new ArrayBuffer(8)]");ck(!p,"unsupported buffer transfer is not silently copied");JS_FreeValue(a,JS_GetException(a));js_ports_discard(p);
    p=packet(a,"0","[z.port2]");ck(p&&js_ports_commit(a,p),"discard fixture transfers a real endpoint");js_ports_discard(p);
    run(a,"z.port1.postMessage('discarded')");ck(!js_ports_pending(a),"discarded transit packet closes destination queue");
    run(b,"var unstarted=new MessageChannel(),started=[];unstarted.port2.addEventListener('message',e=>started.push(e.data));unstarted.port1.postMessage(1);");
    ck(!js_ports_pending(b),"addEventListener alone does not start a port");
    run(b,"unstarted.port2.start()");drain(b);expect(b,"started[0]===1","start enables pending message delivery");
    /* A port can carry application buffers larger than an IPC control record.
     * Verify bytes after asynchronous delivery, not just a successful enqueue.
     * Refusals must leave transferable ownership intact and release quota. */
    run(a,"var bulk=new MessageChannel(),bulkSeen=0,bulkOK=true;bulk.port2.onmessage=e=>{var v=new Uint8Array(e.data);bulkSeen=v.length===262144&&v[0]===17&&v[v.length-1]===93};var bytes=new Uint8Array(262144);bytes[0]=17;bytes[bytes.length-1]=93;try{bulk.port1.postMessage(bytes.buffer)}catch(e){bulkOK=false}");
    expect(a,"bulkOK&&!bulkSeen","large port message accepted without synchronous dispatch");
    drain(a);expect(a,"bulkSeen","large port message preserves complete buffer bytes");
    run(a,"var over=new MessageChannel(),overSeen=0,overName='';over.port1.onmessage=e=>overSeen=e.data;try{bulk.port1.postMessage(new ArrayBuffer(2097152),[over.port2])}catch(e){overName=e.name}over.port2.postMessage(123)");
    drain(a);expect(a,"overName==='QuotaExceededError'&&overSeen===123","oversized message refusal preserves transferred endpoint ownership");
    struct js_port_packet *held[16]={0};int held_n=0,quota=0;
    for(;held_n<16;held_n++){
        held[held_n]=packet(a,"new ArrayBuffer(700*1024)","undefined");
        if(!held[held_n]){
            JSValue ex=JS_GetException(a),name=JS_GetPropertyStr(a,ex,"name");
            const char *s=JS_ToCString(a,name);quota=s&&!strcmp(s,"QuotaExceededError");
            if(s)JS_FreeCString(a,s);JS_FreeValue(a,name);JS_FreeValue(a,ex);break;
        }
    }
    ck(held_n>1&&held_n<16&&quota,"aggregate port byte budget remains bounded");
    for(int i=0;i<held_n;i++)js_ports_discard(held[i]);
    p=packet(a,"new ArrayBuffer(700*1024)","undefined");
    ck(p!=NULL,"discarding queued packets releases aggregate byte quota");
    if(!p)JS_FreeValue(a,JS_GetException(a));js_ports_discard(p);
    run(a,"bulk.port1.close();bulk.port2.close();over.port1.close();over.port2.close()");
    run(a,"x.port1.postMessage({n:9})");js_ports_close(b);
    ck(!js_ports_pending(b),"closing receiving realm discards its pending port work");
    run(a,"x.port1.postMessage({n:10})");ck(!js_ports_pending(a),"surviving peer does not retain a closed realm");
done:
    js_ports_close(a);js_ports_close(b);JS_FreeContext(a);JS_FreeContext(b);JS_FreeRuntime(ar);JS_FreeRuntime(br);
    printf("native-ports: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
