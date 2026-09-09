// THE CONTROL. Rule 5: a control that cannot be watched failing is worse than
// no control. This file is designed so ONE line differs between node and this
// engine for a reason that is neither engine's fault, and the harness must
// report it as a diff. If this case comes back IDENTICAL, the harness is not
// comparing anything and no other result in this run means anything.
function t(tag, f) {
  var r;
  try { r = f(); } catch (e) { r = "THREW " + e.name + ": " + e.message; }
  print(tag + " = " + String(r));
}

// Rows that MUST agree -- they bound the control: if these differ, the
// difference is not the one the control planted.
t("agree: 1+1", function () { return 1 + 1; });
t("agree: [3,1,2].sort()", function () { return JSON.stringify([3, 1, 2].sort()); });
t("agree: typeof null", function () { return typeof null; });

// The planted difference. `deliberate_difference` is defined by the HARNESS,
// not by the engine, and the harness gives the two sides different values on
// purpose. Nothing about the engine is being measured here.
t("PLANTED: harness marker", function () {
  return typeof deliberate_difference === "undefined" ? "MARKER-ABSENT" : deliberate_difference;
});
