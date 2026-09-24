/* Host test for c/apps/gui/browser/js/dom/frame.c -- a same-origin second browsing
 * context -- and the mutation surface js_domparser.c grew for it
 * (createElement/createTextNode/createComment/appendChild/insertBefore/
 * removeChild/setAttribute/removeAttribute/innerHTML=/getElementsByTagName).
 *
 *     make test-frame           the specimen, reduced to its mechanism, with
 *                                no hostname anywhere in it (see js_frame.c's
 *                                own header: this closes Cloudflare's
 *                                bootstrap frame, never anything that
 *                                branches on who is asking).
 *     make test-frame-negctl    the SAME file, linked with -DJS_FRAME_NO_EXEC
 *                                (js_frame.c's own control: the script sink
 *                                still fires, every DOM method above it is
 *                                still fully present and correct, only the
 *                                actual JS_Eval is compiled out) -- must FAIL,
 *                                specifically on the "script ran" assertion
 *                                and nothing upstream of it.
 *     make test-frame-lifecycle-negctl links the same test to a sed-built
 *                                js_frame.c where __frameRelease is a no-op;
 *                                the ninth sequential adoption must then hit
 *                                the 8-live-frame refusal.
 *     make test-frame-nodoc-negctl  a sed-built js_frame.c whose frame Window
 *                                lacks ONLY `document`: every "[doc]" check
 *                                must fail, no other, and the frame must print
 *                                the ReferenceError the openai.com probe saw.
 *     make test-frame-srcreflect-negctl  js_domparser.c without `src`
 *                                reflection: every "[src]" check must fail, no
 *                                other (the loader the bootstrap builds is then
 *                                dropped without a word -- watched, before the
 *                                reflection existed, on the first run of
 *                                scenario 7).
 *
 * SCENARIO 7 (2026-09-25) is the whole bootstrap as the probe measured it,
 * not only its first half: the frame's script uses `document`, builds a
 * <script> with `.src =`, and reports its Window (document, window/self/
 * frames, parent, top, frameElement, location) back through window.parent
 * for the creator to check. Scenario 1's expectation changed with it -- see
 * the comment there for why "document is undefined" was the wrong answer.
 *
 * THIS FILE DELIBERATELY NEVER TOUCHES js_platform.c. Per the build's own
 * allocation, js_platform.c (the contentDocument/contentWindow getters that
 * would call __frameAdopt from a real <iframe>) is owned by other live work
 * and was mid-edit as this was written -- see the report for the ~15-line
 * patch that wires it in, applied last and only once that file is idle.
 * What IS this file's ground -- js_domparser.c's new mutation surface and
 * js_frame.c itself -- is exercised directly: a bare JSContext, DOMParser
 * building a document by hand (standing in for what contentDocument would
 * hand back), __frameAdopt called directly (standing in for what
 * js_platform.c's settle() would call), and the exact DOM sequence
 * Cloudflare's own JS-detections bootstrap performs:
 *
 *   var e = document.createElement("iframe");
 *   function n(){ var n = e.contentDocument || e.contentWindow.document;
 *     if(n){ var t = n.createElement("script"); t.innerHTML = "...loader...";
 *            n.getElementsByTagName("head")[0].appendChild(t) } }
 *
 * reduced to: DOMParser stands in for contentDocument, __frameAdopt stands
 * in for the moment js_platform.c's settle() would have called it, and
 * everything from createElement("script") onward is VERBATIM the same
 * sequence against the SAME js_domparser.c API a real <iframe> would expose.
 *
 * THE ACCEPTANCE CRITERION: the frame script's own console.log line must
 * appear in what this process printed (js_frame.c's minimal console is the
 * only signal a script running in a DOM-less, second-realm context has to
 * report back with -- see js_frame.c's cut list, item 4; since 2026-09-25 a
 * frame script can also write to window.parent, which scenario 7 uses, and
 * the console line stays the signal the earlier scenarios use). Captured via a
 * real pipe, not a string this file builds by hand, so a change to
 * js_frame.c's actual printf format is what this test tracks. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "quickjs.h"

/* dom.c (HTML_PARSER_SRC) declares these as bare externs and relies on the
 * application to define them -- CLAUDE.md's own "two binaries that could not
 * be linked" section names this exact trap for c/lib/image; dom.c has the
 * same shape. */
