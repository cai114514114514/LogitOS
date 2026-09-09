// Intl and structuredClone.
// A STUB THAT RETURNS THE WRONG STRING IS WORSE THAN A MISSING CONSTRUCTOR,
// so every Intl row below asks the object to FORMAT something rather than
// merely asking whether it exists.
function t(tag, f) {
  var r;
  try { r = f(); } catch (e) { r = "THREW " + e.name + ": " + e.message; }
  print(tag + " = " + String(r));
}

t("typeof Intl", function () { return typeof Intl; });
t("Intl own keys", function () { return typeof Intl === "undefined" ? "n/a" : Object.getOwnPropertyNames(Intl).sort().join(","); });
t("NumberFormat exists", function () { return typeof Intl !== "undefined" && typeof Intl.NumberFormat; });
t("NumberFormat plain", function () { return new Intl.NumberFormat("en-US").format(1234567.891); });
t("NumberFormat currency", function () { return new Intl.NumberFormat("en-US", { style: "currency", currency: "USD" }).format(1234.5); });
t("NumberFormat percent", function () { return new Intl.NumberFormat("en-US", { style: "percent" }).format(0.256); });
t("NumberFormat minimumFractionDigits", function () { return new Intl.NumberFormat("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 }).format(1.005); });
t("NumberFormat de-DE", function () { return new Intl.NumberFormat("de-DE").format(1234567.891); });
t("NumberFormat compact", function () { return new Intl.NumberFormat("en-US", { notation: "compact" }).format(1234567); });
t("NumberFormat formatToParts", function () { return JSON.stringify(new Intl.NumberFormat("en-US").formatToParts(1234.5)); });
t("NumberFormat resolvedOptions locale", function () { return new Intl.NumberFormat("en-US").resolvedOptions().locale; });
t("DateTimeFormat exists", function () { return typeof Intl !== "undefined" && typeof Intl.DateTimeFormat; });
t("DateTimeFormat UTC", function () { return new Intl.DateTimeFormat("en-US", { timeZone: "UTC" }).format(new Date(Date.UTC(2024, 0, 13))); });
t("DateTimeFormat options", function () {
  return new Intl.DateTimeFormat("en-US", { timeZone: "UTC", year: "numeric", month: "long", day: "numeric" }).format(new Date(Date.UTC(2024, 0, 13)));
});
t("DateTimeFormat resolvedOptions tz", function () { return new Intl.DateTimeFormat("en-US", { timeZone: "UTC" }).resolvedOptions().timeZone; });
t("Collator", function () {
  var c = new Intl.Collator("en");
  return String(c.compare("a", "b")) + "," + String(["b", "a", "C"].sort(c.compare).join(""));
});
t("RelativeTimeFormat", function () { return new Intl.RelativeTimeFormat("en").format(-1, "day"); });
t("PluralRules", function () { return new Intl.PluralRules("en-US").select(1) + "," + new Intl.PluralRules("en-US").select(2); });
t("ListFormat", function () { return new Intl.ListFormat("en").format(["a", "b", "c"]); });
t("Segmenter", function () { return Array.from(new Intl.Segmenter("en", { granularity: "grapheme" }).segment("a\u{1F600}")).length; });
t("getCanonicalLocales", function () { return JSON.stringify(Intl.getCanonicalLocales("EN-us")); });
t("Intl.Locale", function () { return new Intl.Locale("en-US").language; });
t("String.prototype.localeCompare with Intl options", function () { return String("a".localeCompare("B", "en", { sensitivity: "base" })); });
t("Number.toLocaleString with options", function () { return (1234.5).toLocaleString("en-US", { style: "currency", currency: "EUR" }); });
t("Date.toLocaleDateString with locale", function () { return new Date(Date.UTC(2024, 0, 13)).toLocaleDateString("en-US", { timeZone: "UTC" }); });

t("typeof structuredClone", function () { return typeof structuredClone; });
t("clone plain", function () { var o = { a: 1, b: [2, 3] }; var c = structuredClone(o); return JSON.stringify(c) + "|" + String(c === o) + "|" + String(c.b === o.b); });
t("clone cyclic", function () { var o = {}; o.self = o; var c = structuredClone(o); return String(c.self === c); });
t("clone Map/Set/Date/RegExp", function () {
  var c = structuredClone({ m: new Map([[1, "a"]]), s: new Set([1]), d: new Date(0), r: /a/g });
  return [c.m instanceof Map, c.m.get(1), c.s.has(1), c.d instanceof Date && c.d.getTime(), String(c.r)].join("|");
});
t("clone function throws", function () { return structuredClone(function () {}); });
t("clone symbol throws", function () { return structuredClone(Symbol("x")); });
t("clone shared reference identity", function () { var s = { v: 1 }; var c = structuredClone([s, s]); return String(c[0] === c[1]); });
t("clone ArrayBuffer", function () { var b = new ArrayBuffer(4); var c = structuredClone(b); return String(c.byteLength) + "|" + String(c === b); });
t("clone getter is invoked, prototype dropped", function () {
  function C() { this.a = 1; }
  C.prototype.m = function () {};
  var c = structuredClone(new C());
  return JSON.stringify(c) + "|" + String(c instanceof C) + "|" + String(typeof c.m);
});
t("clone BigInt", function () { return String(structuredClone(1n)); });
