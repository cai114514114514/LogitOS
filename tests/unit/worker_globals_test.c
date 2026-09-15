/* SPDX-License-Identifier: MIT
 * Ordinary worker realm globals, exercised through the real Worker scheduler.
 * The in-memory loader supplies only our scripts; no external site runs here.
 * Parent objects are deliberately created before workers so a process-global
 * ClassID changed by a child installer cannot pass by testing only fresh URLs.
 */
#define main worker_existing_suite_main
#include "worker_test.c"
#undef main

static const char *GLOBALS_JS =
    "var out={};"
    "out.url=typeof URL;out.params=typeof URLSearchParams;"
    "out.encoder=typeof TextEncoder;out.decoder=typeof TextDecoder;"
    "try {var u=new URL('../lib.js?q=one+two','http://fixture.test/base/worker.js');"
    "out.resolved=u.href;out.query=u.searchParams.get('q');"
    "u.searchParams.set('q','x y');out.link=u.search;"
    "out.opaque=new URL('asset.svg','custom:/base/').href;"
    "importScripts(new URL('/lib.js',u).href);out.imported=LIBVAL;"
    "var p=new URLSearchParams([['a','1'],['a','2']]);out.entries=Array.from(p).length;"
    "}catch(e){out.urlError=e.name;}"
    "try {var enc=new TextEncoder(),dec=new TextDecoder();"
    "var bytes=enc.encode('A\\u4e2d\\ud83d\\ude00');out.roundtrip=dec.decode(bytes);"
    "out.first=dec.decode(new Uint8Array([0xe4,0xb8]),{stream:true});"
    "out.last=dec.decode(new Uint8Array([0xad]));"
    "out.latin=new TextDecoder('windows-1252').decode(new Uint8Array([0x80]));"
    "var dst=new Uint8Array(3);out.into=enc.encodeInto('\\u4e2d!',dst);"
    "}catch(e){out.encodingError=e.name;}"
    "out.noDocument=typeof document==='undefined';"
    "out.noFetch=typeof fetch==='undefined';"
    "out.noFalseStreams=typeof TransformStream==='function'||"
    "(typeof TextEncoderStream==='undefined'&&typeof TextDecoderStream==='undefined');"
    "postMessage(out);";

int main(void)
{
    fake_site_reset();
    fake_site_add("http://fixture.test/globals.js", GLOBALS_JS);
    fake_site_add("http://fixture.test/lib.js", LIB_JS);
    struct node *root = dom_parse(PAGE, (int)strlen(PAGE));
    if (!root) return 2;
    js_page_set_clock(clock_fn);
    js_page_set_location("http://fixture.test/page.html");
    if (!js_page_open(root)) return 2;
    ctx = js_page_ctx();
    run("var parentURL=new URL('http://fixture.test/parent?q=old'),"
        "parentParams=new URLSearchParams('a=keep'),parentEncoder=new TextEncoder(),"
        "parentFetch=fetch,parentLocation=location.href,results=[];"
        "function launch(){var w=new Worker('http://fixture.test/globals.js');"
        "w.onmessage=function(e){results.push(e.data);w.terminate();};w.onerror=function(){results.push({error:true});};}");
    for (int i = 0; i < 2; i++) {
        run("launch();");
        pump_until_idle(80);
    }
    ckjs("results.length===2", "two ordinary workers report and finish");
    ckjs("results.length===2&&results.every(r=>r.url==='function')", "workers expose URL");
    ckjs("results.length===2&&results.every(r=>r.params==='function')", "workers expose URLSearchParams");
    ckjs("results.length===2&&results.every(r=>r.encoder==='function')", "workers expose TextEncoder");
    ckjs("results.length===2&&results.every(r=>r.decoder==='function')", "workers expose TextDecoder");
    ckjs("results.length===2&&results.every(r=>r.resolved==='http://fixture.test/lib.js?q=one+two')", "worker URL resolves relative source");
    ckjs("results.length===2&&results.every(r=>r.query==='one two'&&r.link==='?q=x+y')", "worker URL and params share mutation");
    ckjs("results.length===2&&results.every(r=>r.opaque==='custom:/base/asset.svg')", "worker URL uses existing non-special parser");
    ckjs("results.length===2&&results.every(r=>r.imported===42)", "worker URL feeds ordinary importScripts");
    ckjs("results.length===2&&results.every(r=>r.entries===2)", "worker URLSearchParams iterator works");
    ckjs("results.length===2&&results.every(r=>r.roundtrip==='A\\u4e2d\\ud83d\\ude00')", "worker UTF-8 roundtrip preserves non-ASCII");
    ckjs("results.length===2&&results.every(r=>r.first===''&&r.last==='\\u4e2d')", "worker decoder retains partial UTF-8 between calls");
    ckjs("results.length===2&&results.every(r=>r.latin==='\\u20ac')", "worker decoder reuses legacy single-byte table");
    ckjs("results.length===2&&results.every(r=>r.into.read===1&&r.into.written===3)", "worker encodeInto respects complete character capacity");
    ckjs("results.length===2&&results.every(r=>r.noDocument&&r.noFetch&&r.noFalseStreams)", "pure installation publishes no document or false dependent streams");
    ckjs("parentURL.href==='http://fixture.test/parent?q=old'", "worker install preserves parent URL class");
    ckjs("parentParams.get('a')==='keep'", "worker install preserves parent params class");
    ckjs("(parentParams.append('b','new'),parentParams.toString()==='a=keep&b=new')", "parent params still mutate after worker teardown");
    ckjs("(parentURL.pathname='/after',parentURL.href==='http://fixture.test/after?q=old')", "parent URL still mutates after worker teardown");
    ckjs("parentEncoder.encode('ok').length===2&&fetch===parentFetch&&location.href===parentLocation", "worker pure installation preserves page functions and location");
    ck(js_worker_pending() == 0, "worker queue drains after parent termination");
    js_page_close();
    dom_free(root);
    printf("worker-globals: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