void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

void js_domparser_install(JSContext *ctx);
void js_frame_install(JSContext *ctx);
void js_frame_close_all(void);
void js_frame_install_inert(JSContext *ctx);
void js_frame_close_context(JSContext *ctx);
void js_frame_refresh_policy(JSContext *ctx);

static int eval_bool(JSContext *ctx,const char *source)
{
    JSValue v=JS_Eval(ctx,source,strlen(source),"<realm-ownership>",JS_EVAL_TYPE_GLOBAL);
    int ok=!JS_IsException(v)&&JS_ToBool(ctx,v);
    if(JS_IsException(v)){
        JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);
        printf("realm check exception: %s\n",s?s:"?");
        if(s)JS_FreeCString(ctx,s);JS_FreeValue(ctx,e);
    }
    JS_FreeValue(ctx,v);return ok;
}

static int g_fail;
#define CHECK(cond, msg) do { \
    if (cond) printf("PASS: %s\n", msg); \
    else { printf("FAIL: %s\n", msg); g_fail++; } \
} while (0)

/* The specimen, reduced to its mechanism -- no hostname, no challenge
 * provider, nothing this engine could be accused of fitting to a site.
 * `__frameAdopt(doc)` stands in for js_platform.c's settle() calling it once
 * a frame's document is real and not cross-origin-blocked. */
static const char DRIVER_JS[] =
"(function () {\n"
"  var doc = new DOMParser().parseFromString('', 'text/html');\n"
"  if (!doc || !doc.documentElement) throw new Error('empty-string parse produced no document');\n"
"  __frameAdopt(doc);\n"
"  var heads = doc.getElementsByTagName('head');\n"
"  if (heads.length !== 1) throw new Error('expected exactly one head, got ' + heads.length);\n"
"  var t = doc.createElement('script');\n"
"  t.innerHTML = \"console.log('FRAME-RAN', typeof window, typeof self, typeof document); window.__ran = 1;\";\n"
"  heads[0].appendChild(t);\n"
"  return 'driver-ok';\n"
"})();\n";

/* A second scenario in the SAME process: a <script src=...> must be refused
 * BY NAME, never silently run and never silently dropped without saying so
 * (js_frame.c's cut list item 6). Uses a SEPARATE frame document -- the
 * table is keyed by dom_doc*, so this cannot cross-contaminate the first
 * scenario's result. */
static const char DRIVER_SRC_JS[] =
"(function () {\n"
"  var doc = new DOMParser().parseFromString('', 'text/html');\n"
"  __frameAdopt(doc);\n"
"  var t = doc.createElement('script');\n"
"  t.setAttribute('src', 'https://example.invalid/x.js');\n"
"  t.innerHTML = \"console.log('FRAME-SRC-SHOULD-NOT-RUN');\";\n"
"  doc.getElementsByTagName('head')[0].appendChild(t);\n"
"  return 'driver-src-ok';\n"
"})();\n";

/* A script inserted into a document NOBODY EVER ADOPTED -- the ordinary
 * `new DOMParser().parseFromString(...)` case every other caller in this
 * tree already relies on. Must stay INERT: this is the "an addition is
 * inert until js_frame.c opts a document in" claim in js_domparser.c's own
 * header, and it is the one regression that would silently turn every plain
 * DOMParser use in the browser into a script host. */
static const char DRIVER_UNADOPTED_JS[] =
"(function () {\n"
"  var doc = new DOMParser().parseFromString('', 'text/html');\n"
"  var t = doc.createElement('script');\n"
"  t.innerHTML = \"console.log('FRAME-UNADOPTED-SHOULD-NOT-RUN');\";\n"
"  doc.getElementsByTagName('head')[0].appendChild(t);\n"
"  return 'driver-unadopted-ok';\n"
"})();\n";

