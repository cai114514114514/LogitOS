/* SPDX-License-Identifier: MIT
 * Real Worker-to-page transfer, not a same-runtime channel substitute. */
#define main legacy_worker_main
#include "worker_test.c"
#undef main
int main(void)
{
    fake_site_reset();js_page_set_clock(clock_fn);
    struct node *root=dom_parse(PAGE,strlen(PAGE));
    js_page_set_location("https://worker.example/page");
    ck(js_page_open(root),"page opens for outbound port consumer");ctx=js_page_ctx();
    run("var got=[],portOK=false,errors=[];var u=URL.createObjectURL(new Blob(["
        "\"var c=new MessageChannel();c.port1.onmessage=e=>c.port1.postMessage(e.data+1);\"+"
        "\"c.port1.postMessage(19);postMessage('handoff',{transfer:[c.port2]});c.port2.postMessage(99);c.port2.close();\""
        "]));var w=new Worker(u);URL.revokeObjectURL(u);"
        "w.onerror=e=>errors.push(e.message);w.onmessage=function(e){"
        "portOK=e.data==='handoff'&&Object.isFrozen(e.ports)&&e.ports.length===1&&e.ports[0] instanceof MessagePort;"
        "if(portOK){var p=e.ports[0];p.onmessage=e=>got.push(e.data);p.postMessage(40)}};");
    ckjs("got.length===0&&!portOK","outbound handoff is asynchronous");
    for(int i=0;i<30;i++)tick(1);
    ckjs("errors.length===0&&portOK","Worker handoff exposes recipient-realm frozen event ports");
    ckjs("got.length===2&&got[0]===19&&got[1]===41","transferred Worker port preserves queued data and roundtrip");
    run("w.terminate()");for(int i=0;i<5;i++)tick(1);
    /* Clone/duplicate failures precede commit. The same endpoint must still
     * work locally and subsequently transfer after those refusals. */
    run("var rejects=[];var u=URL.createObjectURL(new Blob(["
        "\"var c=new MessageChannel(),n=0;try{postMessage(function(){},[c.port2])}catch(e){n+=e.name==='DataCloneError'}\"+"
        "\"try{postMessage(0,[c.port2,c.port2])}catch(e){n+=e.name==='DataCloneError'}\"+"
        "\"c.port1.onmessage=e=>postMessage(n===2&&e.data===8?'atomic':'bad');c.port2.postMessage(8);\""
        "]));var w2=new Worker(u);URL.revokeObjectURL(u);w2.onmessage=e=>rejects.push(e.data);");
    for(int i=0;i<20;i++)tick(1);
    ckjs("rejects.length===1&&rejects[0]==='atomic'","Worker transfer refusal is atomic");
    run("w2.terminate()");for(int i=0;i<5;i++)tick(1);
    /* 270 cancelled in-transit ports exceed the broker's 256 endpoint slots
     * if task_free forgets packet disposal. Each worker closes before its
     * parent event can run; no synthetic endpoint counter is the oracle. */
    run("var cancelled=0,cancelErrors=0;var cu=URL.createObjectURL(new Blob(["
        "\"var c=new MessageChannel();postMessage('must-drop',[c.port2]);close();\""
        "]));");
    for(int i=0;i<270;i++){
        run("var cw=new Worker(cu);cw.onmessage=()=>cancelled++;cw.onerror=()=>cancelErrors++;");
        for(int j=0;j<4;j++)tick(1);
    }
    ckjs("cancelled===0&&cancelErrors===0","close discards in-transit ports without exhausting broker slots");
    run("URL.revokeObjectURL(cu)");
    js_page_close();dom_free(root);
    ck(!js_worker_pending(),"document close leaves no pending Worker tasks");
    printf("worker-ports: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
