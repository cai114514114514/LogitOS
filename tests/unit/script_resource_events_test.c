/* Exercise app_main's real inserted-script queue, not a DOM dispatch shim.
 * A loader that waits for script.onload otherwise stalls despite the source
 * evaluating successfully; an eval-only test (loader part 2.7) missed this.
 * Fake bfetch supplies HTTP outcomes, and the guest fixture separately checks
 * the deployed binary and live HTTP. No site names or forced visibility. */
#define main loader_existing_main
#include "loader_test.c"
#undef main
void app_main(void);
static int polls,finished,edit_address,edit_posted;
static const char *PAGE=
"<!doctype html><body><div id=state>WAITING</div><script>"
"window.events=[];window.executed=[];window.inlineEvents=0;window.bubbled=0;"
"document.addEventListener('load',function(e){if(e.target.tagName==='SCRIPT')bubbled++});"
"function add(name,url){var s=document.createElement('script');s.src=url;"
"s.onload=function(e){events.push(name+':load:'+executed.indexOf(name)+':'+(e.target===s)+':'+(e.currentTarget===s)+':'+e.bubbles);"
"if(name==='ok')add('chain','/chain.js');};"
"s.onerror=function(e){events.push(name+':error:'+(e.target===s)+':'+(e.currentTarget===s)+':'+e.bubbles)};"
"document.head.appendChild(s);return s;}"
"setTimeout(function(){"
"window.okscript=add('ok','/ok.js');add('missing','/missing.js');add('empty','/empty.js');"
"add('throws','/throws.js');add('html','/html.js');"
"var s=document.createElement('script');s.textContent='executed.push(\"inline\")';"
"s.onload=s.onerror=function(){inlineEvents++};document.head.appendChild(s);"
"setTimeout(function(){document.head.appendChild(okscript)},20);"
"},200);</script>";
static int expr(const char *s){
 JSContext *ctx=js_page_ctx();if(!ctx)return 0;JSValue v=JS_Eval(ctx,s,strlen(s),"<script-resource-observer>",JS_EVAL_TYPE_GLOBAL);int r=0;
 if(JS_IsException(v)){JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);}else r=JS_ToBool(ctx,v);JS_FreeValue(ctx,v);return r;
}
void loader_poll_hook(void){
 host_clock+=10;
 if(edit_address&&!edit_posted&&js_page_live()) {
  /* Go through actual chrome input and leave it unsubmitted while the
   * page's later timer inserts relative scripts. No direct url mutation. */
  struct logit_event e={0};e.type=EV_KEY;e.a=12;e.mods=EV_MOD_CTRL;host_post_event(&e);
  e.a='x';e.mods=0;host_post_event(&e);edit_posted=1;
 }
 if(++polls<80||finished)return;
 finished=1;
 CHECK(expr("executed.indexOf('ok')>=0"),"success source really executed");
 CHECK(expr("events.indexOf('ok:load:0:true:true:false')>=0"),"success load observes execution and exact event target");
 CHECK(expr("events.indexOf('empty:load:-1:true:true:false')>=0"),"empty external script still fires load");
 CHECK(expr("events.some(function(s){return s.indexOf('throws:load:')===0})"),"runtime exception does not become a resource error");
 CHECK(expr("events.indexOf('missing:error:true:true:false')>=0"),"HTTP failure fires script error");
 CHECK(expr("events.indexOf('html:error:true:true:false')>=0"),"HTML body rejected as script fires resource error");
 CHECK(expr("executed.indexOf('chain')>=0&&events.some(function(s){return s.indexOf('chain:load:')===0})"),"load callback can insert and finish the next script");
 CHECK(expr("executed.indexOf('inline')>=0&&inlineEvents===0"),"inline classic executes without resource load event");
 CHECK(expr("events.filter(function(s){return s.indexOf('ok:')===0}).length===1"),"reinserting completed script never repeats load");
 CHECK(expr("bubbled===0"),"script resource events do not bubble");
 printf("script-resource requests=%d polls=%d\n",fake_site_requests(),polls);
 struct logit_event e={0};e.type=EV_CLOSE;host_post_event(&e);
}
int main(int argc,char **argv){
 edit_address=argc>1;
 fake_site_reset();fake_site_add("http://fixture.test/resources.html",PAGE);
 fake_site_add("http://fixture.test/ok.js","executed.push('ok');");
 fake_site_add("http://fixture.test/chain.js","executed.push('chain');");
 fake_site_add("http://fixture.test/empty.js","");
 fake_site_add("http://fixture.test/throws.js","executed.push('throws');throw new Error('fixture-runtime-error');");
 fake_site_add("http://fixture.test/html.js","<!doctype html><html><body>Not JavaScript</body></html>");
 tabs_set_store(&memfs);const char *s="logit-browser-session\t1\t0\n0\thttp://fixture.test/resources.html\tresources\t0\n";memfs_write(SESSION_PATH,s,strlen(s));
 struct logit_event e={0};e.type=EV_KEY;e.a='\n';host_post_event(&e);
 if(setjmp(host_exit_jmp)==0)app_main();
 CHECK(finished,"real app_main reached all resource event observations");
 puts(fail?"script-resource-events: FAIL":"script-resource-events: PASS");return fail?1:0;
}
