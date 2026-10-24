/* Exercise cancellation through the shipping Fetch path. The transport only
 * controls when headers/body become available; all signals and jobs are real. */
#include "stream_net.h"

static void router(struct fakesock *s, const char *method, const char *target)
{
    (void)method; (void)target;
    rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n");
    s->avail = -1;
    s->finished = 0;
}

static void signal_checks(void)
{
    ckjs("typeof AbortSignal.any==='function' && typeof AbortSignal.timeout==='function' && typeof AbortSignal.abort==='function'",
         "all three signal factories exist in a Fetch-only realm");
    ckjs("AbortController.prototype.abort.name==='abort' && AbortController.prototype.abort.length===0"
         "&& AbortSignal.abort.name==='abort' && AbortSignal.prototype.addEventListener.length===2"
         "&& AbortSignal.prototype.removeEventListener.length===2"
         "&& Object.getOwnPropertyDescriptor(AbortSignal.prototype,'reason').get.name==='get reason'",
         "operations expose the WebIDL name and required argument count");
    ckjs("(function () { try { new AbortSignal(); return false; } catch (e) { return e instanceof TypeError; } })()",
         "AbortSignal cannot be constructed by author code");
    ckjs("(function () { var c=new AbortController(), s=c.signal; s.aborted=true; s.reason=7; c.signal=null;"
         "return c.signal===s && !s.aborted && s.reason===undefined; })()",
         "controller signal and signal state are readonly");
    ckjs("(function () { var c=new AbortController(), s=c.signal, n=0;"
         "function f(){n++;} s.addEventListener('abort',f); s.addEventListener('abort',f);"
         "c.abort(0); c.abort(1); return n===1 && s.reason===0; })()",
         "listener identity is deduplicated and first reason wins");
    ckjs("(function () { var c=new AbortController(), s=c.signal, n=0;"
         "s.addEventListener('abort',{handleEvent:function(e){if(e.target===s)n++;}});"
         "c.abort(); return n===1; })()", "handleEvent listeners receive the signal event");
    ckjs("(function () { var c=new AbortController(), n=0; function f(){n++;}"
         "c.signal.addEventListener('abort',f,{capture:true}); c.signal.removeEventListener('abort',f);"
         "c.abort(); return n===1; })()", "removal matches the capture option");
    ckjs("(function () { var c=new AbortController(), n=0; function f(){n+=10;}"
         "c.signal.addEventListener('abort',function(){c.signal.removeEventListener('abort',f);});"
         "c.signal.addEventListener('abort',f); c.abort(); return n===0; })()",
         "a listener removed during dispatch does not run");
    ckjs("(function () { var c=new AbortController(), n=0;"
         "c.signal.addEventListener('abort',function(){throw 7;});"
         "c.signal.addEventListener('abort',function(){n++;});"
         "try { c.abort(); } catch(e) { return false; } return n===1; })()",
         "listener exceptions do not stop cancellation");
    ckjs("(function () { var c=new AbortController(), seen=[];"
         "c.signal.addEventListener('abort',function(){seen.push(1);});"
         "c.signal.onabort=function(){seen.push(2);};"
         "c.signal.addEventListener('abort',function(){seen.push(3);});"
         "c.abort(); return seen.join(',')==='1,2,3'; })()", "onabort follows listener registration order");
    ckjs("(function () { var s=AbortSignal.abort(null); try{s.throwIfAborted();}catch(e){return e===null;} return false; })()",
         "static abort and throwIfAborted preserve a null reason");
    ckjs("(function () { var s=AbortSignal.any([]); return !s.aborted && s.reason===undefined; })()",
         "any of an empty sequence remains pending");
    ckjs("(function () { var a=new AbortController(), b=new AbortController(); a.abort('a'); b.abort('b');"
         "return AbortSignal.any([b.signal,a.signal]).reason==='b'; })()",
         "any chooses the first already aborted input");
    ckjs("(function () { var a=new AbortController(), b=new AbortController(), n=0;"
         "var s=AbortSignal.any(new Set([a.signal,b.signal])); s.addEventListener('abort',function(){n++;});"
         "b.abort(false); a.abort('late'); return s.aborted && s.reason===false && n===1; })()",
         "any consumes an iterable and propagates the winning reason once");
    ckjs("(function () { var a=new AbortController(), x=AbortSignal.any([a.signal]), y=AbortSignal.any([x]);"
         "var ready=false; a.signal.addEventListener('abort',function(){ready=x.aborted&&y.aborted;});"
         "a.abort('chain'); return ready && y.reason==='chain'; })()",
         "all dependent states settle before source listeners run");
    ckjs("(function () { var a=new AbortController(), s=AbortSignal.any([a.signal]);"
         "a.signal.addEventListener('abort',function(e){e.stopImmediatePropagation();});"
         "a.abort('stop'); return s.aborted && s.reason==='stop'; })()",
         "event propagation cannot block dependent cancellation");
    ckjs("(function () { if(typeof AbortSignal.any!=='function')return false; try{AbortSignal.any([AbortSignal.abort(),{}]);return false;}catch(e){return e instanceof TypeError;} })()",
         "any validates the entire sequence before choosing an aborted input");
    ckjs("(function () { if(typeof AbortSignal.timeout!=='function')return false; return [-1,NaN,Infinity,Math.pow(2,64),1n].every(function(n){"
         "try{AbortSignal.timeout(n);return false;}catch(e){return e instanceof TypeError;}}); })()",
         "timeout enforces the unsigned integer WebIDL range");
    ckjs("(function () { try{AbortSignal.prototype.throwIfAborted.call({aborted:false});return false;}"
         "catch(e){return e instanceof TypeError;} })()", "signal methods reject a forged receiver");
    ckjs("(function () { var c=new AbortController(), n=0;"
         "c.signal.addEventListener('probe',function(){n++;},{once:true});"
         "function event(){return typeof Event==='function'?new Event('probe'):{type:'probe'};}"
         "c.signal.dispatchEvent(event()); c.signal.dispatchEvent(event());"
         "return n===1 && !c.signal.aborted; })()", "dispatchEvent delivers synthetic events and honors once");
    ckjs("(function () { var c=new AbortController(), guard=new AbortController(), n=0;"
         "c.signal.addEventListener('abort',function(){n++;},{signal:guard.signal});"
         "guard.abort(); c.abort(); return n===0; })()", "listener signal option removes the guarded listener");
    ckjs("(function () { var c=new AbortController(), n=0; function f(){n++;}"
         "c.signal.onabort=f; c.signal.onabort=null; c.signal.onabort=f; c.abort();"
         "return n===1; })()", "onabort may be cleared and reinstalled");
    run("var root=new AbortController(), retainedCount=0;"
        "(function(){var child=AbortSignal.any([root.signal]);child.onabort=function(){retainedCount++;};})();");
    JS_RunGC(rt);
    run("root.abort('gc');");
    ckjs("retainedCount===1", "a dependent with an observer survives collection");
    run("var distant=AbortSignal.timeout(4294967296);");
    settle(4);
    ckjs("!distant.aborted", "a large delay does not wrap into an immediate timeout");
    ckjs("new Request('/hold').signal instanceof AbortSignal", "each Request owns a pending signal by default");
    ckjs("(function(){var c=new AbortController(), r=new Request('/hold',{signal:c.signal});"
         "var copy=r.clone();c.abort('request');return r.signal!==c.signal && copy.signal!==r.signal"
         "&& r.signal.reason==='request' && copy.signal.reason==='request';})()",
         "Request and clone own distinct following signals");
    ckjs("(function(){var c=new AbortController(),r=new Request('/hold',{signal:c.signal});"
         "r.signal=null;return r.signal instanceof AbortSignal;})()", "Request.signal is readonly");
    ckjs("(function(){try{new Request('/hold',{signal:{aborted:false}});return false;}"
         "catch(e){return e instanceof TypeError;}})()", "Request validates its signal brand");
}

