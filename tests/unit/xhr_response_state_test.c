/* The page can wrap response getters; transport state must still update.
 * Reuse the real HTTP parser and streamed response fixture, including an
 * incomplete body. No request goes to an external service in this gate. */
#define main xhr_progress_original_main
#include "xhr_progress_test.c"
#undef main
static void wrapped(void)
{
    fs_reset();fs_set_router(route);open_ctx("http://page.example/test");
    run("var made=false, load=0, error=0, end=0, seen=[], x;"
        "var desc=Object.getOwnPropertyDescriptor(XMLHttpRequest.prototype,'responseText');"
        "Object.defineProperty(XMLHttpRequest.prototype,'responseText',{configurable:true,"
        "get:function(){return desc&&desc.get?desc.get.call(this):''}});"
        "var rtDesc=Object.getOwnPropertyDescriptor(XMLHttpRequest.prototype,'responseType'),setterCalls=0;"
        "Object.defineProperty(XMLHttpRequest.prototype,'responseType',{configurable:true,"
        "get:function(){return rtDesc.get.call(this)},set:function(v){setterCalls++;rtDesc.set.call(this,v)}});"
        "try{x=new XMLHttpRequest();made=true}catch(e){}"
        "if(made){x.onprogress=function(){seen.push(x.responseText)};"
        "x.onload=function(){load++};x.onerror=function(){error++};x.onloadend=function(){end++};"
        "x.open('GET','/parts');x.send()}");
    ckjs("made","wrapped readonly getter permits XHR construction");
    settle(12); int fd=fs_live();
    ck(fd>=0,"wrapped XHR opens real local transport");
    if(fd>=0){
        ckjs("setterCalls===0","constructor initializes slots without invoking page setters");
        ckjs("(x.responseType='text',setterCalls===1&&x.responseType==='text')","wrapped writable response descriptor forwards real state");
        ckjs("x.readyState===2&&x.status===200&&load===0","readonly response state publishes headers");
        fs_push(fd,"abc");settle(12);
        ckjs("x.responseText==='abc'&&x.readyState===3&&load===0&&seen[0]==='abc'","wrapped getter observes partial body before completion");
        fs_push(fd,"d");fs_finish(fd);settle(20);
        ckjs("x.responseText==='abcd'&&x.response==='abcd'&&x.readyState===4&&load===1&&error===0&&end===1","wrapped getter retains full body and one completion");
        ckjs("(function(){'use strict';try{x.responseText='changed';return false}catch(e){return e instanceof TypeError&&x.responseText==='abcd'}})()","readonly public response cannot replace internal bytes");
        ckjs("!Object.prototype.hasOwnProperty.call(x,'responseText')&&x.responseURL.indexOf('/parts')>=0","readonly properties remain on the real prototype");
        ckjs("(function(){try{desc.get.call({});return false}catch(e){return e instanceof TypeError}})()","response getter refuses an unrelated receiver");
        run("x.abort()");
        ckjs("x.readyState===0","abort updates private ready state");
    }
    close_ctx();
}
int main(void)
{
    wrapped();
    printf("xhr-response-state: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