/* Scenario 4: A SCRIPT THAT WAS ALREADY IN THE FRAME'S MARKUP. The specimen
 * creates its script and inserts it, so the insertion sink sees it; a frame
 * whose script arrived in `srcdoc="..."` or in a same-origin response body is
 * inserted by NOBODY and no sink ever fires for it. That gap was invisible
 * from scenario 1 and is by far the more common shape on the open web, which
 * is why it gets its own scenario rather than a line in an existing one.
 * js_domparser_offer_scripts (called from __frameAdopt) is what closes it. */
static const char DRIVER_MARKUP_JS[] =
"(function () {\n"
"  var doc = new DOMParser().parseFromString(\n"
"    '<html><head><script>console.log(\"FRAME-MARKUP-RAN\");<\\/script></head><body></body></html>',\n"
"    'text/html');\n"
"  __frameAdopt(doc);\n"
"  return 'driver-markup-ok';\n"
"})();\n";

/* Scenario 5: textContent, and RUN-ONCE. This scenario is here because the
 * first version of it FAILED and the failure was right: it asserted that
 * `t.textContent = code` on an ALREADY-INSERTED empty <script> runs it, which
 * js_domparser.c's file header claimed and which no real browser does --
 * inserting the empty script already ran "prepare a script" and set
 * `already started`, so the later text is inert. The gate now pins the two
 * behaviours that are actually correct, in one capture so the ORDER of the
 * printed lines is part of the evidence:
 *
 *   A. DETACHED-FIRST WORKS -- set the text, THEN insert. This is the shape
 *      real loaders use and the shape the specimen uses (with innerHTML).
 *   B. AN ALREADY-HANDLED SCRIPT IS INERT -- neither a later textContent nor
 *      a later innerHTML on it runs anything, and in particular must not
 *      RE-RUN the text that already ran, which is what a missing
 *      NF_SCRIPT_DONE check produces and what presence-only assertions cannot
 *      distinguish. */
static const char DRIVER_TEXT_JS[] =
"(function () {\n"
"  var doc = new DOMParser().parseFromString('', 'text/html');\n"
"  __frameAdopt(doc);\n"
"  var head = doc.getElementsByTagName('head')[0];\n"
"  var t = doc.createElement('script');\n"
"  t.textContent = \"console.log('FRAME-TEXTCONTENT-RAN');\";\n"   /* A: text first ... */
"  head.appendChild(t);\n"                                          /*    ... then insert */
"  t.textContent = \"console.log('FRAME-TEXT-RERUN');\";\n"        /* B: both inert */
"  t.innerHTML   = \"console.log('FRAME-HTML-RERUN');\";\n"
"  return 'driver-text-ok';\n"
"})();\n";

/* Scenario 6: JSF_MAX_FRAMES is a simultaneous-live cap, not a lifetime
 * allocation counter. Twelve is deliberate: scenarios 1/2/4/5 above leave
 * four contexts live, so a build whose __frameRelease is a no-op has room for
 * exactly four of these and refuses the other eight. The ordinary build
 * releases every document immediately and prints all twelve markers. Counting
 * markers rather than grepping for the refusal is important: a diagnostic can
 * change wording while slot reuse is the behavioural contract. */
static const char DRIVER_LIFECYCLE_JS[] =
"(function () {\n"
"  var keep = [];\n"
"  for (var i = 0; i < 12; i++) {\n"
"    var doc = new DOMParser().parseFromString('', 'text/html');\n"
"    keep.push(doc);\n"
"    __frameAdopt(doc);\n"
"    var t = doc.createElement('script');\n"
"    t.textContent = \"console.log('FRAME-LIFECYCLE-RAN')\";\n"
"    doc.getElementsByTagName('head')[0].appendChild(t);\n"
"    __frameRelease(doc);\n"
"  }\n"
"  return 'driver-lifecycle-ok';\n"
"})();\n";

/* Scenario 7: THE WHOLE BOOTSTRAP SHAPE, as the probe measured it on
 * openai.com -- reduced to its mechanism, with no hostname and no vendor
 * path in it. The creator builds an <iframe> in ITS document, gets the
 * frame's document (DOMParser stands in for contentDocument, __frameAdopt
 * for js_platform.c's settle(), now with the Window inputs settle() passes:
 * the element and the committed URL), and inserts an inline script whose
 * text is the measured one's statement for statement:
 *
 *   window['<params>'] = {...};                  write to ITS OWN window
 *   var ..., s = document.createElement('script');   <- died here, 'document'
 *   s.nonce = ''; s.src = '<relative path>';                is not defined
 *   document.getElementsByTagName('head')[0].appendChild(s);
 *
 * and then reports what its Window is back to the creator through
 * `window.parent` -- the variable the creator reads afterwards. */
