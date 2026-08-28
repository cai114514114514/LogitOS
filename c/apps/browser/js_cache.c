/* c/apps/browser/js_cache.c -- CacheStorage (`caches` / `Cache`), part A of
 * the ServiceWorker workflow item.
 *
 * WHY THIS IS THE PART THAT SHIPS. Measured before writing a line of this
 * file (see the workflow's own scope note, kept in full there): a promise
 * reaction queued inside a Worker's own JSRuntime never ran, and `fetch` /
 * `Request` / `Response` / `Headers` are singleton-bound to the PAGE realm
 * (js_webapi.c's g_mk_response / g_fetch[] / g_stores[] are file statics,
 * installed once via js_webapi_install(ctx, url)). Together those two
 * findings are why parts B and C of the larger feature (the
 * navigator.serviceWorker CLIENT surface, and EXECUTING a service worker
 * script with fetch interception) are scoped down to "register() always
 * rejects" and "not built" respectively -- see js_swreg.c.
 *
 * CacheStorage needs NEITHER of those two blockers. service-workers/
 * cache-storage/'s own `.any.js` files are `// META: global=window,worker`
 * and use `self.caches` + `new Request` + `new Response` + `fetch` -- all of
 * which the WINDOW realm already has, complete, including a real
 * ReadableStream body (js_webapi.c). So this file installs `caches` on the
 * window only, over the EXISTING Request/Response/Headers/fetch -- one
 * implementation of each, not a second -- and is deliberately silent
 * (`typeof caches === 'undefined'`) inside a Worker/ServiceWorker realm,
 * which is the CORRECT feature-detect answer for a per-realm store this
 * build does not share across realms (the same argument js_idb.c already
 * made for `indexedDB` in a worker, restated here rather than re-derived).
 *
 * THE TERMINATION BAR, EVERY PUBLIC METHOD: every CacheStorage/Cache method
 * below is a WebIDL promise-returning operation, and the WebIDL convention
 * this file holds itself to is "synchronous argument-processing exceptions
 * become a REJECTED promise, never a promise that never settles and never a
 * bare synchronous throw out of a `Promise<T>`-typed method". Concretely:
 * every method's body is either wrapped in `new Promise(function(resolve,
 * reject){ try {...} catch(e){reject(e);} ... })` or built by chaining real
 * Promises (`.then`) that themselves only ever resolve or reject. `put()` is
 * the one path that touches a body -- it rejects synchronously for a
 * non-GET request, a non-http(s) scheme, a 206 response, a `Vary: *`
 * response, and a used/locked body (all spec requirements), and otherwise
 * reads the body through the SAME arrayBuffer() path js_webapi.c already
 * exposes, which is terminal because a stalled body is a stalled `fetch`
 * and js_webapi_pump already drives every wfetch to BF_DONE or BF_FAILED.
 * `add`/`addAll` reject TypeError on any non-2xx fetch or a duplicate
 * request in the list (spec), and are terminal because `fetch` and `put`
 * both are.
 *
 * A HARD QUOTA, NAMED, NOT AN AFTERTHOUGHT: this store holds attacker-
 * controlled response bodies inside the one ring-3 process that holds every
 * capability, with no ASLR and no stack canaries, and this machine was
 * raised from 512 MiB to 1 GiB the day this file was written because the
 * browser peaked near 600 MB. CACHE_QUOTA_BYTES below is a hard cap on total
 * cached bytes across every named cache; exceeding it REJECTS put() with a
 * real QuotaExceededError DOMException -- a settled promise, never a pending
 * one, and never a silent truncation that would leave a shorter body cached
 * under the right key.
 *
 * DELIBERATELY NOT HERE, refused rather than half-built:
 *   - Durability across a page load / reboot. Session-scoped, in-memory only
 *     -- the SAME durability localStorage and js_idb.c already have on this
 *     machine, and for the same reason: there is no VFS positional write to
 *     build real durability on (CLAUDE.md structural gap #3).
 *   - `caches` inside a Worker/ServiceWorker realm. `typeof caches ===
 *     'undefined'` there; see the header paragraph above.
 *   - Storage buckets (`cache-storage-buckets.https.any.js` stays failing,
 *     correctly).
 *   - A `CacheQueryOptions` field this file does not implement silently
 *     doing nothing: `ignoreSearch`, `ignoreMethod` and `ignoreVary` are all
 *     honoured for real (see matchOne/entryMatchesRequest below); there is
 *     no fourth option to half-implement.
 *
 * Install AFTER js_webapi_install and js_platform_install -- see js_cache.h.
 */

