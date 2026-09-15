/* SPDX-License-Identifier: MIT
 * Exercise the real loader, not a second script scheduler. No remote service
 * or verification algorithm is used: the widget is an ordinary local callback.
 * All bytes arrive in one loader batch, so ready async tasks run after the
 * blocking parser scripts and before the ordered deferred queue. */
#define main loader_existing_main
#include "loader_test.c"
#undef main
void app_main(void);
static int polls,finished;
static int expr(const char *s){
 JSContext *ctx=js_page_ctx();if(!ctx)return 0;
 JSValue v=JS_Eval(ctx,s,strlen(s),"<parser-order-observer>",JS_EVAL_TYPE_GLOBAL);
 int ok=0;if(JS_IsException(v))JS_FreeValue(ctx,JS_GetException(ctx));else ok=JS_ToBool(ctx,v);
 JS_FreeValue(ctx,v);return ok;
}
void loader_poll_hook(void){
 host_clock+=10;if(++polls<80||finished)return;finished=1;
 CHECK(expr("order.slice(0,4).join(',')==='head,blocking,inline,tail'"),"blocking parser scripts precede deferred and async batch tasks");
 CHECK(expr("callbackCount===1"),"async resource callback sees later parser declaration exactly once");
 CHECK(expr("order.indexOf('defer1')<order.indexOf('module')&&order.indexOf('module')<order.indexOf('defer2')"),"classic defer and nonasync modules share document order");
 CHECK(expr("order.indexOf('async')>order.indexOf('tail')&&order.indexOf('async')<order.indexOf('defer1')"),"async overrides defer in the ready batch");
 CHECK(expr("seen.blocking==='loading'&&seen.async==='interactive'&&seen.defer1==='interactive'&&seen.module==='interactive'"),"deferred scripts observe interactive document before DOMContentLoaded");
 CHECK(expr("order.indexOf('defer2')<order.indexOf('dcl')&&document.readyState==='complete'"),"DOMContentLoaded follows the deferred queue");
 CHECK(expr("changes.join(',')==='interactive,complete'"),"readiness changes once at each native lifecycle boundary");
 CHECK(expr("order.filter(function(x){return x==='async'||x==='defer1'||x==='defer2'||x==='module'}).length===4"),"prepared scripts execute once after attribute mutation");
 struct logit_event e={0};e.type=EV_CLOSE;host_post_event(&e);
}
int main(void){
 const char *page="<!doctype html><body><p>LOCAL SCRIPT ORDER</p>"
 "<script>var order=['head'],seen={},changes=[],callbackCount=0;"
 "document.addEventListener('readystatechange',function(){changes.push(document.readyState)});"
 "document.addEventListener('DOMContentLoaded',function(){order.push('dcl')});</script>"
 "<script id=a async defer src='/async.js' onload='widgetReady()'></script>"
 "<script id=d defer src='/defer1.js'></script>"
 "<script type=module>order.push('module');seen.module=document.readyState;</script>"
 "<script defer src='/defer2.js'></script>"
 "<script src='/blocking.js'></script>"
 "<script async defer>order.push('inline');</script>"
 "<script>order.push('tail');function widgetReady(){callbackCount++}"
 "document.getElementById('a').removeAttribute('async');"
 "document.getElementById('d').removeAttribute('defer');"
 "document.body.appendChild(document.getElementById('d'));</script>";
 fake_site_reset();fake_site_add("http://fixture.test/parser-order.html",page);
 fake_site_add("http://fixture.test/async.js","order.push('async');seen.async=document.readyState;");
 fake_site_add("http://fixture.test/defer1.js","order.push('defer1');seen.defer1=document.readyState;");
 fake_site_add("http://fixture.test/defer2.js","order.push('defer2');");
 fake_site_add("http://fixture.test/blocking.js","order.push('blocking');seen.blocking=document.readyState;");
 tabs_set_store(&memfs);const char *session="logit-browser-session\t1\t0\n0\thttp://fixture.test/parser-order.html\tparser order\t0\n";
 memfs_write(SESSION_PATH,session,strlen(session));struct logit_event e={0};e.type=EV_KEY;e.a='\n';host_post_event(&e);
 if(setjmp(host_exit_jmp)==0)app_main();
 CHECK(finished&&host_exited&&host_exit_code==0,"parser-order app_main completes normally");
 puts(fail?"parser-script-order: FAIL":"parser-script-order: PASS");return fail?1:0;
}
