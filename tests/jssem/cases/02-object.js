// Object statics a framework uses on every render pass.
function t(tag, f) {
  var r;
  try { r = f(); } catch (e) { r = "THREW " + e.name + ": " + e.message; }
  print(tag + " = " + String(r));
}

t("keys order int-then-string", function () {
  var o = { b: 1, 2: 1, a: 1, 1: 1, "-1": 1, "01": 1 };
  return Object.keys(o).join(",");
});
t("entries", function () { return JSON.stringify(Object.entries({ a: 1, b: 2 })); });
t("entries on string", function () { return JSON.stringify(Object.entries("ab")); });
t("entries skips non-enumerable", function () {
  var o = {};
  Object.defineProperty(o, "h", { value: 1, enumerable: false });
  o.v = 2;
  return JSON.stringify(Object.entries(o));
});
t("values", function () { return JSON.stringify(Object.values({ a: 1, b: [2] })); });
t("fromEntries array", function () { return JSON.stringify(Object.fromEntries([["a", 1], ["b", 2]])); });
t("fromEntries map", function () { return JSON.stringify(Object.fromEntries(new Map([["a", 1]]))); });
t("fromEntries dup key", function () { return JSON.stringify(Object.fromEntries([["a", 1], ["a", 2]])); });
t("fromEntries non-iterable", function () { return Object.fromEntries(5); });
t("fromEntries bad pair", function () { return JSON.stringify(Object.fromEntries([1])); });

t("hasOwn", function () {
  var o = Object.create({ inh: 1 }); o.own = 2;
  return String(Object.hasOwn(o, "own")) + "," + String(Object.hasOwn(o, "inh")) + "," + String(Object.hasOwn([1], 0)) + "," + String(Object.hasOwn([1], "length"));
});
t("hasOwn on primitive", function () { return String(Object.hasOwn("ab", 1)); });

t("groupBy", function () {
  if (typeof Object.groupBy !== "function") return "absent";
  var g = Object.groupBy([1, 2, 3, 4, 5], function (n) { return n % 2 ? "odd" : "even"; });
  return JSON.stringify(g) + "|proto=" + String(Object.getPrototypeOf(g));
});
t("groupBy key coercion", function () {
  if (typeof Object.groupBy !== "function") return "absent";
  var g = Object.groupBy([1, 2], function (n) { return n; });
  return JSON.stringify(g) + "|" + Object.keys(g).join(",");
});
t("groupBy index arg", function () {
  if (typeof Object.groupBy !== "function") return "absent";
  return JSON.stringify(Object.groupBy(["a", "b", "c"], function (v, i) { return i < 2 ? "lo" : "hi"; }));
});

t("assign getters invoked", function () {
  var src = { get g() { return 5; } };
  var out = Object.assign({}, src);
  return JSON.stringify(Object.getOwnPropertyDescriptor(out, "g"));
});
t("spread vs assign setter", function () {
  var log = [];
  var target = {};
  Object.defineProperty(target, "p", { set: function (v) { log.push("set" + v); }, enumerable: true, configurable: true });
  Object.assign(target, { p: 1 });
  var spread = Object.assign({}, { p: 2 });
  return log.join(",") + "|" + JSON.stringify(spread);
});
t("getOwnPropertyDescriptors", function () {
  var o = { a: 1 };
  return JSON.stringify(Object.getOwnPropertyDescriptors(o));
});
t("getOwnPropertyNames array", function () { return Object.getOwnPropertyNames([1, 2]).join(","); });
t("defineProperty defaults", function () {
  var o = {};
  Object.defineProperty(o, "x", { value: 1 });
  return JSON.stringify(Object.getOwnPropertyDescriptor(o, "x"));
});
t("accessor on prototype seen by instance", function () {
  function C() {}
  var got = [];
  Object.defineProperty(C.prototype, "v", {
    get: function () { got.push("g"); return 1; },
    set: function (x) { got.push("s" + x); },
    configurable: true
  });
  var c = new C();
  c.v = 9;
  var r = c.v;
  return got.join(",") + "|" + r + "|own=" + String(Object.hasOwn(c, "v"));
});
t("freeze / isFrozen", function () {
  var o = Object.freeze({ a: 1 });
  try { o.a = 2; } catch (e) {}
  return String(Object.isFrozen(o)) + "," + o.a;
});
t("seal then delete", function () {
  var o = Object.seal({ a: 1 });
  return String(delete o.a) + "," + o.a;
});
t("preventExtensions", function () {
  var o = Object.preventExtensions({});
  o.n = 1;
  return String(Object.isExtensible(o)) + "," + String(o.n);
});
t("Object.is", function () {
  return [Object.is(NaN, NaN), Object.is(0, -0), Object.is(-0, -0), Object.is("a", "a")].join(",");
});
t("__proto__ in literal", function () {
  var p = { m: 1 };
  var o = { __proto__: p };
  return String(Object.getPrototypeOf(o) === p) + "," + String(Object.keys(o).length);
});
t("null-proto object stringify", function () {
  var o = Object.create(null); o.a = 1;
  return JSON.stringify(o);
});
t("null-proto String()", function () { return String(Object.create(null)); });
t("structured key enumeration with symbol", function () {
  var s = Symbol("s");
  var o = { a: 1 }; o[s] = 2;
  return Object.keys(o).join(",") + "|" + String(Object.getOwnPropertySymbols(o).length) + "|" + JSON.stringify(o);
});
