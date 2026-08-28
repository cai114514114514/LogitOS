/* jsfb_drive.js -- press the eight buttons and read the table back.
 *
 * Loaded by `build/webapi_probe --drive tests/unit/jsfb_drive.js <fixture>`,
 * evaluated in the page's OWN context at the settle point, in channel 2 only.
 * See the --drive comment in tests/unit/webapi_probe.c for why the click
 * primitive is native rather than el.click().
 *
 * ============================ THE PROHIBITION ============================
 * NOTHING IN THIS FILE MAY NAME AN IMPLEMENTATION, A FRAMEWORK, A BUNDLE OR A
 * HOST. Every id, selector and class below comes from the js-framework-
 * benchmark SPECIFICATION -- the contract all 253 implementations were written
 * against -- and from nowhere else. That is the same standing
 * tests/fixtures/frameworks/_paint/paint.js has when it reads `#inc`: a shared
 * contract is not a special case.
 *
 * HOW YOU WOULD KNOW IF IT HAD DRIFTED, stated so it can be checked rather
 * than promised:
 *
 *   1. There is no branch on any implementation's identity anywhere in this
 *      file. `grep -nE "keyed/|vanilla|solid|svelte|react|vue|angular" ` over
 *      it returns nothing. One branch would be visible as one branch.
 *   2. The SAME bytes run against every implementation. A fix that helps one
 *      row and no other is a fix to that row, and the matrix shows it as one
 *      row moving.
 *   3. The null control must fail all eight. A driver that has been shaped to
 *      the pages cannot report a page with no behaviour as broken -- see
 *      jsfb_matrix.py, which refuses to print a matrix whose control passed.
 *   4. Six implementations in this corpus are JavaScript EMITTED BY A COMPILER
 *      from Scala, Haskell, PureScript or OCaml. They exercise the DOM in
 *      shapes no hand-written bundle produces. If they score like the
 *      hand-written ones, this is measuring the platform. If this file had
 *      been shaped to hand-written output, they are where it shows.
 *
 * ============================ WHAT IS ASSERTED ==========================
 * The eight operations of the specified application, each with the assertion
 * the specification gives it, checked against the DOM the page actually built:
 *
 *   run       #run       -> 1,000 rows
 *   update    #update    -> every 10th row's label gained " !!!", others did not
 *   select    row 2's label link -> exactly one tr.danger, and it is row 2
 *   swaprows  #swaprows  -> rows 2 and 999 exchanged labels
 *   remove    row 4's remove link -> 999 rows, row 4 now holds old row 5
 *   add       #add       -> 1,000 more rows
 *   clear     #clear     -> 0 rows
 *   runlots   #runlots   -> 10,000 rows
 *
 * They run in that order in ONE page load, because a probe run is one load.
 * Every result line therefore carries the row count it started from, so a
 * cascade (op 3 failing because op 1 never ran) is readable as a cascade
 * instead of being counted as three independent failures.
 *
 * Output is one `#JSFB` line per operation on stdout. jsfb_matrix.py parses it.
 */
