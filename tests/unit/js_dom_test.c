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
#include <time.h>
#include "quickjs.h"
#include "dom.h"
#include "css.h"
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

/* ======================================================================
 * Part 3: the CSSOM -- element.style, getComputedStyle, classList, and the
 * invalidation record that decides how much of the document a mutation costs.
 * ====================================================================== */

/* Kept as a separate C string rather than inside a <style> element, because
 * that is the shape css_apply() takes: browser.c collects the <style> text out
 * of the DOM and hands it over as a buffer. */
static const char CSSOM_CSS[] =
    "#box { color: blue; background-color: #ffff00; width: 200px;"
    "       padding: 5px 6px 7px 8px; margin: 1px 2px 3px 4px;"
    "       font-size: 20px; font-family: Georgia, serif; position: relative;"
    "       top: 3px; left: 4px;"
    "       opacity: 0.5; z-index: 7; overflow: hidden;"
    "       border: 2px dashed #00ff00; text-align: center; line-height: 24px;"
    "       box-sizing: border-box; min-width: 10px; max-width: 400px; }"
    "#flex { display: flex; flex-direction: column; flex-wrap: wrap;"
    "        justify-content: space-between; align-items: center; }"
    "#fi { flex-grow: 2; flex-shrink: 0; flex-basis: 30px; order: 3;"
    "      align-self: flex-end; visibility: hidden; }"
    /* right/bottom get their own element: on a `position:relative` box CSS
     * §9.4.3 makes them the negation of left/top, so #box could never show
     * their authored values. */
    "#abs { position: absolute; right: 5px; bottom: 6px; }"
    "#gone { display: none; }"
    ".hot { color: #ff0000; }"
    ".wide { width: 333px; }"
    ".on + .after { color: rgb(1, 2, 3); }";

/* #leaf is deliberately the LAST child of <body>: the scoped re-style covers an
 * element and its FOLLOWING siblings, so a leaf at the end has a scope of
 * exactly one element and the count assertion below can be exact rather than
 * approximate. */
static const char CSSOM_HTML[] =
    "<html><body>"
    "<div id='box' style='font-weight: bold'>B</div>"
    "<div id='flex'><span id='fi'>i</span></div>"
    "<div id='sib'>s</div><div id='af' class='after'>a</div>"
    "<div id='abs'>p</div>"
    "<div id='gone'>g</div>"
    "<div id='par'><i id='k1'>1</i><i id='k2'>2</i></div>"
    "<div id='leaf'>leaf</div>"
    "</body></html>";

/* browser.c's restyle(), in miniature: read the invalidation record, re-style
 * exactly the marked scopes, fall back to the document when there are none.
 * Having the test drive the same decision procedure is the point -- it is the
 * decision procedure, not css_apply_scoped alone, that has to be right. */
static int settle(struct node *root, const char *css, int csslen)
{
    int level = js_dom_inval_level();
    if (level == INVAL_NONE) return CSS_CHANGED_NONE;
    int nroots = js_dom_inval_roots(), changed = CSS_CHANGED_NONE;
    for (int i = 0; i < nroots; i++) {
        int sib = 0;
        struct node *n = js_dom_inval_root(i, &sib);
        if (!n) { nroots = 0; break; }
        changed |= css_apply_scoped(n, sib, css, csslen);
    }
    if (nroots == 0) { css_apply(root, css, csslen); changed = CSS_CHANGED_LAYOUT; }
    if (level >= INVAL_LAYOUT) changed |= CSS_CHANGED_LAYOUT;
    js_dom_clear_dirty();
    return changed;
}

/* How many elements the last cascade actually touched. */
static int styled_since(int *mark)
{
    int now = 0;
    css_stats(&now, 0);
    int d = now - *mark;
    *mark = now;
    return d;
}

/* Assert a JS expression evaluates to the expected string. Reported by value,
 * because "expected 16px, got 20px" is the whole diagnostic. */
static void ckstr(const char *expr, const char *want, const char *what)
{
    char src[1024];
    snprintf(src, sizeof src,
             "(function(){var v=String(%s);if(v!==\"%s\")throw \"%s -> \"+v;})()",
             expr, want, what);
    if (!prun(src)) { printf("FAIL %s\n", what); fails = 1; }
    else printf("ok: %s == %s\n", what, want);
}

