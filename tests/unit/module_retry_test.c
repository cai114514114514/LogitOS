/* SPDX-License-Identifier: MIT
 * Reuse the actual page/loader/watchdog apparatus. Only the response source
 * changes: a cached module has a failed transitive dependency, then a second
 * entry imports it. The old engine dereferences its null dependency at +0x82.
 */
#define main module_budget_unused_main
#define res_fetch module_budget_unused_fetch
#include "module_budget_test.c"
#undef res_fetch
#undef main

static int missing_requests;
int res_fetch(const char *url, unsigned char **out, int *len)
{
    const char *source;
    if (strstr(url, "/missing.js")) { missing_requests++; return -1; }
    if (strstr(url, "/broken.js")) source = "import './missing.js'; export const value=1;";
    else if (strstr(url, "/bad-cycle-a.js")) source = "import './bad-cycle-b.js'; import './missing.js'; export const value=1;";
    else if (strstr(url, "/bad-cycle-b.js")) source = "import './bad-cycle-a.js'; export const value=2;";
    else if (strstr(url, "/cycle-a.js")) source = "import {b} from './cycle-b.js'; export function a(){return 4}; export const total=b();";
    else if (strstr(url, "/cycle-b.js")) source = "import {a} from './cycle-a.js'; export function b(){return a()+3};";
    else source = "export const value=9;";
    *len = (int)strlen(source);
    *out = malloc((size_t)*len+1);
    memcpy(*out, source, (size_t)*len+1);
    return 0;
}

static int run_module(const char *source, const char *url)
{ return js_module_eval(source, (int)strlen(source), url); }

int main(void)
{
    setbuf(stdout, NULL);
    struct node *root = dom_parse("<html><body></body></html>", 26);
    if (!root || !js_page_open(root)) return 2;
    ck(!run_module("import './broken.js'; globalThis.brokenRan=true;", "https://module.test/entry1.js"),
       "first transitive missing dependency rejected");
    ck(missing_requests == 1, "first failure reached real module loader");
    JS_RunGC(JS_GetRuntime(js_page_ctx()));
    puts("MODULE-RETRY cached failed graph follows");
    ck(!run_module("import './broken.js'; globalThis.retryRan=true;", "https://module.test/entry2.js"),
       "cached failed dependency rejected without crash");
    ck(missing_requests == 1, "failed cached graph is not refetched as a successful module");
    ck(value("typeof brokenRan==='undefined' && typeof retryRan==='undefined'"),
       "neither failed module body executes");
    ck(run_module("import {value} from './healthy.js'; globalThis.healthy=value;", "https://module.test/entry3.js") && value("healthy===9"),
       "unrelated module still executes after failure");
    ck(run_module("import {total} from './cycle-a.js'; globalThis.cyclic=total;", "https://module.test/entry4.js") && value("cyclic===7"),
       "successful cyclic module graph still links");
    ck(!run_module("import './bad-cycle-a.js';", "https://module.test/entry5.js"),
       "failed cyclic graph rejected");
    ck(!run_module("import './bad-cycle-b.js';", "https://module.test/entry6.js"),
       "apparently resolved cycle member propagates ancestor failure");
    JS_RunGC(JS_GetRuntime(js_page_ctx()));
    ck(!run_module("import './bad-cycle-b.js';", "https://module.test/entry7.js"),
       "partly created cycle member retains failure on repeated entry");
    const char *dynamic = "globalThis.dynamicRejects=0; import('./broken.js').catch(function(e){if(e instanceof ReferenceError)dynamicRejects++});";
    JSValue v = JS_Eval(js_page_ctx(), dynamic, strlen(dynamic), "https://module.test/dynamic.js", JS_EVAL_TYPE_GLOBAL);
    ck(!JS_IsException(v), "dynamic import schedules after cached failure");
    JS_FreeValue(js_page_ctx(), v);
    JSContext *job_ctx; int jobs;
    do { jobs = JS_ExecutePendingJob(JS_GetRuntime(js_page_ctx()), &job_ctx); } while (jobs > 0);
    ck(value("dynamicRejects===1"), "dynamic import rejects with original failure");
    js_page_close(); dom_free(root);
    printf("module-retry: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