static const char DRIVER_BOOTSTRAP_JS[] =
"(function () {\n"
"  var host = new DOMParser().parseFromString('<!doctype html><html><head></head><body></body></html>', 'text/html');\n"
"  var e = host.createElement('iframe');\n"
"  host.body.appendChild(e);\n"
"  var n = new DOMParser().parseFromString('', 'text/html');\n"
"  __frameAdopt(n, e, 'about:blank');\n"
"  globalThis.bootFrame = e; globalThis.bootDoc = n;\n"
"  var t = n.createElement('script');\n"
"  t.nonce = '';\n"
"  t.innerHTML = \"window['__boot$params']={r:'r0',t:'t0'};\" +\n"
"    \"var now=Date.now()/1000,ts=''+Math.floor(now),_s=document.createElement('script');\" +\n"
"    \"_s.nonce='';_s.src='/boot-loader.js?ts='+ts;\" +\n"
"    \"document.getElementsByTagName('head')[0].appendChild(_s);\" +\n"
"    \"window.parent.bootReport={doc:document,win:window,self:self,frames:frames,glob:globalThis,\" +\n"
"    \"parent:window.parent,top:window.top,frameElement:frameElement,href:location.href,\" +\n"
"    \"docLoc:document.location,loc:location,url:document.URL};\" +\n"
"    \"console.log('FRAME-BOOT-RAN');\";\n"
"  n.getElementsByTagName('head')[0].appendChild(t);\n"
"  return 'driver-bootstrap-ok';\n"
"})();\n";

static char *slurp_and_restore(int savedout, int pfd[2])
{
    fflush(stdout);
    close(pfd[1]);
    dup2(savedout, STDOUT_FILENO);
    close(savedout);

    static char buf[16384];
    size_t off = 0;
    ssize_t n;
    while (off < sizeof buf - 1 && (n = read(pfd[0], buf + off, sizeof buf - 1 - off)) > 0)
        off += (size_t)n;
    buf[off] = 0;
    close(pfd[0]);
    return buf;
}