static void cssom_tests(void)
{
    css_init();
    css_viewport(760, 540);

    struct node *root = dom_parse(CSSOM_HTML, (int)strlen(CSSOM_HTML));
    CK(root != NULL, "part3: fixture parsed");
    if (!root) return;
    css_apply(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);

    struct node *box  = dom_get_element_by_id(root->doc, "box");
    struct node *fi   = dom_get_element_by_id(root->doc, "fi");
    struct node *sib  = dom_get_element_by_id(root->doc, "sib");
    struct node *af   = dom_get_element_by_id(root->doc, "af");
    struct node *leaf = dom_get_element_by_id(root->doc, "leaf");
    struct node *par  = dom_get_element_by_id(root->doc, "par");
    CK(box && fi && sib && af && leaf && par, "part3: fixture elements found");
    CK(box && box->computed != NULL,
       "css_apply left a css_computed_style on the node (the thing getComputedStyle reads)");

    g_now = 5000;
    js_page_set_clock(fake_clock);
    CK(js_page_open(root), "part3: page runtime opens over the styled DOM");

    /* ---- A. element.style: a live view of the style attribute ---- */
    ckstr("document.getElementById('box').style.fontWeight", "bold",
          "style reads a declaration that was in the markup");
    ckstr("document.getElementById('box').style.getPropertyValue('font-weight')", "bold",
          "getPropertyValue agrees with the camelCase accessor");
    ckstr("document.getElementById('box').style.color", "",
          "a property the attribute does not carry reads as empty");

    CK(prun("document.getElementById('box').style.backgroundColor = 'lime';"),
       "el.style.backgroundColor = 'lime'");
    CK(box && strstr(dom_attr(box, "style") ? dom_attr(box, "style") : "",
                     "background-color: lime") != NULL,
       "the write went into the style ATTRIBUTE, dashed-name and all");
    CK(box && strstr(dom_attr(box, "style") ? dom_attr(box, "style") : "",
                     "font-weight: bold") != NULL,
       "the declaration that was already there survived the write");
    ckstr("document.getElementById('box').style.backgroundColor", "lime",
          "the write round-trips through the attribute");

    CK(prun("document.getElementById('box').style.setProperty('margin-top', '9px');"),
       "setProperty('margin-top', '9px')");
    ckstr("document.getElementById('box').style.marginTop", "9px",
          "setProperty is visible through the camelCase accessor");
    ckstr("document.getElementById('box').style['margin-top']", "9px",
          "and through the dashed spelling");

    /* !important survives the round trip and is reported separately, which is
     * what makes getPropertyValue's answer usable as a value. */
    CK(prun("document.getElementById('box').style.setProperty('padding-top', '4px', 'important');"),
       "setProperty with priority");
    ckstr("document.getElementById('box').style.getPropertyValue('padding-top')", "4px",
          "getPropertyValue strips !important");
    ckstr("document.getElementById('box').style.getPropertyPriority('padding-top')", "important",
          "getPropertyPriority reports it");
    CK(box && strstr(dom_attr(box, "style"), "!important") != NULL,
       "!important really reached the attribute (so LibCSS sees it)");

    ckstr("document.getElementById('box').style.removeProperty('margin-top')", "9px",
          "removeProperty returns the old value");
    ckstr("document.getElementById('box').style.marginTop", "",
          "removeProperty removed it");
    ckstr("document.getElementById('box').style.fontWeight", "bold",
          "and left the neighbouring declarations alone");

    CK(prun("var s = document.getElementById('box').style;"
            "if (s.length < 3) throw 'length ' + s.length;"
            "var names = []; for (var i = 0; i < s.length; i++) names.push(s.item(i));"
            "if (names.indexOf('font-weight') < 0) throw 'item(): ' + names.join(',');"),
       "length + item() enumerate the declarations by name");

    CK(prun("document.getElementById('leaf').style.cssText = 'color: teal; width: 12px';"
            "var s = document.getElementById('leaf').style;"
            "if (s.color !== 'teal' || s.width !== '12px') throw 'cssText= ' + s.cssText;"
            "if (s.cssText.indexOf('teal') < 0) throw 'cssText read ' + s.cssText;"),
       "cssText writes and reads the whole declaration block");
    CK(prun("document.getElementById('leaf').style.removeProperty('color');"
            "document.getElementById('leaf').style.removeProperty('width');"),
       "leaf inline style cleared again");

    /* ---- B. cascade priority: inline still beats an author rule ----
     * This is the property that would have been silently lost by writing
     * computed values straight onto the node instead of into the attribute. */
    ckstr("getComputedStyle(document.getElementById('box')).color", "rgb(0, 0, 255)",
          "before the inline write the author rule (#box{color:blue}) wins");
    CK(prun("document.getElementById('box').style.color = 'red';"), "inline color: red");
    CK(settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1) != CSS_CHANGED_NONE,
       "the inline write re-styled something");
    ckstr("getComputedStyle(document.getElementById('box')).color", "rgb(255, 0, 0)",
          "INLINE STYLE BEATS THE AUTHOR RULE (it went through LibCSS as an inline sheet)");
    CK(prun("document.getElementById('box').style.removeProperty('color');"),
       "inline color removed again");
    settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);
    ckstr("getComputedStyle(document.getElementById('box')).color", "rgb(0, 0, 255)",
          "removing the inline declaration hands the cascade back to the author rule");

    /* ---- C. getComputedStyle: resolved values as CSS text ----
     * Clear the inline declarations part A wrote first, so what is measured
     * here is the author sheet rather than this test's own leftovers. */
    CK(prun("var s = document.getElementById('box').style;"
            "s.removeProperty('background-color'); s.removeProperty('padding-top');"),
       "part A's inline declarations removed");
    settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);
    CK(prun("window.cs = getComputedStyle(document.getElementById('box'));"),
       "getComputedStyle(el) returns a declaration");
    ckstr("cs.backgroundColor", "rgb(255, 255, 0)", "background-color");
    ckstr("cs.width", "200px", "width");
    ckstr("cs.paddingTop", "5px", "padding-top");
    ckstr("cs.paddingRight", "6px", "padding-right");
    ckstr("cs.paddingBottom", "7px", "padding-bottom");
    ckstr("cs.paddingLeft", "8px", "padding-left");
    ckstr("cs.marginTop", "1px", "margin-top");
    ckstr("cs.marginRight", "2px", "margin-right");
    ckstr("cs.marginBottom", "3px", "margin-bottom");
    ckstr("cs.marginLeft", "4px", "margin-left");
    ckstr("cs.fontSize", "20px", "font-size");
    ckstr("cs.fontFamily", "Georgia, serif", "font-family keeps the authored list");
    ckstr("cs.fontWeight", "700", "font-weight is numeric, as a real UA reports it");
    ckstr("cs.fontStyle", "normal", "font-style");
    ckstr("cs.display", "block", "display");
    ckstr("cs.position", "relative", "position");
    ckstr("cs.top", "3px", "top");
    ckstr("cs.left", "4px", "left");
    /* On a relatively positioned box, right/bottom are the negation of
     * left/top (CSS 9.4.3) -- so both the authored pair and the derived pair
     * get checked, on two different elements. */
    ckstr("cs.right", "-4px", "right on a relative box is -left");
    ckstr("cs.bottom", "-3px", "bottom on a relative box is -top");
    ckstr("getComputedStyle(document.getElementById('abs')).right", "5px",
          "right, authored on an absolutely positioned box");
    ckstr("getComputedStyle(document.getElementById('abs')).bottom", "6px",
          "bottom, authored on an absolutely positioned box");
    ckstr("cs.opacity", "0.5", "opacity");
    ckstr("cs.zIndex", "7", "z-index");
    ckstr("cs.overflow", "hidden", "overflow (both axes agree -> one value)");
    ckstr("cs.borderTopWidth", "2px", "border-top-width");
    ckstr("cs.borderLeftStyle", "dashed", "border-left-style");
    ckstr("cs.borderRightColor", "rgb(0, 255, 0)", "border-right-color");
    ckstr("cs.textAlign", "center", "text-align");
    ckstr("cs.lineHeight", "24px", "line-height");
    ckstr("cs.visibility", "visible", "visibility");
    ckstr("cs.boxSizing", "border-box", "box-sizing");
    ckstr("cs.minWidth", "10px", "min-width");
    ckstr("cs.maxWidth", "400px", "max-width");
    /* Nothing set them, so these report their initial values -- which is the
     * whole point of a COMPUTED style as opposed to the declarations. */
    ckstr("cs.maxHeight", "none", "max-height falls back to its initial value");
    ckstr("cs.height", "auto", "height reports the computed value (see css.h on resolved values)");
    ckstr("cs.float", "none", "float");
    ckstr("cs.cssFloat", "none", "cssFloat is the same property under its IDL name");
    ckstr("cs.whiteSpace", "normal", "white-space");
    ckstr("cs.textDecoration", "none", "text-decoration");

    CK(prun("window.fs = getComputedStyle(document.getElementById('flex'));"
            "window.fis = getComputedStyle(document.getElementById('fi'));"), "flex styles");
    ckstr("fs.display", "flex", "display:flex");
    ckstr("fs.flexDirection", "column", "flex-direction");
    ckstr("fs.flexWrap", "wrap", "flex-wrap");
    ckstr("fs.justifyContent", "space-between", "justify-content");
    ckstr("fs.alignItems", "center", "align-items");
    ckstr("fis.flexGrow", "2", "flex-grow");
    ckstr("fis.flexShrink", "0", "flex-shrink");
    ckstr("fis.flexBasis", "30px", "flex-basis");
    ckstr("fis.order", "3", "order");
    ckstr("fis.alignSelf", "flex-end", "align-self");
    ckstr("fis.visibility", "hidden", "visibility:hidden");
    ckstr("getComputedStyle(document.getElementById('gone')).display", "none",
          "display:none is reported, not hidden from the CSSOM");
    /* Inheritance is what a computed style is FOR: #fi never mentions colour. */
    ckstr("fis.color", "rgb(0, 0, 0)", "an inherited property resolves on the child");

    /* CSS property names are ASCII case-insensitive, so getPropertyValue has
     * to answer whatever case a page hands it. */
    ckstr("cs.getPropertyValue('BACKGROUND-COLOR')", "rgb(255, 255, 0)",
          "getPropertyValue is case-insensitive on the dashed name");
    ckstr("cs.getPropertyValue('no-such-property')", "",
          "a property we cannot resolve reads as empty, not as a throw");
    CK(prun("var c = getComputedStyle(document.getElementById('box'));"
            "if (c.length < 40) throw 'length ' + c.length;"
            "if (c.item(0) !== 'width') throw 'item(0) ' + c.item(0);"
            "if (c.cssText !== '') throw 'computed cssText should be empty';"
            "c.color = 'purple'; c.setProperty('color', 'purple');"
            "if (getComputedStyle(document.getElementById('box')).color !== 'rgb(0, 0, 255)')"
            "  throw 'a computed declaration must be read-only';"),
       "a computed declaration enumerates, has no cssText and ignores writes");

    /* ---- D. classList, completed ---- */
    CK(prun("var e = document.getElementById('leaf');"
            "e.setAttribute('class', 'a b c');"
            "var cl = e.classList;"
            "if (cl.length !== 3) throw 'length ' + cl.length;"
            "if (cl.item(0) !== 'a' || cl.item(2) !== 'c') throw 'item()';"
            "if (cl.item(9) !== null) throw 'item() past the end must be null';"
            "if (cl[1] !== 'b') throw 'index ' + cl[1];"
            "if (cl[9] !== undefined) throw 'index past the end';"
            "if (String(cl) !== 'a b c') throw 'toString ' + String(cl);"
            "if (cl.value !== 'a b c') throw 'value ' + cl.value;"),
       "classList length / item() / indexing / toString / value");
    CK(prun("var cl = document.getElementById('leaf').classList;"
            "if ([...cl].join('-') !== 'a-b-c') throw 'spread ' + [...cl].join('-');"
            "var seen = []; for (var t of cl) seen.push(t);"
            "if (seen.join('-') !== 'a-b-c') throw 'for-of ' + seen.join('-');"
            "var f = []; cl.forEach(function(t){ f.push(t); });"
            "if (f.join('-') !== 'a-b-c') throw 'forEach ' + f.join('-');"),
       "classList is iterable (spread, for..of, forEach)");
    /* The index hook is consulted for EVERY key, including Symbol.iterator.
     * It must fall through to the prototype for a non-index key, and it must
     * not try to stringify a symbol on the way (that throws, and the exception
     * would sit pending in the context until something unrelated tripped
     * over it). */
    CK(prun("var cl = document.getElementById('leaf').classList;"
            "if (typeof cl[Symbol.iterator] !== 'function') throw 'no Symbol.iterator';"
            "if (typeof cl.add !== 'function') throw 'the index hook shadowed the prototype';"
            "if (!('length' in cl)) throw 'length missing';"
            "if (cl.nosuchkey !== undefined) throw 'made up a value for an unknown key';"),
       "the index hook falls through to the prototype and tolerates a Symbol key");
    CK(prun("var cl = document.getElementById('leaf').classList;"
            "if (cl.replace('b', 'B') !== true) throw 'replace returned false';"
            "if (String(cl) !== 'a B c') throw 'replace lost the position: ' + String(cl);"
            "if (cl.replace('zz', 'yy') !== false) throw 'replace of a missing token';"),
       "classList.replace() swaps in place and reports whether it did");
    CK(prun("var cl = document.getElementById('leaf').classList;"
            "cl.add('d', 'e');"
            "if (String(cl) !== 'a B c d e') throw 'multi-add ' + String(cl);"
            "cl.remove('a', 'e');"
            "if (String(cl) !== 'B c d') throw 'multi-remove ' + String(cl);"
            "if (cl.toggle('c', true) !== true || String(cl) !== 'B c d') throw 'toggle force-on';"
            "if (cl.toggle('c', false) !== false || String(cl) !== 'B d') throw 'toggle force-off';"),
       "add/remove take several tokens and toggle takes a force flag");
    /* The live-ness the exotic index hook exists for: the SAME object must see
     * a token added after it was handed out. */
    CK(prun("var cl = document.getElementById('leaf').classList;"
            "var before = cl.length; cl.add('zed');"
            "if (cl.length !== before + 1) throw 'length did not update';"
            "if (cl[cl.length - 1] !== 'zed') throw 'index did not update';"
            "cl.remove('zed');"),
       "a token list handed to a script stays LIVE");
    CK(prun("document.getElementById('leaf').setAttribute('class', '');"), "leaf class cleared");

    /* ---- E. invalidation: the right scope, and not the whole document ---- */
    js_dom_clear_dirty();
    CK(js_dom_inval_level() == INVAL_NONE, "a clean page reports INVAL_NONE");

    CK(prun("document.getElementById('leaf').classList.add('hot');"), "class toggle on a leaf");
    CK(js_dom_inval_level() == INVAL_STYLE, "a class change is a STYLE invalidation");
    CK(js_dom_inval_roots() == 1, "it marked exactly ONE scope, not the document");
    {
        int sib = 0;
        CK(js_dom_inval_root(0, &sib) == leaf, "the scope is the element that changed");
        CK(sib == 1, "and its following siblings, for the sibling combinators");
    }
    {
        int mark = 0; styled_since(&mark);
        int ch = settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);
        int n = styled_since(&mark);
        /* #leaf is the last child of <body>, so the scope is exactly itself. */
        CK(n == 1, "the re-style touched ONE element, not the document");
        CK(ch == CSS_CHANGED_PAINT,
           ".hot { color } is reported as PAINT-only: no box moved");
        printf("    (leaf class toggle re-styled %d element(s))\n", n);
    }
    ckstr("getComputedStyle(document.getElementById('leaf')).color", "rgb(255, 0, 0)",
          "and the scoped re-style really produced the new colour");

    /* A class that matches no rule at all: the cascade runs and reports that
     * nothing came out different, so the caller skips layout AND the repaint. */
    js_dom_clear_dirty();
    CK(prun("document.getElementById('leaf').classList.add('nosuchrule');"), "meaningless class");
    CK(settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1) == CSS_CHANGED_NONE,
       "a class that matches NO rule costs no layout and no repaint");

    /* A width class does move boxes. */
    js_dom_clear_dirty();
    CK(prun("document.getElementById('leaf').classList.add('wide');"), "width class");
    CK(settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1) == CSS_CHANGED_LAYOUT,
       ".wide { width } is reported as LAYOUT-affecting");
    ckstr("getComputedStyle(document.getElementById('leaf')).width", "333px", "the width took");
    prun("document.getElementById('leaf').setAttribute('class', '');");
    settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);

    /* Inline-style writes are tiered by which property they touch. */
    js_dom_clear_dirty();
    CK(prun("document.getElementById('box').style.color = 'green';"), "inline paint-only write");
    CK(js_dom_inval_level() == INVAL_PAINT, "writing a colour is a PAINT invalidation");
    settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);
    js_dom_clear_dirty();
    CK(prun("document.getElementById('box').style.width = '111px';"), "inline geometry write");
    CK(js_dom_inval_level() == INVAL_STYLE, "writing a width is a STYLE invalidation");
    settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);
    prun("document.getElementById('box').style.removeProperty('color');"
         "document.getElementById('box').style.removeProperty('width');");
    settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);

    /* Writing the value that is already there must not dirty anything: a page
     * that re-asserts its style every animation frame otherwise re-styles (and
     * grows the DOM arena) forever. */
    prun("document.getElementById('box').style.color = 'orange';");
    settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);
    js_dom_clear_dirty();
    CK(prun("document.getElementById('box').style.color = 'orange';"), "same value written again");
    CK(js_dom_inval_level() == INVAL_NONE, "a redundant style write does not dirty the page");
    prun("document.getElementById('leaf').classList.add('hot');");
    settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);
    js_dom_clear_dirty();
    CK(prun("document.getElementById('leaf').classList.add('hot');"), "class already present");
    CK(js_dom_inval_level() == INVAL_NONE, "re-adding a class that is present does not dirty it");
    prun("document.getElementById('leaf').setAttribute('class', '');");
    settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);
    prun("document.getElementById('box').style.removeProperty('color');");
    settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);

    /* Structural changes are LAYOUT by construction, rooted at the parent (that
     * is what moves :first-child / :nth-child / :empty for the siblings). */
    js_dom_clear_dirty();
    CK(prun("var d = document.createElement('b');"
            "document.getElementById('par').appendChild(d);"), "appendChild");
    CK(js_dom_inval_level() == INVAL_LAYOUT, "appendChild is a LAYOUT invalidation");
    {
        int sib = 0;
        CK(js_dom_inval_roots() == 1 && js_dom_inval_root(0, &sib) == par,
           "the scope is the PARENT whose child list changed");
        CK(sib == 0, "structural changes need no sibling scope (no :has())");
    }
    js_dom_clear_dirty();
    CK(prun("document.getElementById('par').textContent = 'flat';"), "textContent=");
    CK(js_dom_inval_level() == INVAL_LAYOUT, "textContent= is a LAYOUT invalidation");
    CK(settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1) == CSS_CHANGED_LAYOUT,
       "a structural change forces layout even when no computed style moved");

    /* Coalescing: several mutations under one parent are ONE scope, and a
     * mutation on an ancestor swallows the scopes below it. */
    js_dom_clear_dirty();
    CK(prun("var p = document.getElementById('par');"
            "p.appendChild(document.createElement('u'));"
            "p.appendChild(document.createElement('u'));"
            "p.appendChild(document.createElement('u'));"), "three appends under one parent");
    CK(js_dom_inval_roots() == 1, "three mutations in one place are ONE scope");
    js_dom_clear_dirty();
    CK(prun("document.getElementById('par').classList.add('x');"
            "document.body.classList.add('y');"), "a leaf then an ancestor");
    CK(js_dom_inval_roots() == 1 && js_dom_inval_root(0, 0) == dom_doc_body(root->doc),
       "an ancestor scope swallows the descendant scope it already covers");
    prun("document.getElementById('par').setAttribute('class','');"
         "document.body.setAttribute('class','');");
    settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);

    /* ---- E. off-document mutations claim no scope ----
     *
     * The rule mark() now applies: a node that is not attached to the document
     * has no box and no pixel, so mutating it cannot leave anything on screen
     * stale and must not spend a scope. This is measured, not just asserted --
     * see measure_commit() -- but the PROPERTY is asserted here, because the
     * measurement would still look good if the mutations were being dropped
     * for the wrong reason. */
    js_dom_clear_dirty();
    CK(prun("var det = document.createElement('div');"
            "det.className = 'wide';"                  /* .wide { width: 333px } */
            "det.setAttribute('data-k', '1');"
            "det.id = 'detached';"
            "det.style.color = '#090807';"
            "var t = document.createTextNode('x'); det.appendChild(t);"
            "t.nodeValue = 'y';"),
       "seven mutations on a node that is not in the document");
    CK(js_dom_inval_level() == INVAL_NONE,
       "an off-document mutation claims NO scope and does not even dirty the page");

    /* ...and the attach is what makes all of it visible, in one scope. */
    js_dom_clear_dirty();
    CK(prun("document.getElementById('par').appendChild(det);"), "the attach");
    CK(js_dom_inval_level() == INVAL_LAYOUT && js_dom_inval_roots() == 1
       && js_dom_inval_root(0, 0) == par,
       "attaching it marks exactly the destination -- which covers everything "
       "that came in with it");
    settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);
    /* The proof that dropping those marks lost nothing. Both an AUTHOR-SHEET
     * match (the class, via .wide) and an INLINE write (the colour) were made
     * while the element was off-document and claimed no scope of their own;
     * both have to be live now, produced by the scoped re-style that the ATTACH
     * asked for. If the skip were losing work, this is where it would show. */
    ckstr("getComputedStyle(document.getElementById('detached')).width", "333px",
          "the off-document className is live after the attach");
    ckstr("getComputedStyle(document.getElementById('detached')).color", "rgb(9, 8, 7)",
          "and so is the off-document inline style");

    /* ---- overflow: more distinct places than the record holds ----
     *
     * The whole-document fallback is the safety net under every scope decision
     * above, so it has to stay exercised -- a fallback nothing tests is not a
     * fallback.
     *
     * The twelve scopes are built ATTACHED and mutated in a turn of their own.
     * This fixture used to create twelve DETACHED divs and lean on the fact
     * that setAttribute on each of them claimed a scope. Since off-document
     * mutations claim none, that shape now produces exactly ONE scope -- <body>,
     * from the twelve appends -- and the test would have gone on passing while
     * testing nothing at all. So the build is settled first, and only then are
     * twelve unrelated, attached, sibling leaves touched: twelve real places in
     * one turn, which is the situation the cap exists for. */
    prun("var b = document.body;"
         "for (var i = 0; i < 12; i++) {"
         "  var d = document.createElement('div'); d.id = 'ov' + i;"
         "  b.appendChild(d); }");
    CK(js_dom_inval_roots() == 1,
       "building twelve children off-document and appending them is ONE scope");
    settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);   /* the build is not what is under test */

    js_dom_clear_dirty();
    CK(prun("for (var i = 0; i < 12; i++)"
            "  document.getElementById('ov'+i).classList.add('hot');"),
       "twelve separate scopes marked, every one of them attached");
    CK(js_dom_inval_roots() == 0, "past the cap the record reports 'whole document'");
    CK(settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1) == CSS_CHANGED_LAYOUT,
       "and the caller re-styles everything, which is the old behaviour");
    ckstr("getComputedStyle(document.getElementById('ov11')).color", "rgb(255, 0, 0)",
          "the fallback really did re-style the twelfth element");

    /* A scope whose node is destroyed before the embedder gets to it. The
     * subtree is torn down through the DOM API directly, NOT through
     * removeChild -- removeChild would mark its parent and the record would
     * notice the recycled slot at mark time. This is the other order: the node
     * dies quietly and the staleness has to be caught at READ time, by the
     * serial. */
    js_dom_clear_dirty();
    CK(prun("document.getElementById('ov0').classList.add('wide');"),
       "a scope marked on a node that is about to die");
    CK(js_dom_inval_roots() == 1, "one scope marked");
    dom_destroy_subtree(dom_get_element_by_id(root->doc, "ov0"));
    CK(js_dom_inval_root(0, 0) == NULL,
       "a scope whose node was recycled reads as NULL, not as the new occupant");
    CK(settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1) == CSS_CHANGED_LAYOUT,
       "and the caller falls back to re-styling the whole document");

    /* ---- F. sibling scope: `.on + .after` ---- */
    js_dom_clear_dirty();
    ckstr("getComputedStyle(document.getElementById('af')).color", "rgb(0, 0, 0)",
          "the sibling rule does not match yet");
    CK(prun("document.getElementById('sib').classList.add('on');"), "class added to #sib");
    {
        int want_sib = 0;
        CK(js_dom_inval_root(0, &want_sib) == sib && want_sib == 1,
           "the scope is #sib AND its following siblings");
    }
    settle(root, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);
    ckstr("getComputedStyle(document.getElementById('af')).color", "rgb(1, 2, 3)",
          "THE FOLLOWING SIBLING was re-styled -- `.on + .after` fired");
    /* And the converse: a scope WITHOUT the sibling flag leaves #af behind,
     * which is the bug this flag exists to prevent. */
    prun("document.getElementById('sib').setAttribute('class', '');");
    js_dom_clear_dirty();
    css_apply_scoped(sib, 0, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);
    ckstr("getComputedStyle(document.getElementById('af')).color", "rgb(1, 2, 3)",
          "without the sibling flag the neighbour keeps its stale colour (why the flag exists)");
    css_apply_scoped(sib, 1, CSSOM_CSS, (int)sizeof CSSOM_CSS - 1);
    ckstr("getComputedStyle(document.getElementById('af')).color", "rgb(0, 0, 0)",
          "with it, the neighbour is corrected");

    js_page_close();
    dom_free(root);
    (void)fi; (void)box;
}

