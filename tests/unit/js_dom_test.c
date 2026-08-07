/* Host test for the JS<->DOM bindings (c/apps/browser/js_dom.c) and the page
 * runtime that keeps them alive (js_page.c).
 *
 * Part 1 parses HTML, runs a script that reads/writes the DOM via
 * document/Element, and checks the live DOM changed + the dirty flag fired.
 *
 * Part 2 is the live-page half: a runtime that OUTLIVES the script that created
 * it, three-phase event dispatch, default actions, and timers driven off an
 * injected clock. The clock is injected precisely so this test can assert timer
 * ORDER without sleeping -- "it fired eventually" is not the property that
 * matters, "a 10 ms timer fired before a 30 ms timer" is. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "js_page.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

static struct node *find(struct node *n, const char *tag)
{
    if (n->type == N_ELEM && !strcmp(n->tag, tag)) return n;
    for (struct node *c = n->first_child; c; c = c->next) { struct node *r = find(c, tag); if (r) return r; }
    return 0;
}
static const char *firsttext(struct node *n)
{ for (struct node *c = n->first_child; c; c = c->next) if (c->type == N_TEXT) return c->text; return ""; }

static int fails;
#define CK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails = 1; } else printf("ok: %s\n", m); } while (0)

static int run(JSContext *ctx, const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<t>", JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(v);
    if (!ok) { JSValue e = JS_GetException(ctx); const char *m = JS_ToCString(ctx, e);
               printf("  JS exception: %s\n", m ? m : "?"); if (m) JS_FreeCString(ctx, m); JS_FreeValue(ctx, e); }
    JS_FreeValue(ctx, v);
    return ok;
}

/* ---- part 2 scaffolding ---- */

/* A clock the test owns. Real time would make every ordering assertion below a
 * race; stepping it by hand makes them deterministic. */
static unsigned long long g_now;
static unsigned long long fake_clock(void) { return g_now; }

/* Evaluate in the page runtime (which drains the microtask queue for us). */
static int prun(const char *src) { return js_page_eval(src, (int)strlen(src), "<t>"); }

/* Advance the clock and service whatever came due. */
static int tick_to(unsigned long long t) { g_now = t; return js_page_run_due(); }

static struct js_event_init click_init(void)
{
    struct js_event_init i;
    memset(&i, 0, sizeof i);
    i.bubbles = 1; i.cancelable = 1; i.detail = 1;
    i.client_x = 40; i.client_y = 12;
    return i;
}

