/* The platform features the framework corpus actually dies on, asked for one
 * at a time, from a plain deferred classic script -- the same context a
 * bundler runtime bootstraps in.
 *
 * WHY THIS IS A FIXTURE AND NOT A UNIT TEST IN C. Each line here was derived
 * from a THROW SITE in a real built bundle (see CAUSES in
 * tests/unit/framework_rank.py), so this file is the reduction of those seven
 * bundles to the smallest program that reaches the same properties. A C test
 * asserting "js_platform publishes baseURI" would pass the day someone adds
 * the name and say nothing about whether Angular's router can resolve a route.
 *
 * Every line is `#API<tab>name<tab>value` so tests/unit/framework_rank.py can
 * read it back. Values are stringified deliberately loosely: what matters is
 * present/absent/null, not the exact text. */
function api(name, v) {
  var s;
  try { s = (v === null) ? "null" : (v === undefined) ? "undefined" : String(v); }
  catch (e) { s = "threw:" + e; }
  console.log("#API\t" + name + "\t" + s);
}

var d = document;

/* -- the webpack / Turbopack chunk base, cause #1 in the ranked table ------
 * webpack's runtime: `"SCRIPT" === document.currentScript?.tagName.toUpperCase()
 * && (e = document.currentScript.src)`, then a getElementsByTagName("script")
 * fallback that only accepts a src matching /^http(s?):/. BOTH are asked here
 * because either one alone is enough to make webpack work, and neither being
 * enough on its own is the finding. */
api("document.currentScript", d.currentScript ? "element" : d.currentScript);
api("currentScript.tagName", d.currentScript && d.currentScript.tagName);
api("currentScript.src", d.currentScript && d.currentScript.src);
var els = d.getElementsByTagName("script");
api("getElementsByTagName(script).length", els && els.length);
api("script[0].src", els && els.length ? els[0].src : "no-script");
api("script[0].getAttribute(src)", els && els.length ? els[0].getAttribute("src") : "no-script");

/* -- angular's router: new URL(path, document.baseURI) -------------------- */
api("document.baseURI", d.baseURI);

/* -- vue's runtime-dom: `el instanceof SVGElement` picks the patch path ---- */
api("SVGElement", typeof SVGElement !== "undefined" ? "ctor" : undefined);

/* -- svelte's template(): sets template.innerHTML, returns .content -------- */
var t = d.createElement("template");
t.innerHTML = "<p>x</p>";
api("template.content", t.content ? "fragment" : t.content);

/* -- reached for but never fatal in this corpus; here so a change shows ---- */
api("document.contentType", d.contentType);
api("window.trustedTypes", typeof window.trustedTypes !== "undefined" ? "object" : undefined);
api("document.documentMode", d.documentMode);
api("window.event", typeof window.event !== "undefined" ? "object" : undefined);
