/* SPDX-License-Identifier: MIT */
#include "js_download.h"
#include <string.h>
static js_download_handler download_handler;
void js_download_set_handler(js_download_handler handler){download_handler=handler;}
static JSValue queue_download(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)self;if(argc!=3||!download_handler)return JS_ThrowTypeError(ctx,"download service unavailable");
    const char *url=JS_ToCString(ctx,argv[0]),*name=JS_ToCString(ctx,argv[1]);
    if(!url||!name){if(url)JS_FreeCString(ctx,url);if(name)JS_FreeCString(ctx,name);return JS_EXCEPTION;}
    size_t n=0;uint8_t *bytes=NULL;int rc=-1;
    if(JS_IsNull(argv[2]))rc=download_handler(url,name,NULL,-1);
    else if((bytes=JS_GetArrayBuffer(ctx,&n,argv[2]))&&n<=64*1024*1024)rc=download_handler(url,name,bytes,(int)n);
    JS_FreeCString(ctx,url);JS_FreeCString(ctx,name);
    if(rc<0)return JS_ThrowTypeError(ctx,"download queue or size limit reached");
    return JS_UNDEFINED;
}
static const char source[]=
"(function(queue){'use strict';var G=globalThis,fetchBlob=G.fetch;\n"
"function anchor(el){\n"
" if(!el||!el.hasAttribute('download')||!el.hasAttribute('href'))return false;\n"
" var u;try{u=new G.URL(el.getAttribute('href'),el.baseURI||G.location.href)}catch(e){return false}\n"
" var name=el.getAttribute('download')||'';\n"
" if(u.protocol==='http:'||u.protocol==='https:'){\n"
"  if(u.origin!==G.location.origin)return false;queue(u.href,name,null);return true;\n"
" }\n"
" if(u.protocol!=='blob:'&&u.protocol!=='data:')return false;\n"
" if(u.protocol==='blob:'&&u.origin!==G.location.origin)return false;\n"
" /* Capture the Blob now: revokeObjectURL immediately after click must not\n"
"  * erase the bytes already accepted by this download. fetch's Blob path\n"
"  * captures its object before its first Promise callback. */\n"
" fetchBlob(u.href).then(function(r){if(!r.ok)throw new Error('download response failed');return r.arrayBuffer()})\n"
" .then(function(b){queue(u.href,name,b)}).catch(function(e){if(G.console)G.console.error('Download failed: '+e)});return true;\n"
"}\n"
"G.__logitDownloadAnchor=anchor;\n"
"[G.HTMLAnchorElement,G.HTMLAreaElement].forEach(function(C){if(!C)return;\n"
" Object.defineProperty(C.prototype,'download',{configurable:true,enumerable:true,get:function(){return this.getAttribute('download')||''},set:function(v){this.setAttribute('download',String(v))}});\n"
"});\n"
"})";
void js_download_install(JSContext *ctx)
{
    JSValue fn=JS_Eval(ctx,source,sizeof source-1,"<downloads>",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(fn)){JS_FreeValue(ctx,fn);return;}
    JSValue arg=JS_NewCFunction(ctx,queue_download,"download",3);
    JSValue result=JS_Call(ctx,fn,JS_UNDEFINED,1,&arg);
    JS_FreeValue(ctx,result);JS_FreeValue(ctx,arg);JS_FreeValue(ctx,fn);
}