#include "js_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The hard cap (see this file's header). Overridable at compile time ONLY so
 * tests/unit/cache_test.c can watch the QuotaExceededError path actually
 * fire without allocating 64 MiB in a host test -- rule 2 of this workflow
 * ("build the control and WATCH it go red"). The production default is the
 * real number; nothing about the STORE's behaviour changes, only how much
 * it takes to trip it. */
#ifndef JS_CACHE_QUOTA_BYTES
#define JS_CACHE_QUOTA_BYTES (64 * 1024 * 1024)
#endif

static const char *CACHE_JS =
"(function (G) {\n"
"if (G.caches) return;\n"
"if (typeof G.Promise !== 'function') return;\n"
"if (typeof G.fetch !== 'function') return;\n"
"if (typeof G.Request !== 'function') return;\n"
"if (typeof G.Response !== 'function') return;\n"
"if (typeof G.Headers !== 'function') return;\n"
"if (typeof G.URL !== 'function') return;\n"
"if (typeof G.DOMException !== 'function') return;\n"
"if (typeof G.Map !== 'function') return;\n"
"\n"
"function named(f, n, len) {\n"
"  try { Object.defineProperty(f, 'name', { value: n, configurable: true }); } catch (e) {}\n"
"  try { Object.defineProperty(f, 'length', { value: len, configurable: true }); } catch (e) {}\n"
"  return f;\n"
"}\n"
"function domErr(msg, name) { return new G.DOMException(msg, name); }\n"
"function illegal(name) { return named(function () { throw new TypeError('illegal constructor: ' + name); }, name, 0); }\n"
"\n"
/* ==== the store: name -> array of entries, insertion order preserved so
 * caches.keys()/match() walk caches in creation order as the spec assumes.
 * A JS Map (not a plain object) because a cache name is an arbitrary
 * string, including ones that collide with Object.prototype members
 * ('toString' is a real WPT case). ================================== */
"var CACHES = new G.Map();\n"
   /* Hard cap, named rather than silently absorbed: this store holds
      attacker-controlled bytes in the one process running adversary code.
      64 MiB total across every named cache -- generous for the corpus (the
      whole cache-storage/ test suite puts small fixed strings), tiny next
      to what an unbounded store could be asked to hold. */
"var CACHE_QUOTA_BYTES = $$CACHE_QUOTA_BYTES$$;\n"
"var totalBytes = 0;\n"
"\n"
"function entriesFor(name) {\n"
"  var e = CACHES.get(name);\n"
"  if (!e) { e = []; CACHES.set(name, e); }\n"
"  return e;\n"
"}\n"
"function toRequest(input) {\n"
"  if (input instanceof G.Request) return input;\n"
"  return new G.Request(String(input));\n"
"}\n"
"function normalizeOpts(o) {\n"
"  o = o || {};\n"
"  return {\n"
"    ignoreSearch: !!o.ignoreSearch,\n"
"    ignoreMethod: !!o.ignoreMethod,\n"
"    ignoreVary: !!o.ignoreVary,\n"
"    cacheName: o.cacheName !== undefined ? String(o.cacheName) : undefined\n"
"  };\n"
"}\n"
"function urlKey(u, ignoreSearch) {\n"
"  if (!ignoreSearch) return u;\n"
"  var i = u.indexOf('?');\n"
"  return i < 0 ? u : u.slice(0, i);\n"
"}\n"
"function pairsGet(pairs, name) {\n"
"  var k = name.toLowerCase(), found = null;\n"
"  for (var i = 0; i < pairs.length; i++)\n"
"    if (pairs[i][0].toLowerCase() === k) found = (found === null) ? pairs[i][1] : (found + ', ' + pairs[i][1]);\n"
"  return found;\n"
"}\n"
"function reqHeaderPairs(req) { var out = []; req.headers.forEach(function (v, k) { out.push([k, v]); }); return out; }\n"
"function resHeaderPairs(res) { var out = []; res.headers.forEach(function (v, k) { out.push([k, v]); }); return out; }\n"
"\n"
   /* fetch #cache-storage matching, Vary half: a stored entry's OWN vary
      header names which request headers had to agree for this entry to be
      the right variant. `Vary: *` never matches anything (put() already
      refuses to store one), so encountering it here would be a bug
      upstream, not a real corpus case -- treated as "no match" rather than
      a crash. */