/* ======================================================================
 * The invalidation measurement.
 *
 * The claim being tested is not "scoped re-styling is architecturally nicer",
 * it is "a class toggle on a leaf must not cost a full-document re-style" --
 * so it is measured, on a document of a few thousand elements, in the two
 * units that matter: how many elements the cascade visited, and how long it
 * took. Both paths do the identical toggle; only the invalidation scope
 * differs.
 * ====================================================================== */

#define MEAS_SECTIONS 120
#define MEAS_LEAVES    25            /* -> 120 * 26 + 2 = ~3100 elements */
#define MEAS_REPS      20

static const char MEAS_CSS[] =
    "body{margin:0;font-family:sans-serif}"
    "section{display:block;padding:4px;border-bottom:1px solid #eee}"
    "section p{margin:2px 0;line-height:1.4}"
    "section p b{font-weight:bold}"
    "section:first-child{padding-top:0}"
    ".hot{color:#c00}.cold{color:#00c}"
    "p.hot b{text-decoration:underline}"
    "div > section > p{font-size:14px}"
    "[data-k]{opacity:1}"
    "p:nth-child(2n){background:#fafafa}"
    "section p + p{margin-top:6px}"
    "a:link{color:#1a0dab}a:hover{color:#c00}"
    "#wrap{max-width:900px}";

