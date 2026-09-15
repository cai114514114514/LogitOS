/* SPDX-License-Identifier: MIT */
#include "quickjs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void js_subtle_install(JSContext *);
static int eval(JSContext *ctx,const char *text,const char *name){
 JSValue v=JS_Eval(ctx,text,strlen(text),name,JS_EVAL_TYPE_GLOBAL);int bad=JS_IsException(v);JS_FreeValue(ctx,v);
 if(bad){JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);fprintf(stderr,"%s: %s\n",name,s?s:"exception");if(s)JS_FreeCString(ctx,s);JS_FreeValue(ctx,e);}return bad;
}
static int file(JSContext *ctx,const char *path){FILE *f=fopen(path,"rb");if(!f)return 1;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char *s=malloc((size_t)n+1);if(!s){fclose(f);return 1;}size_t got=fread(s,1,(size_t)n,f);fclose(f);s[got]=0;int rc=eval(ctx,s,path);free(s);return rc;}
int main(void){
 JSRuntime *rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);JS_SetMaxStackSize(rt,8*1024*1024);
 if(eval(ctx,"globalThis.crypto={};globalThis.DOMException=class extends Error{constructor(m,n){super(m);this.name=n}};", "setup"))return 2;
 if(file(ctx,"tests/fixtures/browser/key-host-base64.js"))return 2;
 js_subtle_install(ctx);
#ifdef WEB_ENTROPY_NEGCTL
 if(eval(ctx,"globalThis.cryptoKeyResults=[];crypto.subtle.generateKey({name:'AES-GCM',length:128},true,['encrypt']).then(()=>{cryptoKeyResults.push(['entropy refusal',false]);globalThis.cryptoKeyDone=true},e=>{cryptoKeyResults.push(['entropy refusal',e.name==='OperationError']);globalThis.cryptoKeyDone=true});","entropy control"))return 2;
#else
 if(file(ctx,"tests/fixtures/browser/key-vectors.js")||file(ctx,"tests/fixtures/browser/key-checks.js"))return 2;
#endif
 JSContext *job;int steps=0,rc;while((rc=JS_ExecutePendingJob(rt,&job))>0&&++steps<2000){}
 const char *expr="JSON.stringify({done:!!globalThis.cryptoKeyDone,checks:cryptoKeyResults.length,failed:cryptoKeyResults.filter(x=>!x[1])})";
 JSValue v=JS_Eval(ctx,expr,strlen(expr),"result",JS_EVAL_TYPE_GLOBAL);const char *result=JS_ToCString(ctx,v);printf("KEY_RESULTS %s\n",result?result:"{}");
 int ok=result&&strstr(result,"\"done\":true")&&strstr(result,"\"failed\":[]");
#ifndef WEB_ENTROPY_NEGCTL
 ok=ok&&strstr(result,"\"checks\":74,");
#endif
 if(result)JS_FreeCString(ctx,result);JS_FreeValue(ctx,v);JS_FreeContext(ctx);JS_FreeRuntime(rt);return ok?0:1;
}