"function varyMatches(entry, queryHeaders) {\n"
"  var vv = pairsGet(entry.resHeaders, 'vary');\n"
"  if (!vv) return true;\n"
"  var names = vv.split(',').map(function (s) { return s.trim(); }).filter(function (s) { return s.length; });\n"
"  if (names.indexOf('*') >= 0) return false;\n"
"  for (var i = 0; i < names.length; i++) {\n"
"    var a = pairsGet(entry.reqHeaders, names[i]);\n"
"    var b = queryHeaders ? queryHeaders.get(names[i]) : null;\n"
"    if (a !== b) return false;\n"
"  }\n"
"  return true;\n"
"}\n"
"function entryMatchesRequest(entry, req, opts) {\n"
"  if (!opts.ignoreMethod && req.method !== 'GET' && req.method !== 'HEAD') return false;\n"
"  if (urlKey(entry.url, opts.ignoreSearch) !== urlKey(req.url, opts.ignoreSearch)) return false;\n"
"  if (!opts.ignoreVary && !varyMatches(entry, req.headers)) return false;\n"
"  return true;\n"
"}\n"
"function findMatchesIn(entries, req, opts) {\n"
"  var out = [];\n"
"  for (var i = 0; i < entries.length; i++) if (entryMatchesRequest(entries[i], req, opts)) out.push(entries[i]);\n"
"  return out;\n"
"}\n"
"function entryToResponse(entry) {\n"
"  var h = new G.Headers();\n"
"  for (var i = 0; i < entry.resHeaders.length; i++) h.append(entry.resHeaders[i][0], entry.resHeaders[i][1]);\n"
"  var body = entry.body ? entry.body.slice(0) : null;\n"
"  return new G.Response(body, {\n"
"    status: entry.status, statusText: entry.statusText, headers: h,\n"
"    url: entry.url, type: entry.resType, __allowStatus0: entry.status === 0\n"
"  });\n"
"}\n"
"function entryToRequestObj(entry) {\n"
"  var h = new G.Headers();\n"
"  for (var i = 0; i < entry.reqHeaders.length; i++) h.append(entry.reqHeaders[i][0], entry.reqHeaders[i][1]);\n"
"  return new G.Request(entry.url, { method: entry.method, headers: h });\n"
"}\n"
"\n"
/* ==== Cache ==== */
"var CacheProto = {};\n"
"CacheProto.match = function (request, options) {\n"
"  var self = this;\n"
"  return new G.Promise(function (resolve, reject) {\n"
"    try {\n"
"      var req = toRequest(request), opts = normalizeOpts(options);\n"
"      var m = findMatchesIn(entriesFor(self._name), req, opts);\n"
"      resolve(m.length ? entryToResponse(m[0]) : undefined);\n"
"    } catch (e) { reject(e); }\n"
"  });\n"
"};\n"
"CacheProto.matchAll = function (request, options) {\n"
"  var self = this;\n"
"  return new G.Promise(function (resolve, reject) {\n"
"    try {\n"
"      var opts = normalizeOpts(options), entries = entriesFor(self._name), out;\n"
"      if (request === undefined) out = entries.slice();\n"
"      else out = findMatchesIn(entries, toRequest(request), opts);\n"
"      resolve(out.map(entryToResponse));\n"
"    } catch (e) { reject(e); }\n"
"  });\n"
"};\n"
"CacheProto.keys = function (request, options) {\n"
"  var self = this;\n"
"  return new G.Promise(function (resolve, reject) {\n"
"    try {\n"
"      var opts = normalizeOpts(options), entries = entriesFor(self._name), out;\n"
"      if (request === undefined) out = entries.slice();\n"
"      else out = findMatchesIn(entries, toRequest(request), opts);\n"
"      resolve(out.map(entryToRequestObj));\n"
"    } catch (e) { reject(e); }\n"
"  });\n"
"};\n"
"CacheProto['delete'] = function (request, options) {\n"
"  var self = this;\n"
"  return new G.Promise(function (resolve, reject) {\n"
"    try {\n"
"      var req = toRequest(request), opts = normalizeOpts(options);\n"
"      var entries = entriesFor(self._name), kept = [], freed = 0, removed = false;\n"
"      for (var i = 0; i < entries.length; i++) {\n"
"        if (entryMatchesRequest(entries[i], req, opts)) { removed = true; freed += entries[i].body ? entries[i].body.byteLength : 0; }\n"
"        else kept.push(entries[i]);\n"
"      }\n"
"      CACHES.set(self._name, kept);\n"
"      totalBytes -= freed;\n"
"      resolve(removed);\n"
"    } catch (e) { reject(e); }\n"
"  });\n"
"};\n"
   /* put(): the only path that touches a body. Every synchronous rejection
      below is a spec requirement (non-GET, non-http(s) scheme, 206, Vary:*,
      a used/locked body); everything after that point is a Promise chain,
      so a body read failure or the quota check rejects rather than hangs. */
