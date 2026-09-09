// RegExp. A SyntaxError here is the worst outcome in this whole file: a regex
// literal is parsed when the SCRIPT is parsed, so one unsupported flag kills
// the entire bundle before a line of it runs.
function t(tag, f) {
  var r;
  try { r = f(); } catch (e) { r = "THREW " + e.name + ": " + e.message; }
  print(tag + " = " + String(r));
}

t("named groups", function () {
  var m = /(?<y>\d{4})-(?<m>\d{2})/.exec("2024-01");
  return m.groups.y + "/" + m.groups.m + "|proto=" + String(Object.getPrototypeOf(m.groups));
});
t("named backreference", function () { return String(/(?<a>x)\k<a>/.test("xx")); });
t("named group in replace", function () { return "2024-01".replace(/(?<y>\d{4})-(?<m>\d{2})/, "$<m>/$<y>"); });
t("named group undefined branch", function () {
  var m = /(?<a>a)|(?<b>b)/.exec("b");
  return String(m.groups.a) + "|" + m.groups.b;
});
t("lookbehind positive", function () { return String(/(?<=\$)\d+/.exec("$42")[0]); });
t("lookbehind negative", function () { return String(/(?<!\$)\d+/.exec("$42")[0]); });
t("variable-length lookbehind", function () { return String(/(?<=ab+)c/.test("abbbc")); });
t("lookahead", function () { return String(/\d+(?=px)/.exec("10px")[0]); });
t("sticky y", function () {
  var re = /a/y;
  re.lastIndex = 1;
  return String(re.test("ba")) + "|" + String(re.lastIndex) + "|" + String(re.test("ba"));
});
t("sticky in split", function () { return JSON.stringify("a1b2c".split(/\d/y)); });
t("global lastIndex", function () {
  var re = /a/g;
  return String(re.exec("aa").index) + "," + String(re.lastIndex) + "," + String(re.exec("aa").index) + "," + String(re.exec("aa"));
});
t("unicode flag u", function () {
  return String(/^.$/u.test("\u{1F600}")) + "|" + String(/^.$/.test("\u{1F600}")) + "|" + String(/\u{1F600}/u.test("\u{1F600}"));
});
t("unicode property escapes", function () { return String(/\p{Letter}/u.test("a")) + "|" + String(/\p{Script=Han}/u.test("中")) + "|" + String(/\p{Emoji_Presentation}/u.test("\u{1F600}")); });
t("v flag", function () { return String(new RegExp("[\\p{ASCII}--[a]]", "v").test("a")); });
t("v flag literal", function () { return String(eval("/a/v.flags")); });
t("d flag indices", function () {
  var m = /(?<w>b)/d.exec("abc");
  return JSON.stringify(m.indices) + "|" + JSON.stringify(m.indices.groups);
});
t("s dotall", function () { return String(/a.b/s.test("a\nb")) + "|" + String(/a.b/.test("a\nb")); });
t("m multiline", function () { return String(/^b/m.test("a\nb")); });
t("i with unicode", function () { return String(/é/i.test("É")) + "|" + String(/k/iu.test("K")); });
t("flags getter order", function () { return new RegExp("a", "yusimgd").flags; });
t("source of empty", function () { return new RegExp("").source + "|" + String(/(?:)/.source); });
t("matchAll", function () {
  var out = [];
  for (var m of "a1b2".matchAll(/([a-z])(\d)/g)) out.push(m[1] + m[2] + "@" + m.index);
  return out.join(",");
});
t("matchAll needs g", function () { return Array.from("a".matchAll(/a/)); });
t("replace with function and groups", function () {
  return "2024-01".replace(/(?<y>\d+)-(?<m>\d+)/, function () {
    var a = Array.prototype.slice.call(arguments);
    return "n=" + a.length + ";last=" + JSON.stringify(a[a.length - 1]);
  });
});
t("replaceAll with regexp g", function () { return "aXbXc".replaceAll(/X/g, "-"); });
t("Symbol.replace custom", function () {
  var o = {}; o[Symbol.replace] = function (s, r) { return "CUSTOM:" + s + ":" + r; };
  return "abc".replace(o, "z");
});
t("split with capture groups", function () { return JSON.stringify("a1b".split(/(\d)/)); });
t("split limit with captures", function () { return JSON.stringify("a1b2c".split(/(\d)/, 3)); });
t("backreference to optional group", function () { return String(/(a)?b\1/.test("b")); });
t("quantifier on lookahead", function () { return String(/(?=a)*b/.test("b")); });
t("catastrophic-ish bounded", function () { return String(/^(a+)+$/.test("aaaaaaaaaaaaaaaaaaaab")); });
t("unicode escape in class", function () { return String(/[\u{61}-\u{63}]/u.test("b")); });
t("case-insensitive class range", function () { return String(/[a-z]/i.test("B")); });
t("RegExp escape static", function () { return typeof RegExp.escape; });
t("legacy RegExp.$1", function () { /(\d+)/.exec("a12"); return String(RegExp.$1); });
t("test coerces non-string", function () { return String(/1/.test(1)) + "|" + String(/null/.test(null)); });
t("exec index/input", function () { var m = /b/.exec("abc"); return m.index + "|" + m.input + "|" + m.length; });
t("hex-literal-then-dot lexer", function () { return String(eval("0x10.toString(16)")); });
t("regex after keyword", function () { return String(typeof (function () { return /a/; })()); });
t("division vs regex disambiguation", function () { var a = 4, b = 2, g = 1; return String(a / b / g); });
