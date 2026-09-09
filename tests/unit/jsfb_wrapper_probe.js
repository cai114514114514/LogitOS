/* jsfb_wrapper_probe.js -- does a DOM node's script-visible identity outlive the
 * last SCRIPT reference to it, while the node is still in the document?
 *
 * Loaded the same way jsfb_drive.js is:
 *   build/webapi_probe --docroot=build/jsfb --drive tests/unit/jsfb_wrapper_probe.js \
 *       NAME=build/jsfb/frameworks/keyed/<impl>
 *
 * WHY THIS EXISTS.  The conformance matrix scores three implementations at 0/8
 * with no exception, no failed request and a fully rendered shell, and the
 * reported cause is that a property a script stored on an element is not there
 * when the same element is reached again through the document.  Every other
 * implementation in the corpus passes.  A cause that only some pages meet is
 * not yet a cause -- it is a correlation -- and this file is the experiment
 * that turns it into one, by asking the SAME question of every page instead of
 * reading each page's source and guessing.
 *
 * NOTHING HERE NAMES AN IMPLEMENTATION, A FRAMEWORK OR A KEY.  The
 * specification's ids (`#run`, `table.test-data tbody`) are the only selectors,
 * exactly as in jsfb_drive.js, and every property name printed below is
 * DISCOVERED with getOwnPropertyNames rather than looked for.  A probe that
 * grepped for a particular framework's private key would answer a question
 * about that framework; this one answers a question about the platform.
 *
 * THE FOUR MEASUREMENTS, and the third is the only one that discriminates:
 *
 *   unheld   an element created here, given a property, inserted into the
 *            document, and then dropped -- no script anywhere holds it.  Read
 *            back through getElementById.  This is the defect itself, on a node
 *            whose whole life is in this file, so a page that never loaded can
 *            still be asked.
 *   held     byte-identical, except one array keeps the reference.  The two
 *            differ in exactly one thing, which is what makes `unheld` a
 *            measurement rather than an observation.
 *   approw   a row THE PAGE built, marked here inside a closure that returns
 *            without keeping it, read back by walking the document.  If the
 *            property survives on a node this file does not hold, then somebody
 *            else holds it -- and that somebody is the page.  This is the
 *            discriminator: it separates "the platform preserves expandos" from
 *            "the platform preserves expandos ON NODES THE PAGE HAPPENS TO KEEP".
 *   keys.*   what is actually on the page's own nodes at rest.  Printed, not
 *            matched: the point is to see whether the page's private bookkeeping
 *            is still readable from a node reached only through the DOM.
 *
 * Output is one `#WRAP` line per measurement on stdout.
 */