"CacheProto.put = function (request, response) {\n"
"  var self = this, req;\n"
"  try {\n"
"    req = toRequest(request);\n"
"    if (req.method !== 'GET') throw new TypeError('Cache.put: request method must be GET');\n"
"    var scheme = null;\n"
"    try { scheme = new G.URL(req.url).protocol; } catch (eu) {}\n"
"    if (scheme !== 'http:' && scheme !== 'https:')\n"
"      throw new TypeError('Cache.put: only http/https requests can be cached');\n"
"    if (!(response instanceof G.Response)) throw new TypeError('Cache.put: response must be a Response');\n"
"    if (response.status === 206) throw new TypeError('Cache.put: a 206 Partial Content response cannot be cached');\n"
"    var varyAll = response.headers.get('vary');\n"
"    if (varyAll !== null && varyAll.split(',').some(function (s) { return s.trim() === '*'; }))\n"
"      throw new TypeError('Cache.put: a Vary: * response cannot be cached');\n"
"    if (response.bodyUsed) throw new TypeError('Cache.put: response body has already been used');\n"
"    if (response.body && response.body.locked) throw new TypeError('Cache.put: response body is locked');\n"
"  } catch (eSync) { return G.Promise.reject(eSync); }\n"
"  var reqHeaders = reqHeaderPairs(req);\n"
"  var toRead;\n"
"  try { toRead = response.body ? response.clone() : response; }\n"
"  catch (eClone) { return G.Promise.reject(eClone); }\n"
"  return toRead.arrayBuffer().then(function (buf) {\n"
"    var opts = { ignoreSearch: false, ignoreMethod: false, ignoreVary: false };\n"
"    var entries = entriesFor(self._name), kept = [], oldBytes = 0;\n"
"    for (var i = 0; i < entries.length; i++) {\n"
"      if (entryMatchesRequest(entries[i], req, opts)) oldBytes += entries[i].body ? entries[i].body.byteLength : 0;\n"
"      else kept.push(entries[i]);\n"
"    }\n"
"    if (totalBytes - oldBytes + buf.byteLength > CACHE_QUOTA_BYTES)\n"
"      throw domErr('cache storage quota exceeded (' + CACHE_QUOTA_BYTES + ' bytes total)', 'QuotaExceededError');\n"
"    kept.push({\n"
"      url: req.url, method: req.method, reqHeaders: reqHeaders,\n"
"      status: response.status, statusText: response.statusText,\n"
"      resHeaders: resHeaderPairs(response), resType: response.type, body: buf\n"
"    });\n"
"    CACHES.set(self._name, kept);\n"
"    totalBytes = totalBytes - oldBytes + buf.byteLength;\n"
"    return undefined;\n"
"  });\n"
"};\n"
"CacheProto.add = function (request) { return this.addAll([request]); };\n"
"CacheProto.addAll = function (requests) {\n"
"  var self = this, reqs;\n"
"  try {\n"
"    reqs = Array.prototype.map.call(requests, toRequest);\n"
"    var seen = [];\n"
"    for (var i = 0; i < reqs.length; i++) {\n"
"      if (reqs[i].method !== 'GET') throw new TypeError('Cache.addAll: request method must be GET');\n"
"      var key = reqs[i].method + ' ' + reqs[i].url;\n"
"      if (seen.indexOf(key) >= 0) throw new TypeError('Cache.addAll: duplicate request in the list');\n"
"      seen.push(key);\n"
"    }\n"
"  } catch (eSync) { return G.Promise.reject(eSync); }\n"
"  return G.Promise.all(reqs.map(function (r) { return G.fetch(r); })).then(function (responses) {\n"
"    for (var i = 0; i < responses.length; i++)\n"
"      if (!responses[i].ok)\n"
"        throw new TypeError('Cache.addAll: fetch for ' + reqs[i].url + ' returned status ' + responses[i].status);\n"
"    return G.Promise.all(responses.map(function (res, i) { return self.put(reqs[i], res); }));\n"
"  }).then(function () { return undefined; });\n"
"};\n"
"\n"
/* ==== CacheStorage ==== */
"var CacheStorageProto = {};\n"
"CacheStorageProto.open = function (cacheName) {\n"
"  return new G.Promise(function (resolve, reject) {\n"
"    try {\n"
"      if (typeof cacheName === 'symbol') throw new TypeError('CacheStorage.open: cache name must not be a Symbol');\n"
"      var name = String(cacheName);\n"
"      entriesFor(name);\n"
"      var h = Object.create(CacheProto); h._name = name;\n"
"      resolve(h);\n"
"    } catch (e) { reject(e); }\n"
"  });\n"
"};\n"
"CacheStorageProto.has = function (cacheName) {\n"
"  return new G.Promise(function (resolve, reject) {\n"
"    try {\n"
"      if (typeof cacheName === 'symbol') throw new TypeError('CacheStorage.has: cache name must not be a Symbol');\n"
"      resolve(CACHES.has(String(cacheName)));\n"
"    } catch (e) { reject(e); }\n"
"  });\n"
"};\n"
"CacheStorageProto['delete'] = function (cacheName) {\n"
"  return new G.Promise(function (resolve, reject) {\n"
"    try {\n"
"      if (typeof cacheName === 'symbol') throw new TypeError('CacheStorage.delete: cache name must not be a Symbol');\n"
"      var name = String(cacheName), had = CACHES.has(name);\n"
"      if (had) {\n"
"        var entries = CACHES.get(name), freed = 0;\n"
"        for (var i = 0; i < entries.length; i++) freed += entries[i].body ? entries[i].body.byteLength : 0;\n"
"        totalBytes -= freed;\n"
"        CACHES['delete'](name);\n"
"      }\n"
"      resolve(had);\n"
"    } catch (e) { reject(e); }\n"
"  });\n"
"};\n"
"CacheStorageProto.keys = function () {\n"
"  return new G.Promise(function (resolve) { resolve(Array.from(CACHES.keys())); });\n"
"};\n"
"CacheStorageProto.match = function (request, options) {\n"
"  return new G.Promise(function (resolve, reject) {\n"
"    try {\n"
"      var req = toRequest(request), opts = normalizeOpts(options);\n"
"      var names = opts.cacheName !== undefined ? [opts.cacheName] : Array.from(CACHES.keys());\n"
"      for (var i = 0; i < names.length; i++) {\n"
"        if (!CACHES.has(names[i])) continue;\n"
"        var m = findMatchesIn(CACHES.get(names[i]), req, opts);\n"
"        if (m.length) { resolve(entryToResponse(m[0])); return; }\n"
"      }\n"
"      resolve(undefined);\n"
"    } catch (e) { reject(e); }\n"
"  });\n"
"};\n"
"\n"
"function installIface(name, proto) {\n"
"  var C = illegal(name);\n"
"  C.prototype = proto; proto.constructor = C;\n"
"  Object.defineProperty(G, name, { value: C, writable: true, configurable: true });\n"
"  return C;\n"
"}\n"
"installIface('Cache', CacheProto);\n"
"installIface('CacheStorage', CacheStorageProto);\n"
"var cachesInstance = Object.create(CacheStorageProto);\n"
"Object.defineProperty(G, 'caches', { value: cachesInstance, writable: true, configurable: true, enumerable: true });\n"
"})(typeof globalThis !== 'undefined' ? globalThis : this);\n";

