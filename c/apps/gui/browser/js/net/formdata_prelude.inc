/* FormData (XHR Standard, "Interface FormData").
 *
 * A chunk of js_webapi.c's PRELUDE, included right after blob_prelude.inc for
 * the same reason Blob lives there: fetch() and XMLHttpRequest.send() have to
 * ENCODE one (multipart/form-data, see extractBody in fetch_body_prelude.inc)
 * and Body.formData() has to BUILD one, so the class belongs beside the code
 * that consumes it -- and in the worker fetch prelude too, where FormData is
 * exposed and js_platform.c never runs. js_platform.c's `if (!G.FormData)`
 * fallback is installed later and therefore becomes dead code by
 * construction, exactly like its Blob.
 *
 * WHAT CHANGED FROM THAT FALLBACK, and it is not cosmetic: the spec's "create
 * an entry" turns a Blob value into a File -- named "blob", or the filename
 * argument -- AT append()/set() TIME. The fallback kept the Blob and stored
 * the filename beside it, where the multipart encoder never looked: a page's
 * `fd.append('upload', blob, 'photo.png')` went out as filename="blob".
 *
 * NOT HERE: the `form` constructor argument. Building an entry list from a
 * <form> needs a walk over its submittable elements that this engine does not
 * have (see ElementInternals' note in js_platform.c); the argument is ignored,
 * as it always was. */
"function fdEntry(name, value, filename) {\n"
"  name = String(name);\n"
"  if (value instanceof G.Blob) {\n"
"    if (!(value instanceof G.File)) value = new G.File([value], 'blob', { type: value.type });\n"
"    if (filename !== undefined)\n"
"      value = new G.File([value], String(filename), { type: value.type, lastModified: value.lastModified });\n"
"  } else value = String(value);\n"
"  return [name, value];\n"
"}\n"
/* `_e` is non-enumerable bookkeeping, like Blob's `_b`: the entry list, in
 * insertion order INCLUDING duplicates (three checkboxes of one name are
 * three entries). */
"G.FormData = function FormData() {\n"
"  Object.defineProperty(this, '_e', { value: [], enumerable: false, configurable: true, writable: true });\n"
"};\n"
"G.FormData.prototype = {\n"
"  constructor: G.FormData,\n"
"  append: function (n, v, fn) { this._e.push(fdEntry(n, v, fn)); },\n"
   /* set(): replace the FIRST entry of that name in place, drop the rest. */
"  set: function (n, v, fn) {\n"
"    var e = fdEntry(n, v, fn), done = false;\n"
"    this._e = this._e.filter(function (p) {\n"
"      if (p[0] !== e[0]) return true;\n"
"      if (done) return false;\n"
"      done = true; p[1] = e[1]; return true;\n"
"    });\n"
"    if (!done) this._e.push(e);\n"
"  },\n"
"  get: function (n) { var k = String(n);\n"
"    for (var i = 0; i < this._e.length; i++) if (this._e[i][0] === k) return this._e[i][1];\n"
"    return null; },\n"
"  getAll: function (n) { var k = String(n);\n"
"    return this._e.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; }); },\n"
"  has: function (n) { var k = String(n);\n"
"    return this._e.some(function (p) { return p[0] === k; }); },\n"
"  delete: function (n) { var k = String(n);\n"
"    this._e = this._e.filter(function (p) { return p[0] !== k; }); },\n"
"  forEach: function (fn, t) { var s = this; this._e.slice().forEach(function (p) { fn.call(t, p[1], p[0], s); }); },\n"
"  keys: function () { return this._e.map(function (p) { return p[0]; })[Symbol.iterator](); },\n"
"  values: function () { return this._e.map(function (p) { return p[1]; })[Symbol.iterator](); },\n"
"  entries: function () { return this._e.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); }\n"
"};\n"
"G.FormData.prototype[Symbol.iterator] = G.FormData.prototype.entries;\n"
"Object.defineProperty(G.FormData.prototype, Symbol.toStringTag, { value: 'FormData', configurable: true });\n"
