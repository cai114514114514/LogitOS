/* domlife_drive.js -- CONTROL for the js-framework-benchmark "24 silent run
 * failures" finding (2026-08-29). Loaded by
 *   build/webapi_probe --drive tests/unit/domlife_drive.js tests/fixtures/domlife
 * exactly the way tests/unit/jsfb_drive.js is loaded -- same --drive
 * mechanism, same #TAG\tfield\tfield convention, evaluated in channel 2 only.
 *
 * ============================ THE FINDING ================================
 * 24 of 27 considered js-framework-benchmark implementations fail `run`
 * (create 1,000 rows) SILENTLY: no exception, `rows=0 want=1000`. Traced with
 * an ad hoc /tmp fixture (not committed -- reproduced here instead) to a
 * single mechanism, confirmed independently in TWO unrelated compiled
 * outputs pulled straight from the built corpus:
 *
 *   build/jsfb/frameworks/keyed/solid/dist/main.js:
 *     u.$$click=()=>o(t)                                  (dom-expressions)
 *   build/jsfb/frameworks/keyed/svelte/dist/main.js:
 *     Ee(_).__click=()=>{ve(r,c(1e3))}                    (Svelte 5's own compiler)
 *
 * Both stash the click handler as a plain expando PROPERTY on the target
 * element -- not addEventListener -- and rely on ONE listener registered on
 * `document` (or the app root) that walks up from `event.target` on every
 * click looking for that property. This is not a framework idiom to special-
 * case: `grep -rlE '\.(__|\$\$)(click|input|change)=' build/jsfb/frameworks`
 * (excluding node_modules) currently names implementations built by at least
 * three independent compiler codebases (dom-expressions' family, Svelte's
 * own compiler, and Ember Octane's glimmer VM). Nothing here is fitted to
 * one of them; it asserts the platform contract they all assume.
 *
 * WHY IT FAILS ON THIS ENGINE, TRACED TO SOURCE:
 *   c/apps/browser/js_dom.c's wrap() (~line 309) caches one wrapper per node
 *   in the node's `jsw` slot -- but the comment at its own definition (~line
 *   304) says the slot "takes no reference: the finalizer clears it". A
 *   wrapper is a normal refcounted QuickJS object; when the only JS
 *   reference to it drops (which happens the instant a component's `mount`-
 *   style function returns, if it never assigns the element to a variable
 *   that outlives the function -- the exact shape both dist bundles above
 *   use), elem_finalizer runs SYNCHRONOUSLY (QuickJS is refcounted, not
 *   deferred GC) and clears n->jsw. The underlying `struct node` is still
 *   very much alive (it is reachable from its PARENT via the native child
 *   list -- dom.c's tree, entirely independent of any JS refcount), but its
 *   JS wrapper is gone, and with it every expando property ever set on it.
 *   The NEXT time anything re-wraps that node -- e.g. a document-level click
 *   handler walking `e.target.parentNode` -- it gets a brand new, empty
 *   wrapper. No exception anywhere: property reads on a fresh object just
 *   return undefined, which is valid JS.
 *
 *   In a real browser this cannot happen: a DOM node reachable from a live
 *   document is a GC root by construction (wrapper tracing / "the node's
 *   wrapper survives exactly as long as the node is reachable"), regardless
 *   of whether any JS variable also references it. This engine has the
 *   opposite invariant -- wrapper lifetime is tied to JS reachability alone
 *   -- and that is the platform gap, not a defect in 24 independent apps.
 *
 * THE FIX THIS CONTROL IS WAITING FOR, not applied here because
 * c/apps/browser/js_dom.c, dom.c and dom.h were all under active edit by a
 * sibling line of work at the time this was written (git status + mtimes
 * checked first, per CLAUDE.md's ownership rule) and elem_finalizer/wrap()/
 * recycle_tree were the exact area at risk of a collision: give each node a
 * STRONG hold on its own wrapper for as long as the node exists natively --
 * taken in wrap() when a wrapper is first created, released at the point
 * dom.c's recycle_tree()/node_recycle() actually frees the node (which is
 * DOM-only C with no JSContext, so that release has to happen through a
 * hook symmetric with the existing `g_script_sink` pattern, called from
 * js_dom.c before the node is handed to dom_destroy_subtree / at document
 * teardown). Ordinary JS refcounting is untouched; only "no live JS reference
 * anywhere" stops meaning "wrapper dies" while the node itself survives.
 *
 * ============================ WHAT THIS FILE ASSERTS =====================
 * Two checks, general (no framework name, no selector this repo's own web
 * platform doesn't define):
 *
 *   expando-persist   an expando set on a freshly created, appended element
 *                     from inside a function that retains no reference to it
 *                     must still read back correctly through a LATER,
 *                     independently obtained reference to the same node.
 *   delegated-click    the real-world shape: one document-level listener,
 *                     the handler stashed as an expando at append time in a
 *                     function that does not keep it alive, a REAL click
 *                     routed through __probeClick (mousedown/mouseup/click,
 *                     the same C entry point browser.c's own mouse path
 *                     uses -- see jsfb_drive.js's header for why that
 *                     matters over el.click()).
 *
 * MUST FAIL TODAY. Run it and watch:
 *   build/webapi_probe --docroot=tests/fixtures --json --drive \
 *     tests/unit/domlife_drive.js domlife=tests/fixtures/domlife
 * Expect:  #DOMLIFE  op  expando-persist  FAIL  want=alpha got=undefined
 *          #DOMLIFE  op  delegated-click  FAIL  handler lost: ...
 * The day both read PASS, the fix above landed; wire this into a `make
 * test-domlife` gate at that point (deferred here for the same ownership
 * reason as the fix itself -- the Makefile was also under active edit).
 */
(function () {
  "use strict";

  function emit(kind, a, b, c) {
    console.log("#DOMLIFE\t" + kind + "\t" + a + "\t" + b + "\t" + (c === undefined ? "" : c));
  }

  var root = document.getElementById("root");
  if (!root) { emit("op", "fixture", "FAIL", "no #root in the page"); return; }

  /* ---- check 1: pure property round-trip, no events involved at all ---- */
  (function attachNoRetain() {
    var el = document.createElement("button");
    el.id = "probe1";
    root.appendChild(el);
    el.$$mark = "alpha";
    /* `el` goes out of scope here and is retained nowhere else. */
  })();
  var again = document.getElementById("probe1");
  var got = again ? again.$$mark : undefined;
  emit("op", "expando-persist", got === "alpha" ? "PASS" : "FAIL",
       "want=alpha got=" + got);

  /* ---- check 2: the real shape -- delegated click, expando handler ---- */
  var ranHandler = false;
  document.addEventListener("click", function (e) {
    var t = e.target;
    while (t) {
      if (t.$$act) { t.$$act(); break; }
      t = t.parentNode;
    }
  });
  (function attachDelegated() {
    var el = document.createElement("button");
    el.id = "probe2";
    root.appendChild(el);
    el.$$act = function () { ranHandler = true; };
  })();
  var target = document.getElementById("probe2");
  __probeClick(target);
  emit("op", "delegated-click", ranHandler ? "PASS" : "FAIL",
       ranHandler ? "handler ran"
                  : "handler lost: the expando set at append time was gone by click time");

  emit("done", "2", "", "");
})();