(function () {
  "use strict";

  function emit(kind, a, b, c) {
    console.log("#JSFB\t" + kind + "\t" + a + "\t" + b + "\t" + (c === undefined ? "" : c));
  }

  /* ---- the page's furniture, located once ------------------------------
   * Located by the specification's ids and classes. Whether each was found is
   * REPORTED, not assumed: a control that fails because the buttons are
   * missing is failing for a corpus reason, and one that fails because
   * pressing them does nothing is failing for the reason the control exists.
   * Those two must never be the same line. */
  var BUTTONS = ["run", "runlots", "add", "update", "clear", "swaprows"];

  function byId(id) { try { return document.getElementById(id); } catch (e) { return null; } }

  var found = [], missing = [];
  for (var i = 0; i < BUTTONS.length; i++)
    (byId(BUTTONS[i]) ? found : missing).push(BUTTONS[i]);

  /* The table. `table.test-data tbody` is the specification's own markup; the
   * bare `tbody` fallback is recorded when it is used, because a page with two
   * tbodys would make the two selectors mean different things. */
  var tbody = null, tbodyHow = "none";
  try {
    tbody = document.querySelector("table.test-data tbody");
    if (tbody) tbodyHow = "table.test-data tbody";
    else { tbody = document.querySelector("tbody"); if (tbody) tbodyHow = "tbody"; }
  } catch (e) { tbodyHow = "threw: " + e; }

  /* TWO WAYS TO COUNT THE SAME ROWS, and they are compared on every read.
   * One is the selector engine (js_select.c), the other is a plain child walk
   * over the DOM. If a matrix were built on the selector alone, a selector bug
   * would be reported as thirty broken applications. When they disagree the
   * WALK is used -- it depends on less -- and the disagreement is emitted. */
  var disagreements = 0;

  function rowsWalk() {
    var out = [];
    if (!tbody) return out;
    for (var n = tbody.firstElementChild; n; n = n.nextElementSibling)
      if (n.tagName === "TR") out.push(n);
    return out;
  }
  function rowsSel() {
    try { return document.querySelectorAll("table.test-data tbody tr").length; }
    catch (e) { return -1; }
  }
  function rows() {
    var w = rowsWalk(), s = rowsSel();
    if (s >= 0 && s !== w.length) {
      disagreements++;
      emit("disagree", "querySelectorAll=" + s, "childWalk=" + w.length, "");
    }
    return w;
  }

  /* The label cell is `td:nth-child(2)`, and the specification puts an <a>
   * inside it because that anchor is what `select` is clicked on. Reading the
   * cell's text when the anchor is absent is the same cell, and which one was
   * read is reported. */
  var labelHow = "unknown";
  function label(tr) {
    if (!tr) return null;
    var tds = [], n;
    for (n = tr.firstElementChild; n; n = n.nextElementSibling)
      if (n.tagName === "TD") tds.push(n);
    if (tds.length < 2) return null;
    var a = null;
    for (n = tds[1].firstElementChild; n; n = n.nextElementSibling)
      if (n.tagName === "A") { a = n; break; }
    if (a) { if (labelHow === "unknown") labelHow = "td2>a"; return (a.textContent || "").trim(); }
    if (labelHow === "unknown") labelHow = "td2 (no <a>)";
    return (tds[1].textContent || "").trim();
  }
  /* The clickable target inside a cell. Column 2 selects, column 3 removes.
   *
   * IT DESCENDS TO THE INNERMOST ELEMENT, AND THAT IS NOT A DETAIL -- it was a
   * defect in the first version of this file, caught by three implementations
   * that passed all seven other operations and failed `remove`.  A real click
   * hit-tests the pixel: browser.c calls browser_hittest_node(), which returns
   * the DEEPEST element under the pointer and dispatches there.  The remove
   * cell in the specification's markup is `<a><span class="glyphicon
   * glyphicon-remove"></span></a>`, so the span is what a person hits and the
   * span is what event.target is.  Clicking the anchor instead delivers an
   * event no user can produce, and an implementation whose handler reads
   * e.target reads something that never happens.  The label cell has no inner
   * element, so this changes nothing there -- which is why only `remove`
   * moved. */
  function innermost(el) {
    while (el && el.firstElementChild) el = el.firstElementChild;
    return el;
  }
  function cellTarget(tr, nth) {
    if (!tr) return null;
    var tds = [], n;
    for (n = tr.firstElementChild; n; n = n.nextElementSibling)
      if (n.tagName === "TD") tds.push(n);
    if (tds.length < nth) return null;
    return innermost(tds[nth - 1]) || tds[nth - 1];
  }

  function press(id) {
    var el = byId(id);
    if (!el) return "no #" + id;
    __probeClick(el);
    return null;
  }

  emit("env", "buttons=" + found.length + "/" + BUTTONS.length +
       (missing.length ? " missing:" + missing.join(",") : ""),
       "tbody=" + tbodyHow, "rows0=" + rows().length);

  /* ---- the eight -------------------------------------------------------
   *
   * THREE VERDICTS, AND THE THIRD IS THERE BECAUSE THE CONTROL FOUND A HOLE.
   * The first version of this file scored `clear` as PASS on the null control:
   * clear asserts "0 rows afterwards", and a table that was never filled
   * already has 0.  A check that passes on a page with no behaviour at all is
   * this tree's fifth rule exactly -- "a control that cannot be watched
   * failing is worse than no control, because it reads like one".
   *
   * So an operation whose precondition the previous operations failed to
   * establish is NOPRE, never PASS.  It is not a pass and it is not evidence of
   * a defect in that operation either; it is a cascade, and printing it as one
   * is what keeps eight failures from being reported where there is one.
   * Operations with NO precondition -- run, add, runlots, which are defined
   * from any state -- can never be NOPRE, so the control still has three
   * genuine failures to be watched failing. */
  var ops = [];

  function op(name, pre, fn) {
    ops.push(name);
    var before = rows().length;
    var verdict, detail;
    var why = pre ? pre(before) : null;
    if (why) {
      verdict = "NOPRE";
      detail = why;
    } else {
      try {
        var r = fn(before);
        verdict = r.ok ? "PASS" : "FAIL";
        detail = r.detail;
      } catch (e) {
        verdict = "FAIL";
        detail = "threw " + (e && e.message ? (e.name || "Error") + ": " + e.message : String(e));
      }
    }
    emit("op", name, verdict, "from=" + before + " " + detail);
  }

  function atLeast(n, what) {
    return function (before) {
      return before >= n ? null : "needs >= " + n + " rows (" + what + "), have " + before;
    };
  }

  /* The id cell is `td:nth-child(1)`. It is read for ONE reason -- see `run`
   * below -- and never asserted on directly, because the specification fixes
   * neither the starting id nor that ids restart between operations. */
  function idText(tr) {
    if (!tr) return null;
    var n = tr.firstElementChild;
    while (n && n.tagName !== "TD") n = n.nextElementSibling;
    return n ? (n.textContent || "").trim() : null;
  }

  /* WHY `run` ASSERTS A TRANSITION AND NOT JUST A COUNT.
   *
   * "1,000 rows afterwards" is true of a page that pressed the button and
   * built them, AND of a page that was BORN holding 1,000 rows and ignored the
   * click entirely.  The null control cannot tell those apart -- it starts at
   * zero, so it fails for the count alone and the hole stays open behind a
   * green control.  That is this tree's fifth rule in its exact shape: the
   * check was not wrong, it was unwatchable.
   *
   * So the second control (jsfb_matrix.py: the same skeleton, plus a script
   * that renders 1,000 conforming rows at load and binds NO handler) is the
   * page this disjunct exists for, and it is watched FAILING there.
   *
   * The assertion stays general.  `before !== 1000` is enough for any page
   * that was not already at exactly the target count -- which is every
   * implementation in this corpus, all of which load with 0 or 1 rows -- so
   * strengthening this moves no row of the matrix, and that was measured
   * rather than assumed.  Only when the count did not have to move does the id
   * of the first row have to: the specification's rows carry generated ids, so
   * a table that was genuinely re-created cannot present the same first id.
   * A node-POOLING implementation recycles the row ELEMENTS, which is why the
   * comparison is on the id TEXT and not on node identity -- comparing nodes
   * would report a legitimate optimisation as a broken application. */
  op("run", null, function (before) {
    var id0 = idText(rows()[0]);
    var e = press("run"); if (e) return { ok: false, detail: e };
    var r = rows(), n = r.length;
    if (n !== 1000) return { ok: false, detail: "rows=" + n + " want=1000" };
    var moved = before !== 1000 || idText(r[0]) !== id0;
    return { ok: moved,
             detail: moved ? "rows=1000"
                           : "rows=1000 but the table did not change (first id "
                             + id0 + " both before and after): these rows were"
                             + " not created by the click" };
  });

  op("update", atLeast(20, "a run table"), function () {
    var r0 = rows();
    var beforeA = label(r0[0]), beforeB = label(r0[1]);
    var e = press("update"); if (e) return { ok: false, detail: e };
    var r = rows();
    if (r.length !== r0.length)
      return { ok: false, detail: "row count moved " + r0.length + "->" + r.length };
    var a = label(r[0]), b = label(r[1]);
    /* Every 10th row (0-based 0, 10, 20 ...) gains " !!!"; the rest do not.
     * Checking only the first would pass an implementation that appended the
     * suffix to every row. */
    var tenthOk = a === beforeA + " !!!";
    var restOk = b === beforeB;
    var n = 0;
    for (var i = 0; i < r.length; i += 10) if (/ !!!$/.test(label(r[i]))) n++;
    var m = 0;
    for (var j = 1; j < r.length; j += 10) if (/ !!!$/.test(label(r[j]))) m++;
    return { ok: tenthOk && restOk && n === Math.ceil(r.length / 10) && m === 0,
             detail: "every10th=" + n + "/" + Math.ceil(r.length / 10) +
                     " others_marked=" + m + " row1:" + JSON.stringify(beforeA) +
                     "->" + JSON.stringify(a) };
  });

  op("select", atLeast(2, "a row to select"), function () {
    var r = rows();
    var t = cellTarget(r[1], 2);
    if (!t) return { ok: false, detail: "row 2 has no label cell" };
    __probeClick(t);
    var r2 = rows(), sel = [], i;
    for (i = 0; i < r2.length; i++)
      if ((" " + (r2[i].className || "") + " ").indexOf(" danger ") >= 0) sel.push(i + 1);
    return { ok: sel.length === 1 && sel[0] === 2,
             detail: "tr.danger at rows [" + sel.join(",") + "] want [2]" };
  });

  op("swaprows", atLeast(999, "rows 2 and 999 must exist"), function () {
    var r = rows();
    var a = label(r[1]), b = label(r[998]);
    var e = press("swaprows"); if (e) return { ok: false, detail: e };
    var r2 = rows();
    if (r2.length !== r.length)
      return { ok: false, detail: "row count moved " + r.length + "->" + r2.length };
    var a2 = label(r2[1]), b2 = label(r2[998]);
    return { ok: a2 === b && b2 === a && a !== b,
             detail: "row2 " + JSON.stringify(a) + "->" + JSON.stringify(a2) +
                     " row999 " + JSON.stringify(b) + "->" + JSON.stringify(b2) };
  });

  op("remove", atLeast(5, "a row to remove and one behind it"), function () {
    var r = rows();
    var n0 = r.length, next = label(r[4]);
    var t = cellTarget(r[3], 3);
    if (!t) return { ok: false, detail: "row 4 has no remove cell" };
    __probeClick(t);
    var r2 = rows();
    var now = label(r2[3]);
    return { ok: r2.length === n0 - 1 && now === next,
             detail: "rows " + n0 + "->" + r2.length + " want " + (n0 - 1) +
                     "; row4 now " + JSON.stringify(now) + " want " + JSON.stringify(next) };
  });

  /* add, run and runlots have NO precondition: each is defined from any state,
   * so each is a check the null control must be watched failing. */
  op("add", null, function () {
    var n0 = rows().length;
    var e = press("add"); if (e) return { ok: false, detail: e };
    var n = rows().length;
    return { ok: n === n0 + 1000, detail: "rows=" + n + " want=" + (n0 + 1000) };
  });

  /* THE VACUOUS ONE. "0 rows afterwards" is already true of a table that was
   * never filled, so clear is only a measurement when there was something to
   * clear. This precondition is the fix the null control forced. */
  op("clear", atLeast(1, "something to clear"), function () {
    var e = press("clear"); if (e) return { ok: false, detail: e };
    var n = rows().length;
    return { ok: n === 0, detail: "rows=" + n + " want=0" };
  });

  op("runlots", null, function () {
    var e = press("runlots"); if (e) return { ok: false, detail: e };
    var n = rows().length;
    return { ok: n === 10000, detail: "rows=" + n + " want=10000" };
  });

  emit("done", ops.length, "labelcell=" + labelHow, "selector_disagreements=" + disagreements);
})();