/* a tiny local growable buffer (the test cannot see js_dom.c's) */
struct mbuf { char *p; size_t n, cap; };
static void mb(struct mbuf *b, const char *s)
{
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + l + 1) c *= 2;
        b->p = realloc(b->p, c);
        b->cap = c;
    }
    memcpy(b->p + b->n, s, l);
    b->n += l;
    b->p[b->n] = 0;
}

static void measure_invalidation(void)
{
    struct mbuf h = { 0, 0, 0 };
    char tmp[128];
    mb(&h, "<html><body><div id='wrap'>");
    for (int s = 0; s < MEAS_SECTIONS; s++) {
        snprintf(tmp, sizeof tmp, "<section id='S%d' data-k='%d'>", s, s);
        mb(&h, tmp);
        for (int i = 0; i < MEAS_LEAVES; i++) {
            snprintf(tmp, sizeof tmp, "<p id='L%d'>t<b>x</b></p>", s * MEAS_LEAVES + i);
            mb(&h, tmp);
        }
        mb(&h, "</section>");
    }
    mb(&h, "</div></body></html>");

    struct node *root = dom_parse(h.p, (int)h.n);
    if (!root) { printf("FAIL measurement fixture\n"); fails = 1; free(h.p); return; }
    const int csslen = (int)sizeof MEAS_CSS - 1;
    css_apply(root, MEAS_CSS, csslen);          /* warm: the scoped path resumes from this */

    /* The very last <p> in the document: no following siblings, so the scope is
     * exactly the element that changed -- the shape a framework's class toggle
     * actually has. */
    snprintf(tmp, sizeof tmp, "L%d", MEAS_SECTIONS * MEAS_LEAVES - 1);
    struct node *leaf = dom_get_element_by_id(root->doc, tmp);
    if (!leaf) { printf("FAIL measurement leaf\n"); fails = 1; dom_free(root); free(h.p); return; }

    int mark = 0;
    styled_since(&mark);

    /* BEFORE: what every mutation used to cost -- css_apply over the document. */
    clock_t t0 = clock();
    for (int r = 0; r < MEAS_REPS; r++) {
        dom_set_attr(leaf, "class", (r & 1) ? "hot" : "cold");
        css_apply(root, MEAS_CSS, csslen);
    }
    clock_t t1 = clock();
    int full_total = styled_since(&mark);

    /* AFTER: the same toggle, re-styling only the scope the mutation marked. */
    clock_t t2 = clock();
    for (int r = 0; r < MEAS_REPS; r++) {
        dom_set_attr(leaf, "class", (r & 1) ? "hot" : "cold");
        css_apply_scoped(leaf, 1, MEAS_CSS, csslen);
    }
    clock_t t3 = clock();
    int scoped_total = styled_since(&mark);

    double full_ms = (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC / MEAS_REPS;
    double scoped_ms = (double)(t3 - t2) * 1000.0 / CLOCKS_PER_SEC / MEAS_REPS;
    int elems = full_total / MEAS_REPS;

    printf("\n--- invalidation measurement (%d elements, %d toggles each) ---\n",
           elems, MEAS_REPS);
    printf("  before (css_apply, whole document): %6d elements re-styled, %8.3f ms per toggle\n",
           full_total / MEAS_REPS, full_ms);
    printf("  after  (css_apply_scoped, one leaf): %5d elements re-styled, %8.3f ms per toggle\n",
           scoped_total / MEAS_REPS, scoped_ms);
    if (scoped_ms > 0.0)
        printf("  -> %dx fewer elements, %.0fx faster\n",
               (full_total / MEAS_REPS) / (scoped_total / MEAS_REPS > 0 ? scoped_total / MEAS_REPS : 1),
               full_ms / scoped_ms);

    CK(full_total / MEAS_REPS > 2000,
       "measurement: the old path really did re-style the whole document");
    /* Two: the <p> that changed and the <b> inside it. The subtree is not
     * optional -- the child inherits from the parent, and `p.hot b` in the
     * sheet keys off the class that just moved. */
    CK(scoped_total / MEAS_REPS == 2,
       "measurement: the new path re-styles the changed element and its subtree, nothing else");
    CK(scoped_ms * 20.0 < full_ms,
       "measurement: and it is more than an order of magnitude faster");

    dom_free(root);
    free(h.p);
}

/* ======================================================================
 * The COMMIT measurement.
 *
 * measure_invalidation() above measures one class toggle -- a mutation in a
 * single place, which the record has always scoped well. This measures the
 * shape react-dom actually commits in, which it did NOT: a subtree built
 * entirely off-document over ~30 calls, then attached with one insertBefore.
 *
 * Before off-document mutations were exempted from the record, each of those
 * ~30 claimed a scope root; there are only JS_DOM_MAX_DIRTY (8), so the record
 * overflowed on EVERY commit and the embedder fell back to css_apply over the
 * whole document. The two rows printed below are exactly those two
 * dispositions, run over the same document with the same commit -- "whole
 * document" is not a simulation of the old behaviour, it is the fallback path
 * settle() takes when js_dom_inval_roots() reports 0.
 *
 * The scope COUNT is the other half, and it is the half that decides which row
 * you pay for. It is asserted, not printed: 1, where it used to be 0.
 * ====================================================================== */
#define COMMIT_ROWS  10               /* 10 rows x 3 nodes = ~34 calls per commit */
#define COMMIT_REPS  20

static void measure_commit(void)
{
    struct mbuf h = { 0, 0, 0 };
    char tmp[128];
    mb(&h, "<html><body><div id='wrap'>");
    for (int s = 0; s < MEAS_SECTIONS; s++) {
        snprintf(tmp, sizeof tmp, "<section id='S%d' data-k='%d'>", s, s);
        mb(&h, tmp);
        for (int i = 0; i < MEAS_LEAVES; i++) {
            snprintf(tmp, sizeof tmp, "<p id='L%d'>t<b>x</b></p>", s * MEAS_LEAVES + i);
            mb(&h, tmp);
        }
        mb(&h, "</section>");
    }
    mb(&h, "<div id='mount'></div></div></body></html>");

    struct node *root = dom_parse(h.p, (int)h.n);
    if (!root) { printf("FAIL commit fixture\n"); fails = 1; free(h.p); return; }
    const int csslen = (int)sizeof MEAS_CSS - 1;
    css_apply(root, MEAS_CSS, csslen);

    struct node *mount = dom_get_element_by_id(root->doc, "mount");
    if (!mount) { printf("FAIL commit mount\n"); fails = 1; dom_free(root); free(h.p); return; }

    g_now = 9000;
    js_page_set_clock(fake_clock);
    if (!js_page_open(root)) { printf("FAIL commit runtime\n"); fails = 1;
                              dom_free(root); free(h.p); return; }

    /* One React-shaped commit: a fragment, ten rows, each row an element with a
     * class, an attribute and a text child -- built off-document -- then ONE
     * attach. Nothing here is contrived for the measurement; it is the sequence
     * react-dom's host config emits for `<ul>{items.map(...)}</ul>`. */
    static const char COMMIT_JS[] =
        "(function(){"
        "  var m = document.getElementById('mount');"
        "  m.textContent = '';"
        "  var f = document.createDocumentFragment();"
        "  for (var i = 0; i < 10; i++) {"
        "    var row = document.createElement('p');"
        "    row.className = (i & 1) ? 'hot' : 'cold';"
        "    row.setAttribute('data-k', String(i));"
        "    var b = document.createElement('b');"
        "    b.appendChild(document.createTextNode('r' + i));"
        "    row.appendChild(b);"
        "    f.appendChild(row);"
        "  }"
        "  m.appendChild(f);"
        "})()";

    /* The scope count for one commit. Read before any timing, because the
     * timing loops below deliberately re-style between commits. */
    js_dom_clear_dirty();
    CK(prun(COMMIT_JS), "commit measurement: a React-shaped commit ran");
    int roots = js_dom_inval_roots();
    int level = js_dom_inval_level();
    struct node *scope = js_dom_inval_root(0, 0);
    CK(level == INVAL_LAYOUT, "commit measurement: the commit asks for layout");
    CK(roots == 1 && scope == mount,
       "commit measurement: ~34 calls claim exactly ONE scope -- the mount point "
       "(it was 0, meaning 'whole document', when off-document mutations counted)");
    settle(root, MEAS_CSS, csslen);

    int mark = 0;
    styled_since(&mark);

    /* BEFORE: the disposition an overflowed record forces -- settle()'s
     * nroots == 0 branch, which is css_apply over the document. */
    clock_t t0 = clock();
    for (int r = 0; r < COMMIT_REPS; r++) {
        prun(COMMIT_JS);
        js_dom_clear_dirty();
        css_apply(root, MEAS_CSS, csslen);
    }
    clock_t t1 = clock();
    int full_total = styled_since(&mark);

    /* AFTER: the disposition the record now produces -- one scope, re-styled. */
    clock_t t2 = clock();
    for (int r = 0; r < COMMIT_REPS; r++) {
        prun(COMMIT_JS);
        js_dom_clear_dirty();
        css_apply_scoped(mount, 0, MEAS_CSS, csslen);
    }
    clock_t t3 = clock();
    int scoped_total = styled_since(&mark);

    double full_ms = (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC / COMMIT_REPS;
    double scoped_ms = (double)(t3 - t2) * 1000.0 / CLOCKS_PER_SEC / COMMIT_REPS;

    printf("\n--- commit measurement (%d elements, one React-shaped commit x %d) ---\n",
           full_total / COMMIT_REPS, COMMIT_REPS);
    printf("  before (record overflowed -> whole document): %5d elements re-styled, %8.3f ms\n",
           full_total / COMMIT_REPS, full_ms);
    printf("  after  (record reports 1 scope -> the mount): %5d elements re-styled, %8.3f ms\n",
           scoped_total / COMMIT_REPS, scoped_ms);
    printf("  scopes claimed by one commit: %d (was 0 == whole document)\n", roots);
    if (scoped_ms > 0.0 && scoped_total > 0)
        printf("  -> %dx fewer elements, %.0fx faster per commit\n",
               (full_total / COMMIT_REPS) / (scoped_total / COMMIT_REPS),
               full_ms / scoped_ms);

    CK(full_total / COMMIT_REPS > 2000,
       "commit measurement: the overflow path really did re-style the whole document");
    /* 31: the mount, ten <p>, ten <b> and ten text nodes' owning elements --
     * i.e. exactly the subtree that arrived, and nothing above it. */
    CK(scoped_total / COMMIT_REPS < 100,
       "commit measurement: the scoped path re-styles only what the commit inserted");
    CK(scoped_ms * 10.0 < full_ms,
       "commit measurement: and a commit is an order of magnitude cheaper");

    js_page_close();
    dom_free(root);
    free(h.p);
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

    printf("\n--- CSSOM: element.style, getComputedStyle, classList, invalidation ---\n");
    cssom_tests();

    measure_invalidation();
    measure_commit();

    printf(fails ? "\nJS-DOM TEST FAILED\n" : "\nJS-DOM TEST PASSED\n");
    return fails;
}