static void fetch_checks(void)
{
    fs_reset();
    run("var reasonResult='pending'; var pre=new AbortController(); pre.abort(false);"
        "fetch('/hold',{signal:pre.signal}).catch(function(e){reasonResult=e;});");
    settle(3);
    ckjs("reasonResult===false", "pre-aborted fetch preserves a falsy reason");
    ck(fs_live()<0, "pre-aborted fetch never opens a socket");

    fs_reset();
    run("var active=new AbortController(), activeResult='pending';"
        "fetch('/hold',{signal:active.signal}).catch(function(e){activeResult=e;});");
    settle(6);
    ck(fs_live()>=0, "active cancellation control has a live socket");
    run("active.abort(null);");
    settle(4);
    ckjs("activeResult===null", "active fetch preserves a null abort reason");
    ck(fs_live()<0, "active cancellation closes the transport");

    fs_reset();
    run("var fake=new AbortController(), fakeResult='pending';"
        "fetch('/hold',{signal:fake.signal}).catch(function(e){fakeResult=e;});");
    settle(6);
    run("fake.signal.dispatchEvent(typeof Event==='function'?new Event('abort'):{type:'abort'});");
    settle(3);
    ck(fs_live()>=0, "a synthetic abort event cannot cancel the transport");
    run("fake.abort('real');");
    settle(3);
    ckjs("fakeResult==='real'", "real abort still works after a synthetic event");

    fs_reset();
    run("var timed=AbortSignal.timeout(60), timeoutResult='pending';"
        "fetch('/hold',{signal:timed}).catch(function(e){timeoutResult=e;});");
    ckjs("!timed.aborted", "timeout queues a task rather than aborting synchronously");
    settle(20);
    ckjs("timed.aborted && timed.reason.name==='TimeoutError' && timeoutResult===timed.reason",
         "timeout cancels fetch with the signal's TimeoutError");
    ck(fs_live()<0, "timeout releases its live socket");

    run("var localData='pending',localBlob='pending',invalidSignal='pending';"
        "var localAbort=AbortSignal.abort(0);"
        "fetch('data:,ready',{signal:localAbort}).catch(function(e){localData=e;});"
        "var localURL=URL.createObjectURL(new Blob(['ready']));"
        "fetch(localURL,{signal:localAbort}).catch(function(e){localBlob=e;});"
        "try{fetch('/hold',{signal:{aborted:false}}).catch(function(e){invalidSignal=e.name;});}"
        "catch(e){invalidSignal='synchronous';}");
    settle(4);
    ckjs("localData===0 && localBlob===0", "pre-aborted data and Blob URLs preserve the reason");
    ckjs("invalidSignal==='TypeError'", "invalid fetch signal rejects a promise instead of throwing");
    run("URL.revokeObjectURL(localURL);");
    run("var maskedResult='pending',maskedData='pending',masked=AbortSignal.abort(false);"
        "Object.defineProperty(masked,'aborted',{value:false});"
        "Object.defineProperty(masked,'reason',{value:'forged'});"
        "fetch('/hold',{signal:masked}).catch(function(e){maskedResult=e;});"
        "fetch('data:,ready',{signal:masked}).catch(function(e){maskedData=e;});");
    settle(3);
    ckjs("maskedResult===false && maskedData===false", "Fetch reads signal state without invoking author properties");
    ck(fs_live()<0, "shadowed signal getters cannot reopen a cancelled transport");
    ckjs("(function(){var c=new AbortController(),saved=globalThis.Event;"
         "globalThis.Event=function(){throw new Error('author Event');};"
         "try{c.abort();return c.signal.aborted;}catch(e){return false;}"
         "finally{globalThis.Event=saved;}})()", "author Event replacement cannot interrupt internal abort dispatch");
}

int main(void)
{
    open_ctx("http://abort.example/page");
    fs_set_router(router);
    signal_checks();
    fetch_checks();
    close_ctx();
    printf("abort: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
