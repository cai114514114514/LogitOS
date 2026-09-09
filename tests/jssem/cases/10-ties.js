// Decimal rounding at an EXACT TIE.
//
// This file exists because the host build of this tree's QuickJS and the guest
// build disagreed here, which is the one outcome that outranks any single
// finding: it means the engine a host gate measures is not the engine the
// browser runs. Every value below is a double that is EXACTLY representable
// and lands exactly halfway, so there is no float noise in the answer -- the
// spec (ES2024 21.1.3.3, Number.prototype.toFixed step 5: "If there are two
// such n, pick the LARGER n") mandates ties-away-from-zero, and a ties-to-even
// implementation differs on exactly the ties whose lower neighbour is even.
function t(tag, f) {
  var r;
  try { r = f(); } catch (e) { r = "THREW " + e.name + ": " + e.message; }
  print(tag + " = " + String(r));
}

t("toFixed(0) on x.5, 0..8", function () {
  var out = [];
  for (var i = 0; i <= 8; i++) out.push((i + 0.5).toFixed(0));
  return out.join(",");
});
t("toFixed(0) on -x.5, 0..8", function () {
  var out = [];
  for (var i = 0; i <= 8; i++) out.push((-(i + 0.5)).toFixed(0));
  return out.join(",");
});
t("toFixed(1) on x.x5 exact eighths", function () {
  // n/8 always terminates in binary, so .125/.375/.625/.875 are exact.
  return [(0.25).toFixed(1), (0.75).toFixed(1), (1.25).toFixed(1), (1.75).toFixed(1), (2.25).toFixed(1), (2.75).toFixed(1)].join(",");
});
t("toFixed(2) on exact eighths", function () {
  return [(0.125).toFixed(2), (0.375).toFixed(2), (0.625).toFixed(2), (0.875).toFixed(2), (1.125).toFixed(2), (8.125).toFixed(2)].join(",");
});
t("toFixed(2) money-shaped", function () {
  return [(1.005).toFixed(2), (1.015).toFixed(2), (1.025).toFixed(2), (10.235).toFixed(2), (19.995).toFixed(2)].join(",");
});
t("toPrecision ties", function () {
  return [(1.25).toPrecision(2), (1.75).toPrecision(2), (2.25).toPrecision(2), (0.125).toPrecision(2), (12.5).toPrecision(2), (1.5).toPrecision(1)].join(",");
});
t("toExponential ties", function () {
  return [(1.25).toExponential(1), (1.75).toExponential(1), (0.125).toExponential(2), (2.5).toExponential(0)].join(",");
});
t("Math.round for contrast (always ties-up)", function () {
  var out = [];
  for (var i = 0; i <= 4; i++) out.push(Math.round(i + 0.5));
  return out.join(",") + "|" + [Math.round(-0.5), Math.round(-1.5), Math.round(-2.5)].join(",");
});
t("default ToString is unaffected", function () {
  return [String(2.5), String(0.5), String(1.005), String(0.1 + 0.2)].join("|");
});
t("toFixed matches manual round-half-up", function () {
  // The oracle inside the oracle: what a page's own currency helper computes.
  function half_up(x, d) { var p = Math.pow(10, d); return String(Math.round(x * p) / p); }
  var bad = [];
  var vals = [0.5, 1.5, 2.5, 3.5, 4.5, 0.125, 0.375, 1.125, 8.125];
  for (var i = 0; i < vals.length; i++) {
    var d = vals[i] < 1 ? 2 : 0;
    var a = vals[i].toFixed(d);
    var b = half_up(vals[i], d);
    if (Number(a) !== Number(b)) bad.push(vals[i] + ":toFixed=" + a + " Math.round=" + b);
  }
  return bad.length ? bad.join(" ") : "agree on all " + vals.length;
});
