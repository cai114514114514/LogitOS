/* Shared production prelude: each evaluation owns its closure and JSValues.
 * WEBAPI_FETCH_WORKER_PRELUDE excludes document-only additions. */
/* ---- URL.createObjectURL / revokeObjectURL, backed by a real table ----
 * Until now this fell through to js_platform.c's own fallback (guarded on
 * `!G.URL.createObjectURL`): a Blob became a `data:` URL, which fetch() can
 * dereference but which is neither revocable nor bounded -- an 8 MB Blob
 * became an 11+ MB base64 string sitting in a URL forever. This is the real
 * thing instead: a bounded, revocable table keyed by a `blob:<origin>/<serial>`
 * id that only this table (and fetch(), just below) can resolve.
 *
 * Layering, load-bearing: js_webapi_install runs before js_media_install and
 * js_platform_install (see js_page.c), and both of those guard their own
 * installs on `if (!G.URL.createObjectURL) ...` -- so this, running first,
 * wins and theirs quietly never fire. That is also why a MediaSource still
 * works: this only claims Blob/File, and falls through to
 * `G.__createObjectURL` (js_media.c's primitive, installed onto the global
 * object -- not onto G.URL -- by the C side of that file) for anything else.
 * It is looked up BY NAME at call time rather than captured now, because
 * js_media_install has not run yet at the point this line executes; by the
 * time a page actually calls URL.createObjectURL(mediaSource), every install
 * is long done. Same shape for revokeObjectURL/G.__revokeObjectURL. */
"var __objURLs = Object.create(null), __objURLCount = 0, __objURLBytes = 0, __objURLSerial = 0;\n"
/* Configured privately by C before page code runs. A realm generation prevents
 * page A's retired /1 URL from resolving to page B's freshly allocated /1. */
"var __objURLOrigin = 'null', __objURLGeneration = '0';\n"
"var OBJURL_MAX = 64, OBJURL_MAX_BYTES = 8 * 1024 * 1024;\n"
"G.URL.createObjectURL = function (obj) {\n"
"  if (obj instanceof G.Blob) {\n"
"    if (__objURLCount >= OBJURL_MAX)\n"
"      throw new (G.DOMException || TypeError)(\n"
"        'createObjectURL: too many live object URLs (max ' + OBJURL_MAX + ')', 'QuotaExceededError');\n"
"    if (__objURLBytes + obj.size > OBJURL_MAX_BYTES)\n"
"      throw new (G.DOMException || TypeError)(\n"
"        'createObjectURL: object URL table is full (max ' + OBJURL_MAX_BYTES + ' bytes total)', 'QuotaExceededError');\n"
"    var id = 'blob:' + __objURLOrigin + '/' + __objURLGeneration + '-' + (++__objURLSerial);\n"
"    __objURLs[id] = obj; __objURLCount++; __objURLBytes += obj.size;\n"
"    return id;\n"
"  }\n"
"  if (typeof G.__createObjectURL === 'function') return G.__createObjectURL(obj);\n"
"  throw new TypeError('createObjectURL: needs a Blob (or a File, or a MediaSource if media is linked)');\n"
"};\n"
"G.URL.revokeObjectURL = function (url) {\n"
"  url = String(url);\n"
"  if (Object.prototype.hasOwnProperty.call(__objURLs, url)) {\n"
"    __objURLBytes -= __objURLs[url].size; __objURLCount--;\n"
"    delete __objURLs[url];\n"
"    return;\n"
"  }\n"
"  if (typeof G.__revokeObjectURL === 'function') G.__revokeObjectURL(url);\n"
"};\n"