(function () {
  "use strict";

  function say() {
    console.log("#WRAP\t" + Array.prototype.slice.call(arguments).join("\t"));
  }

  function tbody() {
    try { return document.querySelector("table.test-data tbody"); }
    catch (e) { return null; }
  }
  /* Reached by walking, never cached: a cached row would be a reference this
   * file holds, which is the one thing the experiment must not do. */
  function firstRow() {
    var t = tbody();
    return t ? t.firstElementChild : null;
  }
  function countRows() {
    var t = tbody(), n = 0;
    if (t) for (var x = t.firstElementChild; x; x = x.nextElementSibling) n++;
    return n;
  }

  /* ---- build a table, so there are page-owned nodes to ask about ---------
   * If the page cannot build one this still reports the first two
   * measurements, which need no page at all. That is deliberate: the platform
   * question must be answerable on a page that is broken for other reasons. */
  var b = null;
  try { b = document.getElementById("run"); } catch (e) { b = null; }
  if (b) __probeClick(b);
  say("rows", countRows());

  /* ---- 1. the defect, on a node nothing holds --------------------------- */
  (function () {
    var d = document.createElement("div");
    d.id = "__wrap_unheld";
    d.__mark = 7;
    document.body.appendChild(d);
  })();
  var u = document.getElementById("__wrap_unheld");
  say("unheld", u ? String(u.__mark) : "node-gone");

  /* ---- 2. the same thing, held: the other direction of the control ------ */
  var HELD = [];
  (function () {
    var d = document.createElement("div");
    d.id = "__wrap_held";
    d.__mark = 7;
    document.body.appendChild(d);
    HELD.push(d);
  })();
  var h = document.getElementById("__wrap_held");
  say("held", h ? String(h.__mark) : "node-gone");

  /* ---- 3. THE DISCRIMINATOR: a node the PAGE built and may still hold ---- */
  (function () {
    var r = firstRow();
    if (r) r.__mark = 7;
  })();
  var r2 = firstRow();
  say("approw", r2 ? String(r2.__mark) : "no-row");

  /* ---- 4. what is on the page's own nodes, discovered not assumed -------- */
  function ownkeys(el) {
    if (!el) return "(null)";
    try {
      var k = Object.getOwnPropertyNames(el);
      return k.length ? k.join("|") : "(none)";
    } catch (e) { return "threw " + e; }
  }
  say("keys.row0", ownkeys(firstRow()));
  say("keys.tbody", ownkeys(tbody()));
  var btn = null;
  try { btn = document.getElementById("run"); } catch (e) {}
  say("keys.button", ownkeys(btn));

  /* The label anchor: the deepest node the select operation's handler reads
   * event.target from, so the most load-bearing node in the whole run. */
  var anchor = null;
  var rr = firstRow();
  if (rr) {
    var td = rr.firstElementChild;
    if (td) td = td.nextElementSibling;
    if (td) anchor = td.firstElementChild;
  }
  say("keys.label-a", ownkeys(anchor));

  /* ---- 5. how far the retention goes, if it goes anywhere ----------------
   * Sampled at three depths rather than counted over all rows: the question is
   * whether retention is universal or only covers the nodes the page touched
   * most recently, and three samples answer that without walking 1,000 nodes
   * and holding every one of them alive in the process of asking. */
  function nth(i) {
    var t = tbody(), x = t ? t.firstElementChild : null;
    while (x && i--) x = x.nextElementSibling;
    return x;
  }
  var n = countRows();
  if (n > 2) {
    say("keys.rowMid", ownkeys(nth(Math.floor(n / 2))));
    say("keys.rowLast", ownkeys(nth(n - 1)));
  }

  /* ---- 6. WHERE THE BOUNDARY RUNS ---------------------------------------
   * An expando is script state stored on the wrapper; a listener, an attribute
   * and a class are engine state stored on the node. If the first is lost and
   * the others are kept, then the loss is not "the node was forgotten" -- it is
   * specifically the script-visible half of the node's identity, and that is a
   * far narrower and more actionable statement.
   *
   * Each is set inside a closure that returns without keeping the element, so
   * every one of them is asked under the same condition that loses the expando.
   * The listener is the load-bearing one: an implementation that binds per
   * element survives regardless of what happens to wrappers, and one that binds
   * once and reads a property off the event target does not. */
  var fired = 0;
  (function () {
    var d = document.createElement("div");
    d.id = "__wrap_l";
    d.className = "marked";
    d.setAttribute("data-mark", "7");
    d.textContent = "text7";
    d.addEventListener("click", function () { fired++; });
  document.body.appendChild(d);
  })();
  var L = document.getElementById("__wrap_l");
  if (L) __probeClick(L);
  L = document.getElementById("__wrap_l");
  say("unheld.listener", fired ? "fired" : "LOST");
  say("unheld.className", L ? String(L.className) : "node-gone");
  say("unheld.attribute", L ? String(L.getAttribute("data-mark")) : "node-gone");
  say("unheld.textContent", L ? String(L.textContent) : "node-gone");
  say("unheld.expando", L ? String(L.__mark) : "node-gone");
})();
