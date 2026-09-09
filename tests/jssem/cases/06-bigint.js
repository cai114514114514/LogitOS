// BigInt arithmetic and, just as important, the MIXING ERRORS -- bundles
// feature-detect BigInt and then rely on the TypeError to route.
function t(tag, f) {
  var r;
  try { r = f(); } catch (e) { r = "THREW " + e.name + ": " + e.message; }
  print(tag + " = " + String(r));
}

t("typeof", function () { return typeof 1n; });
t("literal forms", function () { return String(0n) + "|" + String(0x10n) + "|" + String(0b101n) + "|" + String(0o17n) + "|" + String(1_000n); });
t("arithmetic", function () { return [1n + 2n, 7n / 2n, -7n / 2n, 7n % 3n, -7n % 3n, 2n ** 64n, 2n * 3n, 5n - 9n].map(String).join("|"); });
t("division by zero", function () { return 1n / 0n; });
t("unary minus and plus", function () { return String(-5n) + "|" + (function () { try { return +1n; } catch (e) { return e.name; } })(); });
t("bitwise", function () { return [1n << 100n, -1n >> 1n, 0xffn & 0x0fn, 1n | 2n, 5n ^ 3n, ~5n].map(String).join("|"); });
t("unsigned shift refused", function () { return 1n >>> 1n; });
t("mix + number", function () { return 1n + 1; });
t("mix - number", function () { return 1n - 1; });
t("mix * number", function () { return 2n * 2; });
t("mix ** number", function () { return 2n ** 2; });
t("mix bitand number", function () { return 1n & 1; });
t("string concat allowed", function () { return "x" + 1n; });
t("comparison loose", function () { return [1n == 1, 1n == "1", 1n === 1, 1n < 2, 2n > 1.5, 1n == 1.0, 1n == 1.5].map(String).join("|"); });
t("sort mixed", function () { return [3n, 1, 2n].sort(function (a, b) { return a < b ? -1 : a > b ? 1 : 0; }).join(","); });
t("Number(bigint) lossy", function () { return String(Number(2n ** 60n)) + "|" + String(Number(9007199254740993n)); });
t("BigInt(number) non-integer", function () { return BigInt(1.5); });
t("BigInt(string)", function () { return String(BigInt("0x10")) + "|" + String(BigInt(" 12 ")) + "|" + (function () { try { return BigInt("1.0"); } catch (e) { return e.name; } })(); });
t("BigInt(true/null)", function () { return String(BigInt(true)) + "|" + (function () { try { return BigInt(null); } catch (e) { return e.name; } })(); });
t("toString radix", function () { return (255n).toString(16) + "|" + (255n).toString(2) + "|" + (-255n).toString(36); });
t("asIntN/asUintN", function () { return [BigInt.asIntN(8, 255n), BigInt.asUintN(8, -1n), BigInt.asIntN(64, 2n ** 63n)].map(String).join("|"); });
t("truthiness", function () { return String(!!0n) + "|" + String(!!1n) + "|" + String(Boolean(0n)); });
t("Math.max on bigint", function () { return Math.max(1n, 2n); });
t("JSON.stringify bigint", function () { return JSON.stringify(1n); });
t("bigint toJSON shim", function () {
  var proto = BigInt.prototype;
  var had = "toJSON" in proto;
  return String(had);
});
t("big multiplication exactness", function () {
  var a = 123456789012345678901234567890n;
  return String(a * a);
});
t("big power and mod", function () {
  var m = 1000000007n, b = 2n, e = 1000n, r = 1n;
  while (e > 0n) { if (e & 1n) r = r * b % m; b = b * b % m; e >>= 1n; }
  return String(r);
});
t("factorial 30", function () { var r = 1n; for (var i = 1n; i <= 30n; i++) r *= i; return String(r); });
t("negative shift", function () { return String(1n << -1n); });
t("Object(bigint)", function () { return typeof Object(1n) + "|" + String(Object(1n) == 1n) + "|" + Object.prototype.toString.call(Object(1n)); });
t("bigint as object key", function () { var o = {}; o[1n] = "v"; return Object.keys(o).join(",") + "|" + o[1]; });
t("BigInt64Array", function () {
  if (typeof BigInt64Array !== "function") return "absent";
  var a = new BigInt64Array(2); a[0] = -1n;
  return String(a[0]) + "|" + String(new BigUint64Array([-1n])[0]);
});
