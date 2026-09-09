// The questions, asked of whatever Intl is installed above. On node that is
// the real ICU-backed Intl; in the browser it is js_intl.c's prelude, which
// short-circuits on `if (G.Intl) return;` so node keeps its own.
//
// The brief's standard: A STUB THAT RETURNS THE WRONG STRING IS WORSE THAN A
// MISSING CONSTRUCTOR, so every row asks the object to FORMAT something.
function t(tag, f) {
  var r;
  try { r = f(); } catch (e) { r = "THREW " + e.name + ": " + e.message; }
  print(tag + " = " + String(r));
}

t("Intl present", function () { return typeof Intl; });
t("constructors", function () {
  return ["NumberFormat", "DateTimeFormat", "Collator", "PluralRules", "ListFormat", "RelativeTimeFormat", "Segmenter", "DisplayNames", "Locale", "getCanonicalLocales"]
    .map(function (k) { return k + ":" + (typeof Intl[k]); }).join(" ");
});

t("NF plain", function () { return new Intl.NumberFormat("en-US").format(1234567.891); });
t("NF small", function () { return new Intl.NumberFormat("en-US").format(12.5); });
t("NF integer", function () { return new Intl.NumberFormat("en-US").format(1000); });
t("NF negative", function () { return new Intl.NumberFormat("en-US").format(-1234.5); });
t("NF zero and -0", function () { return new Intl.NumberFormat("en-US").format(0) + "|" + new Intl.NumberFormat("en-US").format(-0); });
t("NF NaN/Infinity", function () { return new Intl.NumberFormat("en-US").format(NaN) + "|" + new Intl.NumberFormat("en-US").format(Infinity); });
t("NF default max 3 frac", function () { return new Intl.NumberFormat("en-US").format(1.23456789); });
t("NF min/max fraction digits", function () { return new Intl.NumberFormat("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 }).format(3); });
t("NF minimumIntegerDigits", function () { return new Intl.NumberFormat("en-US", { minimumIntegerDigits: 3 }).format(7); });
t("NF useGrouping false", function () { return new Intl.NumberFormat("en-US", { useGrouping: false }).format(1234567); });
t("NF currency USD", function () { return new Intl.NumberFormat("en-US", { style: "currency", currency: "USD" }).format(1234.5); });
t("NF currency USD negative", function () { return new Intl.NumberFormat("en-US", { style: "currency", currency: "USD" }).format(-1234.5); });
t("NF currency EUR", function () { return new Intl.NumberFormat("en-US", { style: "currency", currency: "EUR" }).format(1234.5); });
t("NF currency JPY (0 decimals)", function () { return new Intl.NumberFormat("en-US", { style: "currency", currency: "JPY" }).format(1234); });
t("NF percent", function () { return new Intl.NumberFormat("en-US", { style: "percent" }).format(0.256); });
t("NF percent integer", function () { return new Intl.NumberFormat("en-US", { style: "percent" }).format(0.5); });
t("NF unit", function () { return new Intl.NumberFormat("en-US", { style: "unit", unit: "byte" }).format(5); });
t("NF compact", function () { return new Intl.NumberFormat("en-US", { notation: "compact" }).format(1234567); });
t("NF signDisplay always", function () { return new Intl.NumberFormat("en-US", { signDisplay: "always" }).format(5); });
t("NF formatToParts", function () { return typeof new Intl.NumberFormat("en-US").formatToParts === "function" ? JSON.stringify(new Intl.NumberFormat("en-US").formatToParts(1234.5)) : "absent"; });
t("NF format is bound", function () {
  var f = new Intl.NumberFormat("en-US").format;
  return f(1234.5);
});
t("NF resolvedOptions", function () { var o = new Intl.NumberFormat("en-US").resolvedOptions(); return o.locale + "|" + o.numberingSystem + "|" + o.style; });
t("NF de-DE (locale data honesty)", function () { return new Intl.NumberFormat("de-DE").format(1234567.891); });
t("NF without new", function () { return Intl.NumberFormat("en-US").format(1000); });

t("DTF default", function () { return new Intl.DateTimeFormat("en-US", { timeZone: "UTC" }).format(new Date(Date.UTC(2024, 0, 13))); });
t("DTF long month", function () { return new Intl.DateTimeFormat("en-US", { timeZone: "UTC", year: "numeric", month: "long", day: "numeric" }).format(new Date(Date.UTC(2024, 0, 13))); });
t("DTF short month", function () { return new Intl.DateTimeFormat("en-US", { timeZone: "UTC", month: "short", day: "numeric" }).format(new Date(Date.UTC(2024, 0, 13))); });
t("DTF weekday", function () { return new Intl.DateTimeFormat("en-US", { timeZone: "UTC", weekday: "long" }).format(new Date(Date.UTC(2024, 0, 13))); });
t("DTF time", function () { return new Intl.DateTimeFormat("en-US", { timeZone: "UTC", hour: "numeric", minute: "2-digit" }).format(new Date(Date.UTC(2024, 0, 13, 15, 4))); });
t("DTF hour12 false", function () { return new Intl.DateTimeFormat("en-US", { timeZone: "UTC", hour: "2-digit", minute: "2-digit", hour12: false }).format(new Date(Date.UTC(2024, 0, 13, 15, 4))); });
t("DTF formatToParts", function () { return typeof new Intl.DateTimeFormat("en-US").formatToParts === "function" ? "function" : "absent"; });
t("DTF resolvedOptions", function () { var o = new Intl.DateTimeFormat("en-US").resolvedOptions(); return o.locale + "|" + o.timeZone; });
t("DTF non-UTC timezone request", function () { return new Intl.DateTimeFormat("en-US", { timeZone: "America/New_York" }).format(new Date(Date.UTC(2024, 0, 13, 2, 0))); });

t("Collator compare", function () { var c = new Intl.Collator("en"); return [c.compare("a", "b"), c.compare("b", "a"), c.compare("a", "a")].join(","); });
t("Collator sorts accents", function () { return ["z", "ä", "a"].sort(new Intl.Collator("en").compare).join(""); });
t("Collator sorts case", function () { return ["b", "A", "a", "B"].sort(new Intl.Collator("en").compare).join(""); });
t("Collator numeric", function () { return ["10", "9", "2"].sort(new Intl.Collator("en", { numeric: true }).compare).join(","); });
t("PluralRules en", function () { var p = new Intl.PluralRules("en-US"); return [p.select(0), p.select(1), p.select(2), p.select(1.5)].join(","); });
t("PluralRules ordinal", function () { var p = new Intl.PluralRules("en-US", { type: "ordinal" }); return [p.select(1), p.select(2), p.select(3), p.select(4)].join(","); });
t("ListFormat conjunction", function () { return new Intl.ListFormat("en").format(["a", "b", "c"]); });
t("ListFormat two", function () { return new Intl.ListFormat("en").format(["a", "b"]); });
t("ListFormat disjunction", function () { return new Intl.ListFormat("en", { type: "disjunction" }).format(["a", "b", "c"]); });
t("RelativeTimeFormat past", function () { return new Intl.RelativeTimeFormat("en").format(-1, "day"); });
t("RelativeTimeFormat future plural", function () { return new Intl.RelativeTimeFormat("en").format(3, "hour"); });
t("RelativeTimeFormat numeric auto", function () { return new Intl.RelativeTimeFormat("en", { numeric: "auto" }).format(-1, "day"); });
t("getCanonicalLocales", function () { return JSON.stringify(Intl.getCanonicalLocales("EN-us")); });

t("Number.toLocaleString default", function () { return (1234567.891).toLocaleString(); });
t("Number.toLocaleString currency", function () { return (1234.5).toLocaleString("en-US", { style: "currency", currency: "USD" }); });
t("Number.toLocaleString percent", function () { return (0.25).toLocaleString("en-US", { style: "percent" }); });
t("Date.toLocaleDateString", function () { return new Date(Date.UTC(2024, 0, 13)).toLocaleDateString("en-US", { timeZone: "UTC" }); });
t("Date.toLocaleTimeString", function () { return new Date(Date.UTC(2024, 0, 13, 5, 6, 7)).toLocaleTimeString("en-US", { timeZone: "UTC" }); });
t("Date.toLocaleString", function () { return new Date(Date.UTC(2024, 0, 13, 5, 6, 7)).toLocaleString("en-US", { timeZone: "UTC" }); });
t("String.localeCompare via Collator", function () { return String("a".localeCompare("b")); });

t("SuppressedError", function () {
  if (typeof SuppressedError !== "function") return "absent";
  var e = new SuppressedError(new Error("e"), new Error("s"), "m");
  return e.name + "|" + e.message + "|" + String(e instanceof Error) + "|" + String(e.error && e.error.message) + "|" + String(e.suppressed && e.suppressed.message);
});
