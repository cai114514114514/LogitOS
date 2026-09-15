/* SPDX-License-Identifier: MIT
 * Reuse the native transport/QuickJS harness, not a second XHR implementation.
 * The one-shot header listener is a common adapter shape. Independent progress
 * assertions intentionally still pass in the constructor-only negative build. */
#define main webapi_regular_main
#include "webapi_test.c"
#undef main
int main(void)
{
    fake_now=1000;fs_reset();open_ctx("http://page.example/dir/index.html");
    run("var names=['UNSENT','OPENED','HEADERS_RECEIVED','LOADING','DONE'];var x=new XMLHttpRequest();");
    ckjs("names.every(function(n,i){return XMLHttpRequest[n]===i})","constructor constants have standard values");
    ckjs("names.every(function(n,i){return XMLHttpRequest.prototype[n]===i})","prototype constants have standard values");
    ckjs("names.every(function(n,i){return x[n]===i})","instances inherit constants");
    ckjs("names.every(function(n){var d=Object.getOwnPropertyDescriptor(XMLHttpRequest,n);return d&&d.enumerable&&!d.writable&&!d.configurable})","constructor constants have WebIDL descriptors");
    ckjs("names.every(function(n){var d=Object.getOwnPropertyDescriptor(XMLHttpRequest.prototype,n);return d&&d.enumerable&&!d.writable&&!d.configurable})","prototype constants have WebIDL descriptors");
    ckjs("names.every(function(n){return !Object.prototype.hasOwnProperty.call(x,n)})","constants inherited without per-instance copies");
    run("var result={headers:0,progress:0,text:'',events:[],ct:'',status:0};"
        "x.open('GET','/hello');"
        "function headers(){if(x.readyState===x.HEADERS_RECEIVED){result.headers++;result.ct=x.getResponseHeader('content-type');result.events.push('headers')}x.removeEventListener('readystatechange',headers)}"
        "x.addEventListener('readystatechange',headers);"
        "x.addEventListener('progress',function(){result.progress++;result.text=x.responseText;result.events.push('progress')});"
        "x.addEventListener('loadend',function(){result.status=x.status;result.events.push('loadend')});x.send();");
    settle(80);
    ckjs("result.headers===1","one-shot instance-constant header hook fires once");
    ckjs("result.ct==='text/plain'","header hook can read response headers");
    ckjs("result.events[0]==='headers'&&result.events.indexOf('progress')>0","headers hook precedes body progress");
    ckjs("result.progress>0&&result.text==='hello world'","progress delivers body independently of header hook");
    ckjs("result.status===200&&result.events[result.events.length-1]==='loadend'","ordinary response completes independently of header hook");
    ck(!js_webapi_pending(),"all transport requests settle");
    /* A stream-capable client starts in streaming mode and switches after
     * Content-Type arrives. Missing the header hook can hide an HTTP-200 JSON
     * application response even though all its bytes are available. */
    run("var mode={stream:true,json:null,streamFeeds:0};var j=new XMLHttpRequest();j.open('GET','/json');"
        "function classify(){if(j.readyState===j.HEADERS_RECEIVED)mode.stream=(j.getResponseHeader('content-type')||'').indexOf('text/event-stream')>=0;j.removeEventListener('readystatechange',classify)}"
        "j.addEventListener('readystatechange',classify);"
        "j.addEventListener('progress',function(){if(mode.stream)mode.streamFeeds++});"
        "j.addEventListener('loadend',function(){if(mode.stream)return;mode.json=JSON.parse(j.responseText)});j.send();");
    settle(80);
    ckjs("mode.stream===false","headers classify ordinary JSON away from stream mode");
    ckjs("mode.json&&mode.json.a===1","HTTP 200 JSON reaches ordinary response classification");
    ckjs("mode.streamFeeds===0","ordinary JSON is not fed to an SSE-shaped callback");
    close_ctx();printf("xhr-constants: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
