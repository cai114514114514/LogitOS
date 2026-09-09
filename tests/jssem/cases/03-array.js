// Array methods. Frameworks render lists with these; a wrong findLast or a
// mutating toSorted is a wrong list on the screen.
function t(tag, f) {
  var r;
  try { r = f(); } catch (e) { r = "THREW " + e.name + ": " + e.message; }
  print(tag + " = " + String(r));
}

t("at", function () {
  var a = [1, 2, 3];
  return [a.at(0), a.at(-1), a.at(3), a.at(-4), a.at(1.7), a.at(NaN)].join(",");
});
t("at on string", function () { return String("abc".at(-1)) + "," + String("abc".at(9)); });
t("flat default", function () { return JSON.stringify([1, [2, [3, [4]]]].flat()); });
t("flat depth", function () { return JSON.stringify([1, [2, [3, [4]]]].flat(2)) + "|" + JSON.stringify([1, [2, [3, [4]]]].flat(Infinity)); });
t("flat holes", function () { var a = [1, , 2, [3, , 4]]; return JSON.stringify(a.flat()) + "|" + a.flat().length; });
t("flat depth 0", function () { return JSON.stringify([1, [2]].flat(0)); });
t("flatMap", function () { return JSON.stringify([1, 2, 3].flatMap(function (x) { return x % 2 ? [x, x] : x; })); });
t("flatMap one level only", function () { return JSON.stringify([1].flatMap(function (x) { return [[x]]; })); });
t("findLast/findLastIndex", function () {
  var a = [1, 2, 3, 4];
  return [a.findLast(function (x) { return x % 2; }), a.findLastIndex(function (x) { return x % 2; }), a.findLast(function () { return false; }), a.findLastIndex(function () { return false; })].join(",");
});
t("findLast visits holes", function () {
  var a = [1, , 3], seen = [];
  a.findLast(function (v, i) { seen.push(i + ":" + String(v)); return false; });
  return seen.join(",");
});
t("copyWithin", function () {
  return JSON.stringify([1, 2, 3, 4, 5].copyWithin(0, 3)) + "|" + JSON.stringify([1, 2, 3, 4, 5].copyWithin(1, -2, -1)) + "|" + JSON.stringify([1, 2, 3, 4, 5].copyWithin(-2, 0));
});
t("toSorted does not mutate", function () {
  if (typeof [].toSorted !== "function") return "absent";
  var a = [3, 1, 2];
  var b = a.toSorted();
  return JSON.stringify(a) + "|" + JSON.stringify(b) + "|" + String(a === b);
});
t("toSorted default is string order", function () {
  if (typeof [].toSorted !== "function") return "absent";
  return JSON.stringify([10, 9, 1, 100].toSorted());
});
t("toSorted comparator", function () {
  if (typeof [].toSorted !== "function") return "absent";
  return JSON.stringify([10, 9, 1].toSorted(function (a, b) { return a - b; }));
});
t("toSorted holes become undefined", function () {
  if (typeof [].toSorted !== "function") return "absent";
  var r = [3, , 1].toSorted();
  return String(r.length) + "|" + String(0 in r) + "|" + String(2 in r) + "|" + JSON.stringify(r);
});
t("toReversed", function () {
  if (typeof [].toReversed !== "function") return "absent";
  var a = [1, 2, 3];
  return JSON.stringify(a.toReversed()) + "|" + JSON.stringify(a);
});
t("toSpliced", function () {
  if (typeof [].toSpliced !== "function") return "absent";
  var a = [1, 2, 3, 4];
  return JSON.stringify(a.toSpliced(1, 2, "x", "y")) + "|" + JSON.stringify(a);
});
t("with", function () {
  if (typeof [].with !== "function") return "absent";
  var a = [1, 2, 3];
  return JSON.stringify(a.with(-1, 9)) + "|" + JSON.stringify(a);
});
t("with out of range", function () {
  if (typeof [].with !== "function") return "absent";
  return [1, 2].with(5, 0);
});
t("sort stability", function () {
  var a = [{ k: 1, i: "a" }, { k: 0, i: "b" }, { k: 1, i: "c" }, { k: 0, i: "d" }, { k: 1, i: "e" }];
  return a.sort(function (x, y) { return x.k - y.k; }).map(function (o) { return o.i; }).join("");
});
t("sort default undefined/holes last", function () {
  var a = [undefined, 3, , 1];
  var s = a.sort();
  return JSON.stringify(s) + "|len=" + s.length + "|2in=" + String(2 in s) + "|3in=" + String(3 in s);
});
t("sort comparator returning non-number", function () {
  return JSON.stringify([3, 1, 2].sort(function () { return "x"; }));
});
t("includes NaN vs indexOf", function () {
  return String([NaN].includes(NaN)) + "," + String([NaN].indexOf(NaN)) + "," + String([-0].includes(0));
});
t("Array.from iterable+mapfn", function () {
  return JSON.stringify(Array.from(new Set([1, 1, 2]), function (x, i) { return x * 10 + i; }));
});
t("Array.from arraylike", function () { return JSON.stringify(Array.from({ length: 3, 0: "a", 2: "c" })); });
t("Array.from string with astral", function () { return JSON.stringify(Array.from("a\u{1F600}b")); });
t("Array.of", function () { return JSON.stringify(Array.of(3)) + "|" + JSON.stringify(Array(3).length); });
t("fill", function () { return JSON.stringify([1, 2, 3].fill(0, 1)) + "|" + JSON.stringify(new Array(3).fill(7)); });
t("reduce no init empty", function () { return [].reduce(function (a, b) { return a + b; }); });
t("reduceRight order", function () { return ["a", "b", "c"].reduceRight(function (a, b) { return a + b; }); });
t("join with null/undefined", function () { return [1, null, undefined, 2].join("-"); });
t("toString on nested", function () { return String([1, [2, [3]]]); });
t("splice returns removed", function () {
  var a = [1, 2, 3, 4];
  return JSON.stringify(a.splice(1, 2, 9)) + "|" + JSON.stringify(a);
});
t("length truncation", function () { var a = [1, 2, 3]; a.length = 1; return JSON.stringify(a); });
t("big index sparse", function () { var a = []; a[5] = 1; return a.length + "|" + JSON.stringify(a) + "|" + a.map(function (x) { return x; }).length; });
t("concat spreadable", function () {
  var o = { length: 2, 0: "a", 1: "b" };
  o[Symbol.isConcatSpreadable] = true;
  return JSON.stringify([].concat(o));
});
t("Array.isArray proxy", function () { return String(Array.isArray(new Proxy([], {}))); });
t("indexOf fromIndex negative", function () { return String([1, 2, 3, 2].indexOf(2, -2)); });
t("lastIndexOf", function () { return String([1, 2, 3, 2].lastIndexOf(2)) + "," + String([1, 2].lastIndexOf(1, -2)); });
