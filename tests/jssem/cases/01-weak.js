// WeakMap / WeakSet / WeakRef / FinalizationRegistry, and object identity.
// Frameworks key component state on object identity; a WeakMap that compares
// by anything other than identity, or an absent WeakRef, breaks that whole
// class of bookkeeping.
function t(tag, f) {
  var r;
  try { r = f(); } catch (e) { r = "THREW " + e.name + ": " + e.message; }
  print(tag + " = " + String(r));
}

t("typeof WeakMap", function () { return typeof WeakMap; });
t("typeof WeakSet", function () { return typeof WeakSet; });
t("typeof WeakRef", function () { return typeof WeakRef; });
t("typeof FinalizationRegistry", function () { return typeof FinalizationRegistry; });

t("WeakMap.name", function () { return WeakMap.name; });
t("WeakMap.length", function () { return WeakMap.length; });
t("WeakMap proto tag", function () { return Object.prototype.toString.call(new WeakMap()); });
t("WeakSet proto tag", function () { return Object.prototype.toString.call(new WeakSet()); });
t("WeakMap Symbol.toStringTag", function () { return WeakMap.prototype[Symbol.toStringTag]; });

// identity, not structural equality
t("wm identity distinct", function () {
  var wm = new WeakMap(), a = {}, b = {};
  wm.set(a, "A"); wm.set(b, "B");
  return wm.get(a) + "/" + wm.get(b) + "/" + wm.has({});
});
t("wm same-shape keys", function () {
  var wm = new WeakMap(), a = { x: 1 }, b = { x: 1 };
  wm.set(a, 1);
  return String(wm.has(b)) + "," + String(wm.has(a));
});
t("wm delete", function () {
  var wm = new WeakMap(), a = {};
  wm.set(a, 1);
  return String(wm.delete(a)) + "," + String(wm.delete(a)) + "," + String(wm.get(a));
});
t("wm set returns this", function () {
  var wm = new WeakMap(), a = {};
  return String(wm.set(a, 1) === wm);
});
t("wm primitive key", function () { return new WeakMap().set(1, 2); });
t("wm string key", function () { return new WeakMap().set("s", 2); });
t("wm null key", function () { return new WeakMap().set(null, 2); });
t("wm symbol key", function () { return String(new WeakMap().set(Symbol("k"), 2) instanceof WeakMap); });
t("wm registered symbol key", function () { return new WeakMap().set(Symbol.for("k"), 2); });
t("wm get missing", function () { return String(new WeakMap().get({})); });
t("wm has primitive", function () { return String(new WeakMap().has(1)); });
t("wm delete primitive", function () { return String(new WeakMap().delete(1)); });
t("wm from iterable", function () {
  var a = {}, b = {};
  var wm = new WeakMap([[a, 1], [b, 2]]);
  return wm.get(a) + "," + wm.get(b);
});
t("wm without new", function () { return WeakMap(); });
t("wm forEach absent", function () { return typeof WeakMap.prototype.forEach; });
t("wm size absent", function () { return String(new WeakMap().size); });

t("ws basics", function () {
  var ws = new WeakSet(), a = {};
  ws.add(a);
  return String(ws.has(a)) + "," + String(ws.has({})) + "," + String(ws.delete(a)) + "," + String(ws.has(a));
});
t("ws add returns this", function () { var ws = new WeakSet(); return String(ws.add({}) === ws); });
t("ws primitive", function () { return new WeakSet().add(1); });

// Map/Set identity for contrast -- SameValueZero, so NaN is one key
t("map NaN key", function () {
  var m = new Map(); m.set(NaN, 1);
  return String(m.get(NaN)) + "," + String(m.has(NaN));
});
t("map -0/+0", function () {
  var m = new Map(); m.set(-0, "z");
  var k = []; m.forEach(function (v, kk) { k.push(Object.is(kk, 0) ? "+0" : String(kk)); });
  return String(m.get(0)) + "," + k.join("");
});
t("set dedupe by identity", function () {
  var a = { x: 1 };
  return String(new Set([a, a, { x: 1 }]).size);
});
t("map insertion order", function () {
  var m = new Map([["b", 1], ["a", 2]]);
  m.set("c", 3); m.delete("b"); m.set("b", 4);
  return Array.from(m.keys()).join("");
});
t("map chain/size", function () {
  var m = new Map();
  return String(m.set(1, 1).set(2, 2).size);
});
t("Map groupBy", function () {
  if (typeof Map.groupBy !== "function") return "absent";
  var g = Map.groupBy([1, 2, 3, 4], function (n) { return n % 2 ? "odd" : "even"; });
  return Array.from(g.keys()).join(",") + "|" + JSON.stringify(g.get("odd"));
});

// WeakRef / FinalizationRegistry surface, without depending on GC timing
t("WeakRef deref same tick", function () {
  var o = { v: 7 };
  var r = new WeakRef(o);
  return String(r.deref() === o);
});
t("WeakRef primitive", function () { return new WeakRef(1); });
t("WeakRef without new", function () { return WeakRef({}); });
t("WeakRef tag", function () { return Object.prototype.toString.call(new WeakRef({})); });
t("FinalizationRegistry register", function () {
  var fr = new FinalizationRegistry(function () {});
  var o = {};
  fr.register(o, "held", o);
  return String(fr.unregister(o));
});