static void live_page_tests(void)
{
    /* Deliberately NOT the same document as part 1: opening a page runtime over
     * a DOM whose wrappers came from a runtime that is already gone is exactly
     * the failure this whole design is built to prevent, and it deserves its own
     * clean fixture rather than a reused one. */
    /* Each behaviour under test gets its OWN element. Listeners persist for the
     * whole page now -- that is the feature -- so sharing one element between
     * cases would mean case N's stopPropagation silently deciding case N+1. */
    const char *html =
        "<html><body id='b'>"
        "<div id='outer'><div id='mid'><button id='btn'>go</button></div></div>"
        "<a id='lnk' href='/next'>next</a>"
        "<p id='inl' onclick=\"this.textContent = 'attr-fired'\">x</p>"
        "<div id='sie'>sie</div>"
        "<div id='rem'>rem</div>"
        "<div id='doom'><span id='doomkid'>d</span></div>"
        "</body></html>";
    struct node *root = dom_parse(html, (int)strlen(html));
    CK(root != NULL, "part2: fixture parsed");
    if (!root) return;

    js_page_set_clock(fake_clock);
    g_now = 1000;
    CK(js_page_open(root), "page runtime opens");
    CK(js_page_live() && js_page_ctx() != NULL, "runtime + context are live");

    struct node *btn = dom_get_element_by_id(root->doc, "btn");
    struct node *lnk = dom_get_element_by_id(root->doc, "lnk");
    struct node *inl = dom_get_element_by_id(root->doc, "inl");
    struct node *sie = dom_get_element_by_id(root->doc, "sie");
    struct node *rem = dom_get_element_by_id(root->doc, "rem");
    CK(btn && lnk && inl && sie && rem, "fixture elements found");

    /* ---- 1. the runtime survives the script that created it ---- */
    CK(prun("window.log = [];"
            "function mk(tag){ return function(e){ log.push(tag + ':' + e.eventPhase); }; }"
            "document.getElementById('b').addEventListener('click', mk('b-cap'), true);"
            "document.getElementById('outer').addEventListener('click', mk('o-cap'), true);"
            "document.getElementById('btn').addEventListener('click', mk('t-cap'), true);"
            "document.getElementById('btn').addEventListener('click', mk('t-bub'), false);"
            "document.getElementById('outer').addEventListener('click', mk('o-bub'), false);"
            "document.getElementById('b').addEventListener('click', mk('b-bub'), false);"),
       "script registered six listeners and returned");
    CK(js_dom_listener_count() == 6, "six listeners live after the script ended");

    /* ---- 2. full three-phase order ---- */
    struct js_event_init ci = click_init();
    CK(js_dom_dispatch(btn, "click", &ci) == 1, "click with no preventDefault -> default proceeds");
    CK(prun("if (log.join('|') !== 'b-cap:1|o-cap:1|t-cap:2|t-bub:2|o-bub:3|b-bub:3')"
            "  throw 'order was ' + log.join('|');"),
       "capture -> target (both kinds) -> bubble, in registration order");

    /* ---- 3. the same listeners fire again ---- */
    ci = click_init();
    js_dom_dispatch(btn, "click", &ci);
    js_dom_dispatch(btn, "click", &ci);
    CK(prun("if (log.length !== 18) throw 'log.length ' + log.length;"),
       "listeners survive repeated delivery (3 x 6 calls)");

    /* ---- 4. event properties a real page reads ---- */
    CK(prun("window.seen = null;"
            "document.getElementById('btn').addEventListener('mousedown', function(e){"
            "  seen = { t: e.type, id: e.target.id, cur: e.currentTarget.id,"
            "           b: e.bubbles, c: e.cancelable, x: e.clientX, y: e.clientY,"
            "           btn: e.button, sh: e.shiftKey, tr: e.isTrusted,"
            "           inst: (e instanceof MouseEvent) && (e instanceof Event) };"
            "});"), "mousedown listener registered");
    {
        struct js_event_init mi = click_init();
        mi.button = 2; mi.shift = 1;
        js_dom_dispatch(btn, "mousedown", &mi);
    }
    CK(prun("if (seen.t !== 'mousedown') throw 'type ' + seen.t;"
            "if (seen.id !== 'btn' || seen.cur !== 'btn') throw 'target/currentTarget';"
            "if (!seen.b || !seen.c) throw 'bubbles/cancelable';"
            "if (seen.x !== 40 || seen.y !== 12) throw 'clientX/Y ' + seen.x + ',' + seen.y;"
            "if (seen.btn !== 2 || !seen.sh) throw 'button/shiftKey';"
            "if (!seen.tr) throw 'isTrusted';"
            "if (!seen.inst) throw 'prototype chain';"),
       "MouseEvent carries type/target/currentTarget/clientX/button/shiftKey/isTrusted");

    /* ---- 5. KeyboardEvent ---- */
    CK(prun("window.k = null;"
            "document.getElementById('b').addEventListener('keydown', function(e){"
            "  k = e.key + '/' + e.code + '/' + e.ctrlKey + '/' + (e instanceof KeyboardEvent); });"),
       "keydown listener registered");
    {
        struct js_event_init ki;
        memset(&ki, 0, sizeof ki);
        ki.bubbles = 1; ki.cancelable = 1; ki.key = "ArrowDown"; ki.code = "ArrowDown"; ki.ctrl = 1;
        js_dom_dispatch(btn, "keydown", &ki);
    }
    CK(prun("if (k !== 'ArrowDown/ArrowDown/true/true') throw 'got ' + k;"),
       "KeyboardEvent carries key/code/ctrlKey and its prototype");

    /* ---- 6. stopPropagation truncates the walk ---- */
    CK(prun("window.log = [];"
            "window.stopper = function(e){ log.push('mid'); e.stopPropagation(); };"
            "document.getElementById('mid').addEventListener('click', stopper, true);"),
       "capture listener on mid calls stopPropagation");
    ci = click_init();
    js_dom_dispatch(btn, "click", &ci);
    CK(prun("if (log.join('|') !== 'b-cap:1|o-cap:1|mid')"
            "  throw 'got ' + log.join('|');"),
       "stopPropagation in capture stops target and bubble, current node still completes");
    /* Take it back off -- and prove removeEventListener honours the capture
     * flag, since a non-capture remove must NOT match a capture registration. */
    prun("document.getElementById('mid').removeEventListener('click', stopper, false);");
    ci = click_init();
    js_dom_dispatch(btn, "click", &ci);
    CK(prun("if (log[log.length - 1] !== 'mid') throw 'capture flag ignored on remove';"),
       "removeEventListener with the wrong capture flag does not match");
    prun("document.getElementById('mid').removeEventListener('click', stopper, true);");
    ci = click_init();
    js_dom_dispatch(btn, "click", &ci);
    CK(prun("if (log[log.length - 1] !== 'b-bub:3') throw 'stopper still there: ' + log.join('|');"),
       "removeEventListener with the matching capture flag removes it");

    /* ---- 7. stopImmediatePropagation stops the rest of THIS node too ---- */
    CK(prun("window.si = [];"
            "var el = document.getElementById('sie');"
            "el.addEventListener('click', function(e){ si.push(1); e.stopImmediatePropagation(); });"
            "el.addEventListener('click', function(e){ si.push(2); });"
            "document.getElementById('b').addEventListener('click', function(){ si.push(3); });"),
       "two listeners on #sie, one more on body");
    ci = click_init();
    js_dom_dispatch(sie, "click", &ci);
    CK(prun("if (si.join('|') !== '1') throw 'got ' + si.join('|');"),
       "stopImmediatePropagation suppresses the sibling listener and the bubble");

    /* ---- 8. preventDefault is what makes the default action optional ----
     * This is the property the browser's link navigation now depends on. */
    CK(prun("document.getElementById('lnk').addEventListener('click', function(e){"
            "  e.preventDefault(); });"), "link listener calls preventDefault");
    ci = click_init();
    CK(js_dom_dispatch(lnk, "click", &ci) == 0, "preventDefault -> dispatch reports 'do not navigate'");
    CK(prun("window.pd = null;"
            "document.getElementById('lnk').addEventListener('click', function(e){ pd = e.defaultPrevented; });"),
       "listener observes defaultPrevented");
    ci = click_init();
    js_dom_dispatch(lnk, "click", &ci);
    CK(prun("if (pd !== true) throw 'defaultPrevented was ' + pd;"),
       "defaultPrevented is visible to later listeners");
    /* A non-cancelable event cannot be prevented, no matter what a handler does. */
    {
        struct js_event_init ni = click_init();
        ni.cancelable = 0;
        CK(js_dom_dispatch(lnk, "click", &ni) == 1, "preventDefault on a non-cancelable event is a no-op");
    }

    /* ---- 9. removeEventListener ---- */
    CK(prun("window.rlog = 0;"
            "window.h = function(){ rlog++; };"
            "window.rel = document.getElementById('rem');"
            "rel.addEventListener('ping', h);"), "listener added to #rem");
    {
        struct node *el = rem;
        struct js_event_init pi;
        memset(&pi, 0, sizeof pi); pi.bubbles = 1;
        js_dom_dispatch(el, "ping", &pi);
        CK(prun("if (rlog !== 1) throw 'rlog ' + rlog;"), "listener fired once");
        prun("rel.removeEventListener('ping', h);");
        js_dom_dispatch(el, "ping", &pi);
        CK(prun("if (rlog !== 1) throw 'rlog after remove ' + rlog;"),
           "removeEventListener stops delivery");
        /* Adding the identical (type, fn, capture) triple twice must register once. */
        prun("rel.addEventListener('ping', h); rel.addEventListener('ping', h);");
        js_dom_dispatch(el, "ping", &pi);
        CK(prun("if (rlog !== 2) throw 'duplicate add fired ' + (rlog - 1) + ' times';"),
           "addEventListener is idempotent for an identical triple");
        /* { once: true } removes itself before it runs. */
        prun("window.olog = 0; rel.addEventListener('once-ev', function(){ olog++; }, { once: true });");
        struct js_event_init oi;
        memset(&oi, 0, sizeof oi);
        js_dom_dispatch(el, "once-ev", &oi);
        js_dom_dispatch(el, "once-ev", &oi);
        CK(prun("if (olog !== 1) throw 'once fired ' + olog + ' times';"),
           "{ once: true } fires exactly once");
        /* { capture: true } is decoded from the options dictionary too. */
        prun("window.clog = '';"
             "document.body.addEventListener('capt', function(e){ clog += e.eventPhase; }, { capture: true });");
        struct js_event_init pi2;
        memset(&pi2, 0, sizeof pi2); pi2.bubbles = 1;
        js_dom_dispatch(el, "capt", &pi2);
        CK(prun("if (clog !== '1') throw 'options-capture phase ' + clog;"),
           "addEventListener options dictionary decodes { capture: true }");
    }

    /* ---- 10. on* handler properties ---- */
    CK(prun("window.on1 = 0; window.on2 = 0;"
            "var b = document.getElementById('btn');"
            "b.onclick = function(){ on1++; };"
            "b.onclick = function(){ on2++; };"
            "if (typeof b.onclick !== 'function') throw 'onclick not readable';"),
       "onclick assigned twice");
    ci = click_init();
    js_dom_dispatch(btn, "click", &ci);
    CK(prun("if (on1 !== 0 || on2 !== 1) throw 'on1=' + on1 + ' on2=' + on2;"),
       "the second on* assignment REPLACES the first");
    CK(prun("document.getElementById('btn').onclick = null;"), "onclick = null accepted");
    ci = click_init();
    js_dom_dispatch(btn, "click", &ci);
    CK(prun("if (on2 !== 1) throw 'on2 after null ' + on2;"), "onclick = null unregisters");

    /* An on<type>= content attribute is compiled on first dispatch. */
    ci = click_init();
    js_dom_dispatch(inl, "click", &ci);
    CK(prun("if (document.getElementById('inl').textContent !== 'attr-fired')"
            "  throw 'inline handler: ' + document.getElementById('inl').textContent;"),
       "inline onclick= attribute runs with `this` bound to the element");

    /* ---- 11. dispatchEvent + constructors from script ---- */
    CK(prun("window.dlog = '';"
            "document.getElementById('btn').addEventListener('synth', function(e){"
            "  dlog = e.type + ':' + e.isTrusted + ':' + e.detail; });"
            "var ok = document.getElementById('btn').dispatchEvent("
            "  new MouseEvent('synth', { bubbles: true, cancelable: true, detail: 7 }));"
            "if (ok !== true) throw 'dispatchEvent returned ' + ok;"
            "if (dlog !== 'synth:false:7') throw 'got ' + dlog;"),
       "dispatchEvent(new MouseEvent(...)) works and is not trusted");
    CK(prun("document.getElementById('btn').addEventListener('cancelme', function(e){ e.preventDefault(); });"
            "var r = document.getElementById('btn').dispatchEvent("
            "  new Event('cancelme', { cancelable: true }));"
            "if (r !== false) throw 'dispatchEvent should report false when prevented';"),
       "dispatchEvent returns false when a listener prevents the default");

    /* window listeners see bubbled events (they hang off the document root). */
    CK(prun("window.wlog = 0;"
            "window.addEventListener('click', function(){ wlog++; });"), "window.addEventListener");
    ci = click_init();
    js_dom_dispatch(btn, "click", &ci);
    CK(prun("if (wlog !== 1) throw 'window listener fired ' + wlog;"),
       "a window listener receives the bubbled click");

    /* ---- 12. a listener whose node is destroyed must not be callable ----
     * This is the second dangerous shape in the brief: a listener holding a node
     * that gets freed. The DOM recycles slots through a free list, so the node
     * POINTER can come back meaning a different element; the {node, serial}
     * handle is what makes that detectable, and the bucket is dropped instead. */
    {
        int before = js_dom_listener_count();
        CK(prun("window.doomed = document.getElementById('doom');"
                "doomed.addEventListener('gone', function(){ throw 'must never run'; });"
                "document.getElementById('doomkid')"
                "        .addEventListener('gone', function(){ throw 'must never run'; });"),
           "two listeners attached to a doomed subtree");
        CK(js_dom_listener_count() == before + 2, "both listeners counted");
        prun("document.body.removeChild(doomed);");
        CK(prun("if (doomed.textContent !== undefined)"
                "  throw 'stale wrapper still resolves: ' + doomed.textContent;"),
           "a wrapper into a destroyed subtree goes stale instead of dangling");
        /* Any dispatch prunes recycled nodes; the handlers go with their nodes. */
        struct js_event_init gi = click_init();
        js_dom_dispatch(dom_doc_body(root->doc), "click", &gi);
        CK(js_dom_listener_count() == before,
           "listeners on a destroyed subtree are reclaimed, not left callable");
        /* And dispatching AT the dead wrapper must be a harmless no-op. */
        CK(prun("if (doomed.dispatchEvent(new Event('gone')) !== false)"
                "  throw 'dispatch on a dead node should report false';"),
           "dispatchEvent on a destroyed node is a no-op, not a crash");
    }

    /* ---- 13. promises ---- */
    CK(prun("window.plog = [];"
            "Promise.resolve(1).then(function(v){ plog.push('a' + v); return 2; })"
            "                  .then(function(v){ plog.push('b' + v); });"),
       "promise chain evaluated");
    CK(prun("if (plog.join('|') !== 'a1|b2') throw 'promise chain: ' + plog.join('|');"),
       "the microtask queue is pumped after a script -- a promise chain resolves");

    CK(prun("window.aw = 'pending';"
            "(async function(){ aw = await Promise.resolve('resolved'); })();"),
       "async function started");
    CK(prun("if (aw !== 'resolved') throw 'await stalled: ' + aw;"),
       "await completes (the job queue drained past the suspension point)");

    /* A promise resolved inside an event callback must settle before the
     * dispatch returns -- that is the microtask checkpoint. */
    CK(prun("window.pev = 'no';"
            "document.getElementById('btn').addEventListener('pp', function(){"
            "  Promise.resolve().then(function(){ pev = 'yes'; }); });"),
       "handler that queues a microtask");
    {
        struct js_event_init pi;
        memset(&pi, 0, sizeof pi);
        js_dom_dispatch(btn, "pp", &pi);
    }
    CK(prun("if (pev !== 'yes') throw 'microtask after callback: ' + pev;"),
       "microtasks queued by an event callback run before dispatch returns");

    /* ---- 14. timers ---- */
    CK(!js_page_pending(), "no timers pending before any are set");
    CK(prun("window.tlog = [];"
            "setTimeout(function(){ tlog.push('c'); }, 30);"
            "setTimeout(function(){ tlog.push('a'); }, 10);"
            "window.dead = setTimeout(function(){ tlog.push('never'); }, 20);"
            "setTimeout(function(){ tlog.push('b'); }, 10);"
            "clearTimeout(dead);"),
       "four setTimeouts, one cleared");
    CK(js_page_pending(), "timers pending");
    CK(js_page_next_due() == (long long)(g_now + 10), "next deadline is the earliest timer");
    CK(tick_to(1005) == 0, "nothing fires before its deadline");
    CK(tick_to(1010) == 2, "both 10 ms timers fire on the same tick");
    CK(prun("if (tlog.join('|') !== 'a|b') throw 'order ' + tlog.join('|');"),
       "equal deadlines fire in registration order");
    CK(tick_to(1029) == 0, "the 30 ms timer has not come due");
    CK(tick_to(1030) == 1, "the 30 ms timer fires last");
    CK(prun("if (tlog.join('|') !== 'a|b|c') throw 'final ' + tlog.join('|');"),
       "timers fire in deadline order");
    CK(prun("if (tlog.indexOf('never') >= 0) throw 'clearTimeout did not cancel';"),
       "clearTimeout cancels before firing");
    CK(!js_page_pending(), "queue empty once every timer has run");

    /* setInterval repeats until it clears itself. */
    CK(prun("window.n = 0;"
            "window.iv = setInterval(function(){ n++; if (n >= 3) clearInterval(iv); }, 10);"),
       "setInterval registered");
    tick_to(1040); CK(prun("if (n !== 1) throw 'n=' + n;"), "interval tick 1");
    tick_to(1050); CK(prun("if (n !== 2) throw 'n=' + n;"), "interval tick 2");
    tick_to(1060); CK(prun("if (n !== 3) throw 'n=' + n;"), "interval tick 3");
    tick_to(1200); CK(prun("if (n !== 3) throw 'interval kept firing: ' + n;"),
                      "clearInterval from inside the callback stops it");
    CK(!js_page_pending(), "no timers left after clearInterval");

    /* A timer scheduled from a timer waits for the next pass -- otherwise a
     * setTimeout(f, 0) loop would never give the paint loop a turn. */
    CK(prun("window.chain = 0;"
            "function step(){ chain++; if (chain < 3) setTimeout(step, 0); }"
            "setTimeout(step, 0);"), "self-rescheduling timer");
    tick_to(1200); CK(prun("if (chain !== 1) throw 'chain ' + chain;"), "one step per pass");
    tick_to(1200); CK(prun("if (chain !== 2) throw 'chain ' + chain;"), "second pass, second step");
    tick_to(1200); CK(prun("if (chain !== 3) throw 'chain ' + chain;"), "third pass, chain ends");
    CK(!js_page_pending(), "the chain terminated");

    /* await across a real timer: the whole point of a runtime that survives. */
    CK(prun("window.tv = 'no';"
            "function delay(ms){ return new Promise(function(r){ setTimeout(r, ms); }); }"
            "(async function(){ await delay(20); tv = 'done'; })();"),
       "async function awaiting a timer");
    CK(prun("if (tv !== 'no') throw 'resolved too early';"), "await is still suspended");
    tick_to(1220);
    CK(prun("if (tv !== 'done') throw 'await never resumed: ' + tv;"),
       "a timer resolving a promise resumes the awaiting function");

    /* requestAnimationFrame gets a timestamp and its own handle space. */
    CK(prun("window.raf1 = -1;"
            "requestAnimationFrame(function(ts){ raf1 = ts; });"
            "window.rc = requestAnimationFrame(function(){ raf1 = -99; });"
            "cancelAnimationFrame(rc);"), "rAF scheduled, second one cancelled");
    tick_to(1240);
    CK(prun("if (raf1 < 0) throw 'rAF never ran (' + raf1 + ')';"), "rAF callback ran");
    CK(prun("if (raf1 !== 240) throw 'rAF timestamp ' + raf1;"),
       "rAF timestamp is ms since the page opened");
    CK(!js_page_pending(), "cancelAnimationFrame removed the second frame callback");

    /* performance.now() rides the same clock. */
    CK(prun("if (performance.now() !== 240) throw 'performance.now ' + performance.now();"),
       "performance.now() is monotonic ms since page open");

    /* ---- 15. DOM mutation from a handler sets the dirty flag ---- */
    js_dom_clear_dirty();
    CK(prun("document.getElementById('btn').addEventListener('paint', function(){"
            "  document.getElementById('btn').textContent = 'mutated'; });"),
       "handler that mutates the DOM");
    CK(!js_dom_dirty(), "registering a listener does not dirty the DOM");
    {
        struct js_event_init pi;
        memset(&pi, 0, sizeof pi);
        js_dom_dispatch(btn, "paint", &pi);
    }
    CK(js_dom_dirty(), "a mutating handler dirties the DOM so the browser re-lays-out");

    /* ---- 16. teardown, then re-open over the SAME DOM ----
     * The trap this replaces: a wrapper slot surviving into a runtime that no
     * longer exists. If js_page_close() misses one, the getElementById below
     * hands a dangling JSObject* straight back to JS. */
    js_page_close();
    CK(!js_page_live(), "runtime closed");
    CK(js_dom_listener_count() == 0, "every listener released at teardown");
    CK(!js_page_pending(), "every timer released at teardown");
    CK(js_page_open(root), "runtime re-opened over the same DOM");
    CK(prun("if (document.getElementById('btn').textContent !== 'mutated')"
            "  throw 'lost the DOM across a runtime swap';"),
       "the DOM survived the runtime swap and the new wrappers are fresh");
    CK(prun("window.again = 0;"
            "document.getElementById('btn').addEventListener('click', function(){ again++; });"),
       "listener registered in the second runtime");
    ci = click_init();
    js_dom_dispatch(btn, "click", &ci);
    CK(prun("if (again !== 1) throw 'again ' + again;"), "the second runtime dispatches too");
    js_page_close();

    dom_free(root);
}

