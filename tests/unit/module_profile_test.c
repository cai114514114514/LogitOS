/* SPDX-License-Identifier: MIT
 * Actual module parser/loader/evaluator, with the existing deterministic
 * transport and injected guest-clock oracle. The clock steps on reads so even
 * compilation has a nonzero observable interval; explicit IO/body advances
 * check attribution. These are accounting checks, not host performance data. */
#define main module_budget_original_main
#include "module_budget_test.c"
#undef main
static unsigned long long ticking_clock(void){return ++now;}
static unsigned long long total(const struct js_module_profile *p)
{return p->compile_ms+p->prefetch_ms+p->fetch_ms+p->link_ms+p->eval_jobs_ms;}
int main(void)
{
 struct node *root=dom_parse("<html><body></body></html>",26);if(!root)return 2;
 js_page_set_clock(ticking_clock);js_page_set_slice_ms(1000);if(!js_page_open(root))return 2;
 JSContext*c=js_page_ctx();JSValue g=JS_GetGlobalObject(c);
 JS_SetPropertyStr(c,g,"cpu",JS_NewCFunction(c,cpu,"cpu",1));JS_FreeValue(c,g);
 js_module_reset();prefetch_delay=77;fetch_delay=111;
 unsigned long long begin=now;
 ck(module("cpu(33);globalThis.answer=v;"),"measured module still evaluates normally");
 unsigned long long end=now;struct js_module_profile p;js_module_profile_get(&p);
 ck(value("answer===7"),"measuring never replaces the real module body");
 ck(p.prefetch_ms>=77&&p.prefetch_ms<90,"prefetch interval belongs to network wait");
 ck(p.fetch_ms>=111&&p.fetch_ms<125,"cache-miss interval belongs to fetch");
 ck(p.compile_ms>0&&p.compile_ms<25,"compile does not inherit child network waits");
 ck(p.eval_jobs_ms>=33&&p.eval_jobs_ms<50,"module body belongs to evaluation and jobs");
 ck(total(&p)<=end-begin&&total(&p)>=77+111+33,"nested loader partitions elapsed time without double counting");
 ck(p.compiles==2&&p.fetches==1&&p.prefetch_batches==1&&p.compile_bytes>30,"real root and dependency produce profile counts");
 now+=10000;struct js_module_profile idle;js_module_profile_get(&idle);
 ck(total(&idle)==total(&p),"idle time after module return is not charged");
 fetch_fail=1;ck(!module("globalThis.failedBody=true"),"failed dependency remains a real failure");
 js_module_profile_get(&p);now+=10000;js_module_profile_get(&idle);
 ck(total(&idle)==total(&p),"failed loader also releases profile ownership");
 fetch_fail=0;fetch_delay=0;prefetch_delay=0;
 ck(!js_module_eval("export ???",10,"https://module.test/bad-syntax.js"),"syntax failure stays visible");
 js_module_profile_get(&p);now+=10000;js_module_profile_get(&idle);
 ck(total(&idle)==total(&p),"failed compiler releases profile ownership");
 js_module_reset();js_module_profile_get(&p);
 ck(total(&p)==0&&p.compiles==0&&p.compile_bytes==0,"navigation reset clears all module accounting");
 js_page_close();dom_free(root);
 printf("module-profile: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