#ifdef JS_CACHE_NO_SETTLE
/* THE CONTROL FOR THE TERMINATION BAR ITSELF. Replaces Cache.prototype.put
 * with exactly the js_platform.h:66-70 shape: a REAL Promise -- `instanceof
 * Promise` holds -- whose executor never calls resolve or reject. Nothing
 * else in this file changes. tests/cache.mk's test-cache-negctl build links
 * this in, and the host gate's own quiescence check (every promise this
 * suite created must settle within a bounded pump) must go RED on exactly
 * the put()-shaped checks and pass on everything upstream of them. */
static const char *CACHE_NEGCTL_JS =
"(function (G) {\n"
"  if (!G.Cache) return;\n"
"  G.Cache.prototype.put = function () {\n"
"    return new G.Promise(function (resolve, reject) { void resolve; void reject; });\n"
"  };\n"
"})(typeof globalThis !== 'undefined' ? globalThis : this);\n";
#endif

/* One token, one substitution: replaces the literal "$$CACHE_QUOTA_BYTES$$"
 * in CACHE_JS with the decimal value of JS_CACHE_QUOTA_BYTES. Returns a
 * malloc'd buffer the caller must free; NULL on the (host-test-only, never
 * hit with the static string above) case where the token is missing. */
static char *cache_js_with_quota(void)
{
    static const char TOKEN[] = "$$CACHE_QUOTA_BYTES$$";
    const char *hit = strstr(CACHE_JS, TOKEN);
    if (!hit) return NULL;
    char num[32];
    snprintf(num, sizeof num, "%d", (int)JS_CACHE_QUOTA_BYTES);
    size_t pre = (size_t)(hit - CACHE_JS);
    size_t post = strlen(hit + (sizeof TOKEN - 1));
    size_t total = pre + strlen(num) + post + 1;
    char *out = malloc(total);
    if (!out) return NULL;
    memcpy(out, CACHE_JS, pre);
    memcpy(out + pre, num, strlen(num));
    memcpy(out + pre + strlen(num), hit + (sizeof TOKEN - 1), post + 1);
    return out;
}

