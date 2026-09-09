// Number and String formatting. A wrong toFixed or a wrong default
// Number->String is wrong TEXT ON THE SCREEN, and nothing else looks broken.
function t(tag, f) {
  var r;
  try { r = f(); } catch (e) { r = "THREW " + e.name + ": " + e.message; }
  print(tag + " = " + String(r));
}

t("default ToString", function () {
  return [0.1 + 0.2, 1e21, 1e-7, 123456789012345680000, 5e-324, 1 / 3, -0].map(String).join("|");
});
t("ToString(-0)", function () { return String(-0) + "|" + (-0).toFixed(1) + "|" + String(Object.is(-0, -0)); });
t("integer boundary", function () {
  return [Number.MAX_SAFE_INTEGER, Number.MIN_SAFE_INTEGER, Number.EPSILON, Number.MAX_VALUE, Number.MIN_VALUE].map(String).join("|");
});
t("toFixed", function () {
  return [(1.005).toFixed(2), (1.45).toFixed(1), (2.5).toFixed(0), (1.5).toFixed(0), (-1.5).toFixed(0), (0).toFixed(2), (1e21).toFixed(2), (1.255).toFixed(2)].join("|");
});
t("toFixed 100 / range", function () {
  var a;
  try { a = (1).toFixed(101); } catch (e) { a = e.name; }
  return (1.1).toFixed(20) + "|" + a;
});
t("toPrecision", function () {
  return [(123.456).toPrecision(2), (0.000123).toPrecision(2), (123456).toPrecision(2), (0).toPrecision(3), (1.25).toPrecision(2), (1.35).toPrecision(2)].join("|");
});
t("toExponential", function () {
  return [(123456).toExponential(2), (0.00012).toExponential(), (0).toExponential(2), (-1.5).toExponential(0)].join("|");
});
t("toString radix", function () {
  return [(255).toString(16), (255).toString(2), (0.5).toString(2), (-255).toString(36), (1e21).toString(16), (0.1).toString(3)].join("|");
});
t("parseFloat / parseInt", function () {
  return [parseFloat("1.5e3abc"), parseInt("0x1f"), parseInt("08"), parseInt("  -12px"), parseInt("1e3"), parseFloat(".5"), parseFloat("Infinityx")].map(String).join("|");
});
t("Number() coercions", function () {
  return [Number(""), Number(" \n42 "), Number("0b101"), Number("0o17"), Number("1_000"), Number([]), Number([5]), Number([1, 2]), Number(null), Number(undefined), Number(true)].map(String).join("|");
});
t("Number.isInteger etc", function () {
  return [Number.isInteger(1.0), Number.isSafeInteger(2 ** 53), Number.isFinite("1"), isFinite("1"), Number.isNaN("x"), isNaN("x")].map(String).join("|");
});
t("Math edge", function () {
  return [Math.round(-0.5), Math.round(0.5), Math.round(2.5), Math.round(-2.5), Math.sign(-0), Math.trunc(-0.9), Math.hypot(3, 4), Math.cbrt(-8), Math.fround(1.1), Math.clz32(1), Math.imul(3, 4)].map(String).join("|");
});
t("Math.max/min empty", function () { return String(Math.max()) + "|" + String(Math.min()) + "|" + String(Math.max(NaN, 1)); });
t("exp/log precision", function () {
  return [Math.log2(8), Math.log10(1000), Math.expm1(0), Math.log1p(0), Math.atan2(1, 1), Math.sinh(1), Math.tanh(1), Math.acosh(2)].map(String).join("|");
});
t("** precedence & bigint-free", function () { return String((-2) ** 2) + "|" + String(2 ** 3 ** 2); });

t("padStart/padEnd", function () {
  return ["5".padStart(3, "0"), "ab".padEnd(5, "xy"), "abc".padStart(2, "0"), "a".padStart(4)].join("|");
});
t("trimStart/trimEnd", function () { return "[" + "  a  ".trimStart() + "][" + "  a  ".trimEnd() + "]"; });
t("replaceAll", function () {
  return "a.b.c".replaceAll(".", "-") + "|" + "aaa".replaceAll("a", function (m, i) { return String(i); }) + "|" + "x".replaceAll("", "-");
});
t("replaceAll non-global regexp", function () { return "aa".replaceAll(/a/, "b"); });
t("replace $ patterns", function () {
  return "abc".replace("b", "[$&|$`|$'|$$]") + "|" + "abc".replace(/(b)/, "<$1><$0>");
});
t("split limits", function () {
  return JSON.stringify("a,b,c".split(",", 2)) + "|" + JSON.stringify("abc".split("")) + "|" + JSON.stringify("".split(",")) + "|" + JSON.stringify("".split(""));
});
t("codePointAt / fromCodePoint", function () {
  return String("\u{1F600}".codePointAt(0)) + "|" + String("\u{1F600}".length) + "|" + String(String.fromCodePoint(0x1f600).length) + "|" + String("\u{1F600}".charCodeAt(0));
});
t("normalize", function () {
  if (typeof "".normalize !== "function") return "absent";
  var d = "é";
  return String(d.normalize("NFC").length) + "|" + String(d.normalize("NFC") === "é") + "|" + String("é".normalize("NFD").length);
});
t("localeCompare", function () {
  return [("a").localeCompare("b"), ("b").localeCompare("a"), ("a").localeCompare("a"), ("ä").localeCompare("z")].join("|");
});
t("toLocaleString number", function () {
  return (1234567.891).toLocaleString() + "|" + (0).toLocaleString();
});
t("toLowerCase specials", function () {
  return "İ".toLowerCase().length + "|" + "ß".toUpperCase() + "|" + "ABC".toLowerCase();
});
t("String.raw", function () { return String.raw({ raw: ["a", "b"] }, 1) + "|" + String.raw`x\ny`; });
t("repeat", function () { return "ab".repeat(0) + "|" + "ab".repeat(2) + "|" + (function () { try { return "a".repeat(-1); } catch (e) { return e.name; } })(); });
t("string comparison", function () { return String("a" < "b") + "," + String("Z" < "a") + "," + String("10" < "9"); });
t("template + toString order", function () {
  var o = { toString: function () { return "TS"; }, valueOf: function () { return "VO"; } };
  return `${o}` + "|" + (o + "") + "|" + String(o);
});
t("Symbol.toPrimitive", function () {
  var o = {}; o[Symbol.toPrimitive] = function (h) { return "hint:" + h; };
  return `${o}` + "|" + (o + "") + "|" + String(+"" ? 1 : (function () { try { return o * 1; } catch (e) { return e.name; } })());
});
