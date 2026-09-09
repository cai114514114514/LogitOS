// Date parsing and formatting. No clock is read: every value here comes from
// a literal epoch or a literal string, so the output is deterministic.
// The FIRST row is the timezone, deliberately: if the two sides disagree on
// it, every local-time row below is explained by that one difference and not
// by a parsing bug.
function t(tag, f) {
  var r;
  try { r = f(); } catch (e) { r = "THREW " + e.name + ": " + e.message; }
  print(tag + " = " + String(r));
}

t("tz offset (context, not a check)", function () { return String(new Date(0).getTimezoneOffset()); });

t("epoch toISOString", function () { return new Date(0).toISOString(); });
t("toISOString ms", function () { return new Date(1705104000123).toISOString(); });
t("toISOString negative year", function () { return new Date(-62167219200000).toISOString(); });
t("toISOString extended year", function () { return new Date(Date.UTC(275760, 8, 13)).toISOString(); });
t("toISOString invalid", function () { return new Date(NaN).toISOString(); });
t("toJSON invalid is null", function () { return String(new Date(NaN).toJSON()) + "|" + JSON.stringify({ d: new Date(NaN) }); });

t("parse date-only is UTC", function () { return new Date("2024-01-13").toISOString(); });
t("parse datetime no zone", function () { var d = new Date("2024-01-13T10:20:30"); return String(d.getHours()) + ":" + String(d.getMinutes()) + "|utc=" + String(d.getUTCHours()); });
t("parse with Z", function () { return new Date("2024-01-13T10:20:30Z").toISOString(); });
t("parse with offset", function () { return new Date("2024-01-13T10:20:30+05:30").toISOString(); });
t("parse with millis", function () { return new Date("2024-01-13T10:20:30.5Z").toISOString(); });
t("parse year-month", function () { return new Date("2024-01").toISOString(); });
t("parse year only", function () { return new Date("2024").toISOString(); });
t("parse legacy toString form", function () { return String(Date.parse("Sat Jan 13 2024 00:00:00 GMT+0000")); });
t("parse RFC1123", function () { return String(Date.parse("Sat, 13 Jan 2024 00:00:00 GMT")); });
t("parse US slash form", function () { return String(Date.parse("1/13/2024")); });
t("parse garbage", function () { return String(Date.parse("not a date")); });
t("parse out-of-range month", function () { return String(Date.parse("2024-13-01")); });
t("parse day 32", function () { return String(Date.parse("2024-01-32")); });
t("parse 24:00", function () { return String(Date.parse("2024-01-13T24:00:00Z")); });
t("parse 2-digit year", function () { return String(Date.parse("2/3/04")); });
t("parse ISO with space instead of T", function () { return String(Date.parse("2024-01-13 10:20:30Z")); });

t("Date.UTC", function () { return String(Date.UTC(2024, 0, 13)) + "|" + String(Date.UTC(2024)) + "|" + String(Date.UTC(96, 0, 1)); });
t("constructor rollover", function () { return new Date(Date.UTC(2024, 12, 32)).toISOString(); });
t("setUTCMonth rollover", function () { var d = new Date(Date.UTC(2024, 0, 31)); d.setUTCMonth(1); return d.toISOString(); });
t("leap day", function () { return new Date(Date.UTC(2024, 1, 29)).toISOString() + "|" + new Date(Date.UTC(2023, 1, 29)).toISOString(); });
t("getUTC* fields", function () {
  var d = new Date(Date.UTC(2024, 0, 13, 10, 20, 30, 400));
  return [d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate(), d.getUTCDay(), d.getUTCHours(), d.getUTCMinutes(), d.getUTCSeconds(), d.getUTCMilliseconds()].join(",");
});
t("valueOf / +d / arithmetic", function () {
  var a = new Date(Date.UTC(2024, 0, 13)), b = new Date(Date.UTC(2024, 0, 14));
  return String(+a) + "|" + String(b - a) + "|" + String(a < b);
});
t("toUTCString", function () { return new Date(0).toUTCString(); });
t("toISOString out of range", function () { return new Date(8.64e15 + 1).toISOString(); });
t("max time value", function () { return String(new Date(8.64e15).getTime()) + "|" + String(new Date(8.64e15 + 1).getTime()); });
t("setTime NaN", function () { var d = new Date(0); d.setTime(NaN); return String(d.getTime()); });
t("Date() as function returns string type", function () { return typeof Date(); });
t("toString shape", function () {
  var s = new Date(0).toString();
  return String(/^[A-Z][a-z]{2} [A-Z][a-z]{2} \d{2} \d{4} \d{2}:\d{2}:\d{2} GMT[+-]\d{4}/.test(s)) + "|hasParen=" + String(s.indexOf("(") >= 0);
});
t("toDateString/toTimeString shape", function () {
  return new Date(0).toDateString() + "|" + String(new Date(0).toTimeString().slice(0, 8));
});
t("toLocaleDateString default", function () { return new Date(Date.UTC(2024, 0, 13)).toLocaleDateString(); });
t("toLocaleTimeString default", function () { return new Date(Date.UTC(2024, 0, 13, 5, 6, 7)).toLocaleTimeString(); });
t("toLocaleString default", function () { return new Date(Date.UTC(2024, 0, 13, 5, 6, 7)).toLocaleString(); });
t("Symbol.toPrimitive on Date", function () {
  var d = new Date(0);
  return String(typeof d[Symbol.toPrimitive]) + "|" + String(typeof (d + 0)) + "|" + String(typeof (d * 1));
});