int main(void)
{
    const char *html = "<body><h1 id='t'>Old</h1><p class='x'>hi</p><a href='/y'>z</a>"
                       "<div id='ih'><b>bold</b> and <i>it</i></div>"
                       "<table><tr id='trc'><td>seed</td></tr></table></body>";
    struct node *root = dom_parse(html, (int)strlen(html));
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_dom_init(ctx, root);

    CK(run(ctx, "document.getElementById('t').textContent = 'New' + document.querySelector('.x').tagName;"),
       "eval getElementById/querySelector/textContent=");
    CK(js_dom_dirty(), "DOM marked dirty after textContent set");
    struct node *h1 = find(root, "h1");
    CK(h1 && !strcmp(firsttext(h1), "Newp"), "h1 textContent updated to 'Newp'");

    js_dom_clear_dirty();
    CK(run(ctx, "var a = document.querySelector('a'); a.setAttribute('href','/changed'); a.getAttribute('href');"),
       "eval setAttribute/getAttribute");
    CK(js_dom_dirty(), "DOM dirty after setAttribute");
    struct node *a = find(root, "a");
    CK(a && !strcmp(dom_attr(a, "href") ? dom_attr(a, "href") : "", "/changed"), "a href updated to /changed");

    CK(run(ctx, "if (document.getElementById('nope') !== null) throw 'should be null';"),
       "getElementById(missing) returns null");

    /* createElement + appendChild: a new node enters the live DOM and is findable */
    js_dom_clear_dirty();
    CK(run(ctx, "var d = document.createElement('DIV');"
                "d.setAttribute('id', 'new'); d.textContent = 'made';"
                "document.body.appendChild(d);"),
       "eval createElement/setAttribute/appendChild");
    CK(js_dom_dirty(), "DOM dirty after appendChild");
    CK(run(ctx, "if (document.getElementById('new').textContent !== 'made') throw 'not in DOM';"),
       "appended element reachable via getElementById");
    /* By id, not find(root,"div"): the fixture has its own <div> for the
     * innerHTML cases below, and a first-match-by-tag walk would grab that one. */
    struct node *dv = dom_get_element_by_id(root->doc, "new");
    CK(dv && !strcmp(dv->tag, "div") && !strcmp(firsttext(dv), "made") &&
       dv->parent == dom_doc_body(root->doc), "div node really linked under body");

    /* appendChild moves (reparents) an existing node */
    CK(run(ctx, "document.querySelector('h1').appendChild(document.querySelector('a'));"),
       "eval appendChild reparent");
    struct node *h1b = find(root, "h1");
    CK(h1b && find(h1b, "a"), "a moved under h1");

    /* removeChild drops the subtree from the DOM */
    js_dom_clear_dirty();
    CK(run(ctx, "var b = document.body, p = document.querySelector('.x');"
                "b.removeChild(p);"),
       "eval removeChild");
    CK(js_dom_dirty(), "DOM dirty after removeChild");
    CK(!find(root, "p"), "p node gone from the tree");
    CK(run(ctx, "if (document.querySelector('.x') !== null) throw 'should be gone';"),
       "querySelector no longer finds removed node");

    /* classList add/remove/toggle/contains */
    js_dom_clear_dirty();
    CK(run(ctx, "var cl = document.querySelector('h1').classList;"
                "cl.add('big');"
                "if (!cl.contains('big')) throw 'add failed';"
                "cl.toggle('on'); cl.toggle('big');"
                "if (cl.contains('big')) throw 'toggle off failed';"
                "if (!cl.contains('on')) throw 'toggle on failed';"
                "cl.remove('on');"
                "if (cl.contains('on')) throw 'remove failed';"),
       "classList add/remove/toggle/contains");
    CK(js_dom_dirty(), "DOM dirty after classList ops");
    CK(!strcmp(dom_attr(h1b, "class") ? dom_attr(h1b, "class") : "", ""),
       "h1 class attribute back to empty");

    /* addEventListener: recorded, does not throw */
    CK(run(ctx, "document.body.addEventListener('click', function() {});"
                "document.body.addEventListener('keydown', function() {});"),
       "addEventListener does not throw");
    CK(js_dom_listener_count() == 2, "two listeners recorded");

    /* document.documentElement exists (no <html> in fixture -> first element) */
    CK(run(ctx, "if (document.documentElement === null) throw 'no documentElement';"),
       "document.documentElement is non-null");

    /* ---- innerHTML, both directions ----
     * The getter used to alias textContent, so it returned markup-free text.
     * It must return real markup now, tags and attributes included. */
    js_dom_clear_dirty();
    CK(run(ctx, "var iv = document.getElementById('ih');"
                "if (iv.innerHTML !== '<b>bold</b> and <i>it</i>') throw 'got: ' + iv.innerHTML;"),
       "innerHTML getter returns markup, not stripped text");
    CK(run(ctx, "if (document.getElementById('ih').textContent !== 'bold and it') throw 'tc';"),
       "textContent still strips markup (the two are different properties)");
    CK(!js_dom_dirty(), "reading innerHTML does not dirty the DOM");

    /* The setter parses through the HTML fragment algorithm and builds real
     * elements -- not a text node containing angle brackets. */
    CK(run(ctx, "document.getElementById('ih').innerHTML = '<span id=\"si\">deep<em>er</em></span>';"),
       "innerHTML setter accepted");
    CK(js_dom_dirty(), "DOM dirty after innerHTML set");
    struct node *si = dom_get_element_by_id(root->doc, "si");
    CK(si && !strcmp(si->tag, "span"), "innerHTML= built a real <span> element");
    CK(si && si->parent && !strcmp(si->parent->tag, "div"), "new subtree parented under the target");
    CK(dom_get_element_by_id(root->doc, "si") == si,
       "imported element joined the target document's id index");
    struct node *em = si ? find(si, "em") : 0;
    CK(em && !strcmp(firsttext(em), "er"), "nested <em> imported with its text");
    CK(run(ctx, "if (document.getElementById('ih').innerHTML !== "
                "'<span id=\"si\">deep<em>er</em></span>') throw 'rt: ' + "
                "document.getElementById('ih').innerHTML;"),
       "innerHTML round-trips through set then get");

    /* The old children are gone, not appended to -- and the strings the new
     * ones point at must outlive the throwaway fragment document. */
    CK(find(root, "b") == NULL, "innerHTML= replaced the previous children");

    /* Context-sensitive fragment parsing: "<td>x" is a cell inside a <tr> and
     * bare text inside a <div>. Same string, two trees -- which is exactly what
     * the fragment algorithm is for. */
    CK(run(ctx, "document.getElementById('trc').innerHTML = '<td>cell</td>';"),
       "innerHTML= on a <tr> accepted");
    struct node *trc = dom_get_element_by_id(root->doc, "trc");
    CK(trc && trc->first_child && !strcmp(trc->first_child->tag, "td"),
       "fragment parsed in <tr> context produced a <td>");
    CK(run(ctx, "document.getElementById('ih').innerHTML = '<td>cell</td>';"
                "if (document.getElementById('ih').innerHTML !== 'cell') throw 'div ctx';"),
       "the same string in <div> context drops the stray <td>");

    /* console.log/warn/error are installed and callable */
    CK(run(ctx, "console.log('L', 1); console.warn('W'); console.error('E');"),
       "console.log/warn/error callable");

    js_dom_cleanup(ctx);
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    dom_free(root);

    printf("\n--- live page: persistent runtime, events, timers ---\n");
    live_page_tests();

    printf(fails ? "\nJS-DOM TEST FAILED\n" : "\nJS-DOM TEST PASSED\n");
    return fails;
}
