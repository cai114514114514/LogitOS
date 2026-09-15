/* Actual parser-collected loader/evaluator and resource event delivery. The
 * previous dynamic-only test never exercised <script src onload> in markup. */
#define main loader_existing_main
#include "loader_test.c"
#undef main
void app_main(void);
static int polls,finished;
static const char *PAGE=
"<!doctype html><body><p id=state>WAITING</p><script>"
"window.executed=[];window.events=[];window.bubbled=0;window.destroyEvents=0;"
"function record(n,e){events.push(n+':'+e.type+':'+(e.target===e.currentTarget)+':'+e.bubbles);"
"if(n==='ok'){var s=document.createElement('script');s.src='/chain.js';document.head.appendChild(s)}}"
"document.addEventListener('load',function(e){if(e.target.tagName==='SCRIPT')bubbled++});"
"</script>"
"<script id=ok src='/ok.js' onload=\"record('ok',event)\"></script>"
"<script src='/empty.js' onload=\"record('empty',event)\"></script>"
"<script src='/missing.js' onerror=\"record('missing',event)\"></script>"
"<script src='/throws.js' onload=\"record('throws',event)\" onerror=\"record('wrong-throws',event)\"></script>"
"<script src='/html.js' onerror=\"record('html',event)\"></script>"
"<div id=holder><script src='/destroy.js' onload='destroyEvents++'></script></div>"
"<script>window.tailRan=true;document.body.appendChild(document.getElementById('ok'));"
"document.getElementById('state').textContent='PARSER-SCRIPTS-READY';console.log('PARSER-SCRIPTS-READY');</script>";
static int expr(const char *s){
 JSContext *ctx=js_page_ctx();if(!ctx)return 0;
 JSValue v=JS_Eval(ctx,s,strlen(s),"<parser-resource-observer>",JS_EVAL_TYPE_GLOBAL);int ok=0;
 if(JS_IsException(v))JS_FreeValue(ctx,JS_GetException(ctx));else ok=JS_ToBool(ctx,v);
 JS_FreeValue(ctx,v);return ok;
}
void loader_poll_hook(void){
 host_clock+=10;if(++polls<80||finished)return;finished=1;
 CHECK(expr("executed.indexOf('ok')>=0&&tailRan"),"parser source and following inline script execute");
 CHECK(expr("events.indexOf('ok:load:true:false')>=0"),"parser external success delivers exact nonbubbling load");
 CHECK(expr("events.indexOf('empty:load:true:false')>=0"),"parser empty 200 delivers load");
 CHECK(expr("events.indexOf('missing:error:true:false')>=0"),"parser HTTP failure delivers error");
 CHECK(expr("events.indexOf('throws:load:true:false')>=0&&!events.some(function(x){return x.indexOf('wrong-throws')===0})"),"parser runtime throw still delivers resource load");
 CHECK(expr("events.indexOf('html:error:true:false')>=0"),"parser refused HTML delivers error");
 CHECK(expr("executed.indexOf('chain')>=0"),"parser load handler starts dynamic continuation");
 CHECK(expr("executed.filter(function(x){return x==='ok'}).length===1&&events.filter(function(x){return x.indexOf('ok:')===0}).length===1"),"reinserted parser script neither executes nor signals twice");
 CHECK(expr("executed.indexOf('destroy')>=0&&destroyEvents===0&&document.getElementById('slotreuse')"),"destroyed parser node receives no event through recycled slot");
 CHECK(expr("bubbled===0"),"parser resource load does not bubble");
 struct logit_event e={0};e.type=EV_CLOSE;host_post_event(&e);
}
int main(void){
 fake_site_reset();fake_site_add("http://fixture.test/parser-resources.html",PAGE);
 fake_site_add("http://fixture.test/ok.js","executed.push('ok');");
 fake_site_add("http://fixture.test/empty.js","");
 fake_site_add("http://fixture.test/throws.js","executed.push('throws');throw Error('expected fixture throw');");
 fake_site_add("http://fixture.test/html.js","<!doctype html><p>not script</p>");
 fake_site_add("http://fixture.test/chain.js","executed.push('chain');");
 fake_site_add("http://fixture.test/destroy.js","executed.push('destroy');document.getElementById('holder').innerHTML='<span id=slotreuse>replacement</span>';");
 tabs_set_store(&memfs);const char *s="logit-browser-session\t1\t0\n0\thttp://fixture.test/parser-resources.html\tparser resources\t0\n";memfs_write(SESSION_PATH,s,strlen(s));
 struct logit_event e={0};e.type=EV_KEY;e.a='\n';host_post_event(&e);
 if(setjmp(host_exit_jmp)==0)app_main();
 CHECK(finished,"parser-resource real app_main reached observations");
 puts(fail?"parser-script-events: FAIL":"parser-script-events: PASS");return fail?1:0;
}
