/* stamp.js -- the GUEST-CLOCK STAMP every attack/anim fixture carries.
 *
 * A screendump taken from the host cannot know at what guest time it was
 * taken: the serial marker it was scheduled from is read by a host poll, the
 * QMP screendump is a host round trip, and under TCG the guest clock does not
 * track host sleeps (tests/qmp/qmp_anim_page.py measured ~2x on 2026-08-30).
 * So the picture carries its own clock: #clk is a bar whose width is
 * performance.now()/5 px (5 ms per pixel, 1100 px = 5.5 s), rewritten from a
 * requestAnimationFrame loop. The driver reads the bar's width off the PPM
 * and that is the stamp. If the bar itself does not move, the rAF+style path
 * is broken and the stamp is unavailable -- that is a finding, and the driver
 * says so and falls back to the serial marker's host arrival time.
 *
 * The same loop counts delivered frames and their spacing, and prints an
 * ANIM-MARK line once per second for six seconds so the driver can measure
 * the guest-vs-host clock rate from the lines' host arrival times.
 *
 * Nothing here is page-specific; each fixture defines window.__P (its marker
 * prefix) before including this file. */
(function () {
  var bar = document.getElementById('clk');
  var txt = document.getElementById('clkt');
  var frames = 0, last = 0, dts = [];
  var P = window.__P || 'X';
  /* ?nostamp=1 : no rAF loop at all (the bar stays at 0) -- the control for
   *              "does a concurrent rAF restyle loop perturb the CSS clock";
   * ?quiet=1   : no MARK lines -- with the page's own T-markers also off,
   *              NOTHING prints to the console between GO and DONE, so no
   *              status-line repaint can force a full redraw; the driver
   *              then schedules its shots from host time instead. */
  var Q = window.__Q || (typeof location !== 'undefined' && location.search) || '';
  var NOSTAMP = /nostamp=1/.test(Q), QUIET = /quiet=1/.test(Q);
  /* ?stampmode=bar|txt|none : which DOM write the rAF loop performs --
   * measured on the glass, a concurrent rAF loop writing ANOTHER element's
   * style.width made every CSS transition on the page jump to its end
   * state (one.html?prop=op vs ?prop=op&nostamp=1, 2026-09-02). These
   * bisect that: `bar` writes only #clk's width, `txt` only #clkt's text,
   * `none` keeps the rAF loop turning with no DOM write at all. */
  var MODE = (/stampmode=(\w+)/.exec(Q) || [0, 'full'])[1];
  window.__QUIET = QUIET;
  window.__log = window.__log || [];
  window.__say = function (s) {
    window.__log.push(s);
    try { console.log(s); } catch (e) {}
  };
  function f(t) {
    frames++;
    if (last) dts.push(t - last);
    last = t;
    var ms = performance.now();
    if (bar && (MODE === 'full' || MODE === 'bar')) bar.style.width = Math.min(1100, ms / 5) + 'px';
    if (txt && (MODE === 'full' || MODE === 'txt')) txt.textContent = ms.toFixed(0);
    requestAnimationFrame(f);
  }
  if (!NOSTAMP) requestAnimationFrame(f);
  window.__stamp = {
    frames: function () { return frames; },
    dts: dts,
    stats: function () {
      if (!dts.length) return 'n=0';
      var mn = 1e9, mx = 0, sum = 0;
      for (var i = 0; i < dts.length; i++) { if (dts[i] < mn) mn = dts[i]; if (dts[i] > mx) mx = dts[i]; sum += dts[i]; }
      return 'n=' + dts.length + ' min=' + mn.toFixed(1) + ' mean=' + (sum / dts.length).toFixed(1) + ' max=' + mx.toFixed(1);
    }
  };
  if (!QUIET) for (var i = 0; i <= 6; i++) (function (i) {
    setTimeout(function () {
      window.__say('ANIM-' + P + '-MARK i=' + i + ' t=' + performance.now().toFixed(0) +
                   ' frames=' + frames + ' ' + window.__stamp.stats());
    }, i * 1000);
  })(i);
})();