static const char *run_capturing(JSContext *ctx, const char *src, size_t len, const char *tag)
{
    int pfd[2];
    if (pipe(pfd) != 0) { perror("pipe"); exit(1); }
    fflush(stdout);
    int savedout = dup(STDOUT_FILENO);
    dup2(pfd[1], STDOUT_FILENO);

    JSValue r = JS_Eval(ctx, src, len, tag, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *out = slurp_and_restore(savedout, pfd);
        const char *m = JS_ToCString(ctx, e);
        printf("%s", out);
        printf("FAIL: %s threw: %s\n", tag, m ? m : "?");
        g_fail++;
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, r);
        return "";
    }
    JS_FreeValue(ctx, r);
    static char captured[16384];
    strncpy(captured, slurp_and_restore(savedout, pfd), sizeof captured - 1);
    captured[sizeof captured - 1] = 0;
    printf("%s", captured);   /* echo it into the real log too, for a human reading test output */
    return captured;
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_domparser_install(ctx);
    js_frame_install(ctx);

    /* Scenario 1: the specimen itself. THE SAME ASSERTION RUNS IN BOTH
     * BUILDS -- that is what makes -DJS_FRAME_NO_EXEC a real negative
     * control rather than a test that adapts to whichever binary it was
     * linked into. Under the plain build the script really runs and prints
     * this line; under JS_FRAME_NO_EXEC js_frame.c's own JS_Eval call is
     * compiled out, so the line can never appear -- this check must FAIL
     * there, and test-frame-negctl (tests/frame.mk) asserts exactly that. */
    const char *out1 = run_capturing(ctx, DRIVER_JS, sizeof DRIVER_JS - 1, "<driver>");
    /* CHANGED 2026-09-25, and the old expectation is kept here because it
     * was pinning the wrong answer: it read "window/self are objects,
     * document is undefined (no DOM in a frame context, by design)" and
     * matched "FRAME-RAN object object undefined". A same-origin about:blank
     * frame's script sees `typeof document === 'object'` in every browser
     * (Chromium measured, js_frame.c's frame_install_window table), and the
     * "by design" undefined is exactly what killed Cloudflare's bootstrap on
     * openai.com. The "[doc]" prefix marks every assertion that depends on
     * the frame's `document` binding -- test-frame-nodoc-negctl requires ALL
     * of them, and ONLY them, to fail when that one binding is removed. */
    CHECK(strstr(out1, "FRAME-RAN object object object") != NULL,
          "[doc] the specimen's script ran inside its OWN context: window, self and "
          "document are all objects (the frame's own document, as in a browser)");

    /* Scenario 2: <script src> is refused by name, not silently run and not
     * silently dropped. True in BOTH builds -- JS_FRAME_NO_EXEC only compiles
     * out the JS_Eval call, and the src check runs before that. */
    const char *out2 = run_capturing(ctx, DRIVER_SRC_JS, sizeof DRIVER_SRC_JS - 1, "<driver-src>");
    CHECK(strstr(out2, "FRAME-SRC-SHOULD-NOT-RUN") == NULL,
          "a <script src=...> must never execute (external frame scripts are not fetched)");
    CHECK(strstr(out2, "refused") != NULL && strstr(out2, "src=") != NULL,
          "the src refusal must be a NAMED line, not silence");

    /* Scenario 3: an ordinary, never-adopted DOMParser document. Every other
     * caller of `new DOMParser().parseFromString(...)` in this tree must see
     * NO behaviour change from this pass. */
    const char *out3 = run_capturing(ctx, DRIVER_UNADOPTED_JS, sizeof DRIVER_UNADOPTED_JS - 1, "<driver-unadopted>");
    CHECK(strstr(out3, "FRAME-UNADOPTED-SHOULD-NOT-RUN") == NULL,
          "a <script> in a document nobody ever __frameAdopt-ed must stay DATA, unchanged");

    /* Scenario 4: markup-borne scripts. FAILS under JS_FRAME_NO_EXEC too --
     * deliberately, and it is worth being explicit about why that is correct
     * rather than a second control by accident: the control's claim is "no
     * frame script runs at all", so every "a script ran" assertion in this
     * file must fail there. tests/frame.mk only requires the control to fail;
     * a control that failed on exactly one of two script-execution paths
     * would mean the OTHER path was still executing under NO_EXEC. */
    const char *out4 = run_capturing(ctx, DRIVER_MARKUP_JS, sizeof DRIVER_MARKUP_JS - 1, "<driver-markup>");
    CHECK(strstr(out4, "FRAME-MARKUP-RAN") != NULL,
          "a <script> already present in the frame's PARSED MARKUP runs on adopt "
          "(the srcdoc/response-body shape, which no insertion sink can see)");

    /* Scenario 5: detached-first, and the run-once flag. */
    const char *out5 = run_capturing(ctx, DRIVER_TEXT_JS, sizeof DRIVER_TEXT_JS - 1, "<driver-text>");
    CHECK(strstr(out5, "FRAME-TEXTCONTENT-RAN") != NULL,
          "textContent set on a DETACHED script and then inserted runs at the insert "
          "(the shape real loaders use; textContent is not itself a door)");
    CHECK(strstr(out5, "FRAME-TEXT-RERUN") == NULL && strstr(out5, "FRAME-HTML-RERUN") == NULL,
          "neither textContent= nor innerHTML= re-runs an already-handled <script>");
    /* The sharpest half, and the one a missing run-once flag fails: the FIRST
     * script must not be executed a second time by a later mutation. COUNTED,
     * not merely present -- presence cannot tell "ran once" from "ran twice",
     * and "ran twice" is precisely the bug NF_SCRIPT_DONE exists to stop. */
    {
        int n = 0;
        for (const char *p = out5; (p = strstr(p, "FRAME-TEXTCONTENT-RAN")) != NULL; p++) n++;
        CHECK(n == 1, "the inserted script ran EXACTLY once, not again on the next mutation "
                      "(counted, because presence cannot tell once from twice)");
    }

    /* Scenario 6: release and reuse. This is the assertion the lifecycle
     * negative control must watch failing against the former no-release
     * behaviour; it is intentionally independent of the NO_EXEC control,
     * which also makes it fail because no frame script runs at all. */
    const char *out6 = run_capturing(ctx, DRIVER_LIFECYCLE_JS,
                                     sizeof DRIVER_LIFECYCLE_JS - 1,
                                     "<driver-lifecycle>");
    {
        int n = 0;
        for (const char *p = out6; (p = strstr(p, "FRAME-LIFECYCLE-RAN")) != NULL; p++) n++;
        CHECK(n == 12, "12 sequential adopt/release cycles all run despite the 8-live-frame cap");
    }

    /* Scenario 7: the bootstrap, and the Window the frame's script ran in.
     * Checked from the CREATOR's side, because that is where the specimen
     * reads its answer back. "[doc]" marks exactly the assertions that
     * depend on the frame's `document` binding (see scenario 1). */
    const char *out7 = run_capturing(ctx, DRIVER_BOOTSTRAP_JS, sizeof DRIVER_BOOTSTRAP_JS - 1,
                                     "<driver-bootstrap>");
    CHECK(strstr(out7, "FRAME-BOOT-RAN") != NULL && eval_bool(ctx, "typeof bootReport==='object'"),
          "[doc] the bootstrap script ran to its end and set the variable its creator reads");
    CHECK(eval_bool(ctx, "bootReport.doc===bootDoc"),
          "[doc] the frame's `document` IS the document its creator holds (contentDocument identity)");
    CHECK(eval_bool(ctx, "bootDoc.getElementsByTagName('script').length===2 && "
                         "bootDoc.getElementsByTagName('script')[1].getAttribute('src').indexOf('/boot-loader.js')===0"),
          "[doc][src] the script the frame built with document.createElement landed in that same document");
    CHECK(strstr(out7, "refused: <script src=\"/boot-loader.js") != NULL,
          "[doc][src] ...with its .src= reflected, so the frame's script door refuses it by NAME (the next wall) instead of dropping it silently");
    CHECK(eval_bool(ctx, "var w=bootReport.win;w===bootReport.self&&w===bootReport.frames&&w===bootReport.glob"),
          "[doc] window === self === frames === the frame's global object");
    CHECK(eval_bool(ctx, "bootReport.parent===globalThis&&bootReport.top===globalThis"),
          "[doc] window.parent and window.top are the creator's window");
    CHECK(eval_bool(ctx, "bootReport.frameElement===bootFrame&&bootReport.frameElement.tagName==='IFRAME'"),
          "[doc] frameElement is the creator's <iframe> element");
    CHECK(eval_bool(ctx, "bootReport.href==='about:blank'&&bootReport.url==='about:blank'&&"
                         "bootReport.docLoc===bootReport.loc"),
          "[doc] location, document.URL and document.location reflect about:blank, one Location object");

    /* Window shape that does NOT depend on the document binding. These must
     * PASS under test-frame-nodoc-negctl too -- that is what makes the
     * control surgical rather than "the frame broke". */
    CHECK(eval_bool(ctx, "var fw=__frameGlobal(bootDoc);fw['__boot$params'].r==='r0'&&"
                         "typeof globalThis['__boot$params']==='undefined'"),
          "the creator reads what the frame wrote to ITS OWN window, and nothing leaked onto the creator's");
    CHECK(eval_bool(ctx, "var L=fw.location;L.href==='about:blank'&&String(L)==='about:blank'&&"
                         "L.protocol==='about:'&&L.pathname==='blank'&&L.host===''&&L.hostname===''&&"
                         "L.port===''&&L.search===''&&L.hash===''&&L.origin==='null'&&"
                         "Object.prototype.toString.call(L)==='[object Location]'"),
          "Location answers about:blank the way Chromium does (origin is the URL's: \"null\")");
    CHECK(eval_bool(ctx, "var d=function(o,k){var x=Object.getOwnPropertyDescriptor(o,k);"
                         "return x?(x.get?'g':'')+(x.set?'s':'')+(x.enumerable?'E':'')+(x.configurable?'C':''):'-';};"
                         "d(fw,'window')==='gE'&&d(fw,'top')==='gE'&&d(fw,'location')==='gsE'&&"
                         "d(fw,'self')==='gsEC'&&d(fw,'parent')==='gsEC'&&d(fw,'frameElement')==='gEC'&&"
                         "d(fw.location,'href')==='gsE'"),
          "own-property descriptors match Chromium's ([LegacyUnforgeable] ones are not configurable)");
    CHECK(eval_bool(ctx, "fw.cdoc=Object.getOwnPropertyDescriptor(fw,'document');"
                         "!!fw.cdoc&&typeof fw.cdoc.get==='function'&&fw.cdoc.configurable===false"),
          "[doc] `document` is an own, non-configurable accessor of the frame's window");
    CHECK(eval_bool(ctx, "fw.setTimeout===undefined&&fw.XMLHttpRequest===undefined&&"
                         "typeof fw.queueMicrotask==='function'&&"
                         "fw.Function.prototype.toString.call(fw.queueMicrotask).indexOf('[native code]')>0"),
          "a creator without a scheduler or XHR leaves those names ABSENT in the frame, not present-and-inert");
    CHECK(eval_bool(ctx, "fw.closed===false&&fw.opener===null&&fw.length===0&&fw.name===''"),
          "closed/opener/length/name are an unopened, unnamed, childless live frame's");
    CHECK(eval_bool(ctx, "bootDoc.location===fw.location&&bootDoc.URL==='about:blank'"),
          "the creator reads the frame document's location/URL as the frame does");
    CHECK(eval_bool(ctx, "fw.self=5;var ok=fw.self===5&&Object.getOwnPropertyDescriptor(fw,'self').value===5;"
                         "__frameRelease(bootDoc);ok&&fw.closed===true"),
          "[Replaceable] self is shadowed by assignment; closed turns true once the frame is released");

    js_frame_close_all();
    /* Independent network documents use different runtimes. Install in the
     * second one, then operate on the first again: singleton DP prototypes
     * and process-wide frame teardown both fail this order. */
    CHECK(eval_bool(ctx,"var pd=new DOMParser().parseFromString('','text/html');var pg=__frameGlobal(pd);pg.JSON!==JSON && pg.Function('return 9')()===9"),"creator exposes fresh working realm intrinsics");
    JSRuntime *child_rt=JS_NewRuntime();JSContext *child=JS_NewContext(child_rt);
    js_domparser_install(child);js_frame_install_inert(child);
    JS_SetStringCodeGenerationAllowed(child,0);
    CHECK(eval_bool(child,"var cd=new DOMParser().parseFromString('','text/html');var cg=__frameGlobal(cd);var denied=false;try{cg.Function('return 1')()}catch(e){denied=e.name==='EvalError'};denied&&cg.JSON.parse('[7]')[0]===7"),"auxiliary realm inherits denied code generation but parses JSON");
    CHECK(eval_bool(ctx,"var pn=pd.createElement('p');pn.textContent='parent';pd.body.appendChild(pn);pd.body.textContent==='parent' && __frameGlobal(pd)===pg"),"DOMParser prototypes and realm identity stay in creator runtime");
    CHECK(eval_bool(ctx,"var savedFunction=pg.Function;true"),"retain a constructor before policy tightens");
    JS_SetStringCodeGenerationAllowed(ctx,0);js_frame_refresh_policy(ctx);
    CHECK(eval_bool(ctx,"var denied=false;try{savedFunction('return 1')()}catch(e){denied=e.name==='EvalError'};denied"),"policy tightening reaches retained auxiliary constructors");
    js_frame_close_context(child);JS_FreeContext(child);JS_FreeRuntime(child_rt);
    CHECK(eval_bool(ctx,"__frameGlobal(pd)===pg && pg.JSON.parse('{\"ok\":true}').ok && pd.createElement('b').tagName==='B'"),"closing child runtime preserves parent auxiliary realm and prototypes");
    js_frame_close_context(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