void js_cache_install(JSContext *ctx)
{
    if (!ctx) return;
    char *js = cache_js_with_quota();
    const char *src = js ? js : CACHE_JS;   /* fallback: eval the raw string, which fails loudly below rather than silently installing no quota */
    JSValue r = JS_Eval(ctx, src, strlen(src), "<js_cache>", JS_EVAL_TYPE_GLOBAL);
    free(js);
    if (JS_IsException(r)) {
        /* Loud, not silent -- js_events.c's rule: a prelude that fails to
         * install must not leave the page thinking it has no caches for an
         * innocent reason. */
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        fprintf(stderr, "js_cache: install failed: %s\n", s ? s : "(unprintable)");
        if (s) JS_FreeCString(ctx, s);
        JSValue st = JS_GetPropertyStr(ctx, e, "stack");
        if (!JS_IsUndefined(st)) {
            const char *ss = JS_ToCString(ctx, st);
            if (ss) { fprintf(stderr, "%s\n", ss); JS_FreeCString(ctx, ss); }
        }
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);

#ifdef JS_CACHE_NO_SETTLE
    {
        JSValue n = JS_Eval(ctx, CACHE_NEGCTL_JS, strlen(CACHE_NEGCTL_JS),
                            "<js_cache_negctl>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(n)) {
            JSValue e = JS_GetException(ctx);
            const char *m = JS_ToCString(ctx, e);
            fprintf(stderr, "js_cache NEGCTL: stub did not install: %s\n", m ? m : "(unprintable)");
            fprintf(stderr, "js_cache NEGCTL: this build is identical to the real one -- its verdict is meaningless\n");
            if (m) JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, e);
            exit(2);
        }
        JS_FreeValue(ctx, n);
    }
#endif
}
