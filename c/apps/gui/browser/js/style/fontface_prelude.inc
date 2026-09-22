/* document.fonts -- CSS Font Loading's FontFaceSet, and FontFace.
 *
 * WHY THIS IS HERE AND WHY IT IS NOT A LIE.
 *
 * Ranked from the corpus rather than guessed: `cannot read property 'ready' of
 * undefined` kills 182 test FILES, second only to `CSS` (533), and every one
 * of the 182 dies before its first assertion. They all open the same way --
 *
 *     document.fonts.ready.then(() => { ... the actual test ... })
 *
 * -- which is the standard way a layout test waits for web fonts before
 * measuring. With `document.fonts` undefined that is a TypeError at the top
 * level of an inline script: the test body is never even reached, the harness
 * never completes, and none of the file's subtests appear in the denominator.
 * So the cost is not "182 failures", it is 182 files whose contents were never
 * measured at all.
 *
 * NOW THE HONEST PART, because a promise that resolves is exactly the shape a
 * useless stub has. In this browser, fonts are NOT loaded from the network:
 * text_init() reads /fonts/{ui,mono}.ttf off the LogitFS disk at boot, before
 * any page exists, and there is no @font-face fetch anywhere in the tree. The
 * font set is therefore complete and unchanging by the time a script runs.
 * "Everything that is ever going to load has loaded" is the TRUE state of this
 * engine, and `ready` resolving immediately reports it correctly -- it is not
 * a stub standing in for work that should happen later, it is the answer.
 *
 * WHAT IS NOT TRUE, stated so nobody reads more into this than it says:
 *   - add()ing a FontFace does not make that font available to layout. The set
 *     is a real collection and remembers what you put in it; the RASTERISER
 *     does not consult it. A page that installs a web font still renders in
 *     the built-in face.
 *   - check() answers true. It cannot answer honestly without the font
 *     matching that layout does, which lives in c/lib/text and is not reachable
 *     from here. True is the safer wrong answer: false makes a page conclude
 *     its font failed and take a fallback path, which is a second wrong
 *     rendering on top of the first.
 *   - loading/loadingdone/loadingerror never fire, because nothing ever starts
 *     loading. The handler properties and listener methods exist so that
 *     registering one is not a TypeError.
 * When real @font-face loading lands, `ready` becomes a promise with something
 * to wait for and check() becomes answerable; nothing else here changes.
 */

"G.FontFace = function FontFace(family, source, descriptors) {\n"
"  if (!(this instanceof FontFace)) throw new TypeError(\"Constructor FontFace requires 'new'\");\n"
"  var d = descriptors || {};\n"
"  this.family = String(family === undefined ? '' : family);\n"
"  this.style = d.style || 'normal';\n"
"  this.weight = d.weight || 'normal';\n"
"  this.stretch = d.stretch || 'normal';\n"
"  this.unicodeRange = d.unicodeRange || 'U+0-10FFFF';\n"
"  this.variant = d.variant || 'normal';\n"
"  this.featureSettings = d.featureSettings || 'normal';\n"
"  this.variationSettings = d.variationSettings || 'normal';\n"
"  this.display = d.display || 'auto';\n"
"  this.ascentOverride = d.ascentOverride || 'normal';\n"
"  this.descentOverride = d.descentOverride || 'normal';\n"
"  this.lineGapOverride = d.lineGapOverride || 'normal';\n"
"  this._source = source;\n"
/* 'loaded', not 'unloaded': see the header. A face constructed from an
 * ArrayBuffer is as loaded as anything gets here, and one constructed from a
 * url() will never load, so reporting 'loading' would be a promise the engine
 * cannot keep. */
"  this.status = 'loaded';\n"
"  var self = this;\n"
"  this.loaded = Promise.resolve(self);\n"
"};\n"
"G.FontFace.prototype.load = function () { this.status = 'loaded'; return Promise.resolve(this); };\n"
"Object.defineProperty(G.FontFace.prototype, Symbol.toStringTag,\n"
"  { value: 'FontFace', configurable: true });\n"

/* The set. Set-like per the IDL: iterable, with size/has/add/delete/clear and
 * the three iterator methods. Backed by a plain array because insertion order
 * is observable and the sets involved have single-digit membership. */
"G.FontFaceSet = function FontFaceSet(initial) {\n"
"  this._f = [];\n"
"  if (initial && initial.length) for (var i = 0; i < initial.length; i++) this._f.push(initial[i]);\n"
"  this._l = {};\n"
"  this.onloading = null; this.onloadingdone = null; this.onloadingerror = null;\n"
"  this.status = 'loaded';\n"
"  this.ready = Promise.resolve(this);\n"
"};\n"
"G.FontFaceSet.prototype = {\n"
"  constructor: G.FontFaceSet,\n"
"  add: function (f) { if (this._f.indexOf(f) < 0) this._f.push(f); return this; },\n"
"  delete: function (f) { var i = this._f.indexOf(f); if (i < 0) return false; this._f.splice(i, 1); return true; },\n"
"  clear: function () { this._f.length = 0; },\n"
"  has: function (f) { return this._f.indexOf(f) >= 0; },\n"
"  forEach: function (cb, thisArg) {\n"
"    var self = this;\n"
"    this._f.slice().forEach(function (f) { cb.call(thisArg, f, f, self); });\n"
"  },\n"
"  values: function () { return this._f.slice()[Symbol.iterator](); },\n"
"  keys: function () { return this._f.slice()[Symbol.iterator](); },\n"
"  entries: function () { return this._f.map(function (f) { return [f, f]; })[Symbol.iterator](); },\n"
/* check(): see the header -- true, deliberately, and documented as such. */
"  check: function (font, text) { return true; },\n"
/* load(): resolves with the faces already in the set whose family the request
 * names, or an empty list. Never rejects: nothing can fail to load. */
"  load: function (font, text) {\n"
"    var fam = String(font === undefined ? '' : font);\n"
"    var out = this._f.filter(function (f) { return fam.indexOf(f.family) >= 0; });\n"
"    return Promise.resolve(out);\n"
"  },\n"
"  addEventListener: function (t, fn) {\n"
"    if (typeof fn !== 'function') return;\n"
"    (this._l[t] || (this._l[t] = [])).push(fn);\n"
"  },\n"
"  removeEventListener: function (t, fn) {\n"
"    var a = this._l[t]; if (!a) return;\n"
"    var i = a.indexOf(fn); if (i >= 0) a.splice(i, 1);\n"
"  },\n"
"  dispatchEvent: function () { return true; }\n"
"};\n"
"Object.defineProperty(G.FontFaceSet.prototype, 'size',\n"
"  { get: function () { return this._f.length; }, configurable: true });\n"
"G.FontFaceSet.prototype[Symbol.iterator] = G.FontFaceSet.prototype.values;\n"
"Object.defineProperty(G.FontFaceSet.prototype, Symbol.toStringTag,\n"
"  { value: 'FontFaceSet', configurable: true });\n"

/* Attach to the document, without disturbing one that already exists: if the
 * CSSOM line lands a real FontFaceSet later, theirs wins and this becomes
 * dead code rather than a conflict. */
"if (G.document && !G.document.fonts) {\n"
"  try {\n"
"    Object.defineProperty(G.document, 'fonts',\n"
"      { value: new G.FontFaceSet(), enumerable: true, configurable: true });\n"
"  } catch (e) { try { G.document.fonts = new G.FontFaceSet(); } catch (e2) {} }\n"
"}\n"
