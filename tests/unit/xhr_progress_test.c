/* Real Fetch/HTTP parser -> ReadableStream -> XHR, with server-held chunks.
 * Listener errors are local script errors, not injected transport failures. */
#include "stream_net.h"
static void route(struct fakesock *s,const char *method,const char *target)
{
    (void)method;
    if(!strcmp(target,"/done"))rsp_add(s,"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 3\r\n\r\nabc");
    else if(!strcmp(target,"/empty"))rsp_add(s,"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 0\r\n\r\n");
    else {
        const char *length=!strcmp(target,"/flush")?"2":"4";
        rsp_add(s,"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ");rsp_add(s,length);rsp_add(s,"\r\n\r\n");
        s->avail=s->rsp_len;s->finished=0;
    }
}
static void begin(void)
{
    fs_reset();fs_set_router(route);open_ctx("http://page.example/test");
    run("var reported=[];globalThis.reportError=function(e){reported.push(e.message)};"
        "var x=new XMLHttpRequest(), states=[], progress=[], events=[], loads=0, errors=0, ends=0, aborts=0;"
        "x.onreadystatechange=function(){states.push(x.readyState)};"
        "x.onprogress=function(e){progress.push([x.responseText,e.loaded,e.total,e.lengthComputable,x.readyState]);events.push('progress')};"
        "x.onload=function(){loads++;events.push('load')};x.onerror=function(){errors++;events.push('error')};"
        "x.onloadend=function(){ends++;events.push('loadend')};x.onabort=function(){aborts++;events.push('abort')};");
}
static void incremental(void)
{
    begin();run("x.open('POST','/parts');x.send('local-fixture');");settle(12);int fd=fs_live();
    ck(fd>=0,"fixture has a live response after headers");
    ckjs("x.status===200&&x.readyState===2&&loads===0","headers precede body and completion");
    fs_push(fd,"\xe4\xbd");settle(12);
    ckjs("progress.length>0&&progress[progress.length-1][0]===''&&progress[progress.length-1][1]===2","partial UTF8 publishes byte progress without partial text");
    fs_push(fd,"\xa0");settle(12);
    ckjs("x.responseText==='\\u4f60'&&x.readyState===3&&loads===0","complete character arrives while server is still open");
    ckjs("progress[progress.length-1][1]===3&&progress[progress.length-1][2]===4&&progress[progress.length-1][3]===true","progress counts received bytes and known total");
    fs_push(fd,"!");fs_finish(fd);settle(20);
    ckjs("x.responseText==='\\u4f60!'&&x.response==='\\u4f60!'&&x.readyState===4&&loads===1&&ends===1&&errors===0","real chunks finish once with complete response text");
    ckjs("progress[progress.length-1][0]===x.responseText&&progress[progress.length-1][1]===4&&events.slice(-3).join(',')==='progress,load,loadend'","final progress precedes load and loadend");
    close_ctx();
}
static void final_decoder(void)
{
    begin();run("x.open('GET','/flush');x.send();");settle(12);int fd=fs_live();
    fs_push(fd,"A");settle(12);ckjs("x.responseText==='A'&&loads===0","flush fixture delivers first text before EOF");
    fs_push(fd,"\xe4");fs_finish(fd);settle(20);
    ckjs("x.responseText==='A\\ufffd'&&loads===1&&errors===0","terminal decoder flush updates responseText");
    ckjs("progress[progress.length-1][0]==='A\\ufffd'&&progress[progress.length-1][1]===2","terminal progress exposes decoder final character");
    close_ctx();
}
static void header_exception(void)
{
    begin();run("var headerSecond=0;x.onreadystatechange=function(){if(x.readyState===2)throw new Error('headers-listener')};"
                "x.addEventListener('readystatechange',function(){if(x.readyState===2)headerSecond++});"
                "x.open('GET','/done');x.send();");settle(30);
    ckjs("loads===1&&errors===0&&ends===1&&x.status===200&&x.responseText==='abc'","header callback error cannot become network failure");
    ckjs("headerSecond===1&&reported.indexOf('headers-listener')>=0","header callback error reports and later listener still runs");
    close_ctx();
}
static void progress_exception(void)
{
    begin();run("var afterBad=0;x.onprogress=function(){throw new Error('progress-property')};"
                "x.addEventListener('progress',function(){throw new Error('progress-listener')});"
                "x.addEventListener('progress',{handleEvent:function(){afterBad++}});"
                "x.open('GET','/done');x.send();");settle(30);
    ckjs("loads===1&&ends===1&&errors===0&&x.status===200&&x.responseText==='abc'","progress callback errors cannot strand request completion");
    ckjs("afterBad>0&&reported.indexOf('progress-property')>=0&&reported.indexOf('progress-listener')>=0","progress callback errors report independently and later listener runs");
    close_ctx();
}
static void terminal_exception(void)
{
    begin();run("var laterLoad=0;x.onload=function(){loads++;throw new Error('load-listener')};"
                "x.addEventListener('load',function(){laterLoad++});x.open('GET','/done');x.send();");settle(30);
    ckjs("loads===1&&laterLoad===1&&ends===1&&errors===0&&x.status===200","load listener exception cannot synthesize a second error terminal sequence");
    close_ctx();
}
static void transport_and_abort(void)
{
    begin();run("x.open('GET','/parts');x.send();");settle(12);int fd=fs_live();
    fs_push(fd,"a");settle(12);fs_finish(fd);settle(20);
    ckjs("errors===1&&loads===0&&ends===1&&x.status===0&&x.readyState===4","actual truncated HTTP body still produces network error");
    close_ctx();
    begin();run("x.open('GET','/parts');x.send();");settle(12);fd=fs_live();fs_push(fd,"a");settle(12);
    int closed=fs_closed_count;run("x.abort();");settle(15);
    ck(fs_closed_count>closed,"abort closes the underlying local transport");
    ckjs("aborts===1&&ends===1&&loads===0&&errors===0&&x.readyState===0","abort retains one abort terminal sequence without load/error");
    close_ctx();
}
static void terminal_edges(void)
{
    begin();run("x.open('GET','/empty');x.send();");settle(30);
    ckjs("loads===1&&ends===1&&errors===0&&progress.length===1&&progress[0][0]===''&&progress[0][1]===0&&events.join(',')==='progress,load,loadend'","empty response has terminal progress before load and loadend");
    close_ctx();

    begin();run("x.onprogress=function(){if(x.responseText==='A\\ufffd')x.abort()};x.open('GET','/flush');x.send();");settle(12);
    int fd=fs_live();fs_push(fd,"A");settle(12);fs_push(fd,"\xe4");fs_finish(fd);settle(20);
    ckjs("aborts===1&&ends===1&&loads===0&&errors===0&&x.readyState===0","abort in terminal progress prevents load completion");
    close_ctx();

    begin();run("var laterError=0;x.onerror=function(){errors++;throw new Error('network-error-listener')};"
                "x.addEventListener('error',function(){laterError++});x.open('GET','/parts');x.send();");settle(12);
    fd=fs_live();fs_push(fd,"a");settle(12);fs_finish(fd);settle(20);
    ckjs("errors===1&&laterError===1&&loads===0&&ends===1&&x.status===0&&reported.indexOf('network-error-listener')>=0","network error listener exception still reaches later listener and loadend");
    close_ctx();
}
int main(void)
{
    incremental();final_decoder();header_exception();progress_exception();terminal_exception();transport_and_abort();terminal_edges();
    printf("xhr-progress: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
