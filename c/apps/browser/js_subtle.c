/* 2026-09-10 correction to the historical scope below: operations now use
 * real crypto; generateKey, EC/OKP DER import/export, Ed25519/ECDSA sign and
 * verify, ECDH/X25519 deriveBits/deriveKey are implemented. AES-GCM, HMAC,
 * HKDF and PBKDF2 remain the symmetric consumers. RSA private operations,
 * wrap/unwrap and other AES modes still reject unsupported operations. */
/* crypto.subtle -- WebCryptoAPI, the validation half.
 *
 * TRIAGE, NOT A GUESS: WebCryptoAPI/'s WPT failure corpus is overwhelmingly
 * negative testing that never reaches key material. importKey_failures.js's
 * own comment says it plainly: these tests check that importKey/generateKey
 * throw the RIGHT kind of error for a wide combinatorial sweep of bad
 * parameters, and the spec's own algorithm puts every one of those checks
 * BEFORE anything that would need real cryptography -- normalize the
 * algorithm name, then check `usages` against what the (format, key type)
 * combination allows, then (for JWK only, because JWK is already a parsed
 * JS object) check the required member set. None of that touches a DER
 * byte or a modular exponentiation.
 *
 * WHAT THIS FILE DOES NOT DO, ON PURPOSE (rule 2: never stub to success)
 *   - No 'spki' or 'pkcs8' import/export. Those formats are ASN.1 DER, this
 *     file has no DER reader or writer, and reading a length prefix wrong
 *     is exactly the kind of bug that produces a plausible-looking wrong
 *     key instead of an error. importKey with either format validates
 *     usages (so "Bad usages"/"Empty usages" still fire correctly) and then
 *     refuses with NotSupportedError rather than pretending to parse.
 *   - Historical claim (corrected below): No actual digest/encrypt/decrypt/sign/verify/deriveBits/wrapKey
 *     result. c/crypto has real SHA-2/AES/ECDSA/Ed25519/X25519 primitives,
 *     but this browser's link line does not pull CRYPTO_SRC into
 *     browser.elf or wpt_test (Makefile:896, tests/wpt.mk:227) and this
 *     file's brief is explicit that that link change is out of scope here.
 *     So every operational method normalizes its algorithm and its key
 *     argument (so InvalidAccessError still fires for a mismatched key or
 *     a key missing the usage) and then rejects NotSupportedError. A page
 *     that calls sign() gets told plainly that it did not happen, rather
 *     than a signature nothing verifies.
 *   - No RSA key material at all (no spki/pkcs8/jwk n/e/d parsing). RSA
 *     import/export is real work (WebCryptoAPI/import_export/
 *     rsa_importKey.https.any.js) that belongs to its own change; this file
 *     only teaches the algorithm registry RSA's names so that RSA's OWN
 *     "Bad usages"/"Empty Usages" validation subtests -- which need no key
 *     material either -- pass for free through the same generic path.
 *
 * WHAT THIS FILE DOES DO, AND WHY IT IS NOT A STUB EITHER
 *   Symmetric (AES-*, HMAC) and EC/OKP (ECDSA, ECDH, Ed25519, X25519)
 *   'raw' and 'jwk' import/export is REAL, not fabricated: both formats are
 *   just bytes (raw) or a JSON object of base64url fields (jwk) with no
 *   ASN.1 in either, so "parsing" them is marshalling, not cryptography.
 *   A key built this way holds its actual key bytes and can be exported
 *   back out byte-for-byte -- it just cannot be used for an operation,
 *   because none of the operations are implemented (see above). That is
 *   the same honest half this tree ships elsewhere: c/crypto/crypto.h has
 *   RSA verify with no RSA sign, ecdsa_verify for two curves and
 *   ecdsa_sign for three -- a capability that is real but incomplete is
 *   reported as exactly that, never rounded up.
 *
 * NORMALIZATION IS ASCII-ONLY, DELIBERATELY. WebCryptoAPI/
 * normalize-algorithm-name.https.any.js feeds "HKDF" (U+212A KELVIN
 * SIGN in place of the K) and requires it NOT to match "HKDF". JavaScript's
 * String.prototype.toLowerCase() folds U+212A to 'k' (it is Unicode
 * lowercase-mapped to LATIN SMALL LETTER K), so using it here would make
 * the Kelvin sign match and the test wants the opposite. asciiUpper() below
 * touches only the 26 ASCII letters and leaves every other code point,
 * including U+212A, untouched -- so the Kelvin string canonicalizes to
 * itself and correctly fails to match "HKDF".
 */
/* Correction (2026-09-09): SHA-1/256/384/512 digest now returns real bytes
 * through js_digest.inc. A second correction in this batch adds HMAC-SHA-2,
 * HKDF/PBKDF2 and AES-GCM operations through js_crypto_ops.inc. The historical
 * refusal above remains beside this correction; RSA/EC operations, SHA-1 HMAC,
 * truncated GCM tags and random key generation still explicitly refuse. */
#include "quickjs.h"
#include "js_platform.h"
#include <string.h>

int printf(const char *, ...);
#include "js_digest.inc"
#include "js_crypto_ops.inc"
#include "js_crypto_keys.inc"

static const char *SUBTLE_PRELUDE =
"(function (nativeDigest, nativeHmac, nativeKdf, nativeGcm, nativeRandom, nativeKeyOp) {\n"
"'use strict';\n"
"var G = globalThis;\n"
"var c = G.crypto;\n"
"if (!c) { c = {}; try { G.crypto = c; } catch (e) { return; } }\n"
"if (c.subtle) return;\n"    /* installs-only-if-absent, same rule as the rest of this directory */

/* ==== the algorithm registry ============================================
 * One entry per algorithm this file knows the NAME of. `cls` decides how
 * import/export and usage validation treat it:
 *   sym  -- AES-*, HMAC: secret key, 'raw'+'jwk' formats
 *   kdf  -- HKDF, PBKDF2: secret key, 'raw' format ONLY (spec: no jwk)
 *   ec   -- ECDSA, ECDH: public/private, 'raw' (public point)+'spki'+
 *           'pkcs8'+'jwk'
 *   okp  -- Ed25519, X25519: same shape as ec but curve is implied by name
 *   rsa  -- RSASSA-PKCS1-v1_5, RSA-PSS, RSA-OAEP: public/private,
 *           'spki'+'pkcs8'+'jwk' (no 'raw' -- RSA has never had one)
 * `hash: true` means the algorithm dictionary carries a required `hash`
 * member that is ITSELF normalized recursively (HmacImportParams,
 * RsaHashedImportParams) -- this is how {name:'HMAC', hash:'MD5'} correctly
 * fails algorithm normalization with NotSupportedError before usages are
 * ever looked at, exactly as WebCryptoAPI/generateKey/failures.js expects.
 * `usages` gives the allowed KeyUsage set per key type; an absent key type
 * (e.g. 'public' for AES) is simply never looked up for that cls. */
"var EC_FLEN = { 'P-256': 32, 'P-384': 48, 'P-521': 66 };\n"
"var ALG = {\n"
"  'AES-CTR': { cls: 'sym', formats: ['raw','jwk'], usages: { secret: ['encrypt','decrypt','wrapKey','unwrapKey'] } },\n"
"  'AES-CBC': { cls: 'sym', formats: ['raw','jwk'], usages: { secret: ['encrypt','decrypt','wrapKey','unwrapKey'] } },\n"
"  'AES-GCM': { cls: 'sym', formats: ['raw','jwk'], usages: { secret: ['encrypt','decrypt','wrapKey','unwrapKey'] } },\n"
"  'AES-KW':  { cls: 'sym', formats: ['raw','jwk'], usages: { secret: ['wrapKey','unwrapKey'] } },\n"
"  'HMAC':    { cls: 'sym', formats: ['raw','jwk'], hash: true, usages: { secret: ['sign','verify'] } },\n"
"  'HKDF':    { cls: 'kdf', formats: ['raw'], usages: { secret: ['deriveKey','deriveBits'] } },\n"
"  'PBKDF2':  { cls: 'kdf', formats: ['raw'], usages: { secret: ['deriveKey','deriveBits'] } },\n"
"  'RSASSA-PKCS1-v1_5': { cls: 'rsa', formats: ['spki','pkcs8','jwk'], hash: true, usages: { public: ['verify'], private: ['sign'] } },\n"
"  'RSA-PSS':  { cls: 'rsa', formats: ['spki','pkcs8','jwk'], hash: true, usages: { public: ['verify'], private: ['sign'] } },\n"
"  'RSA-OAEP': { cls: 'rsa', formats: ['spki','pkcs8','jwk'], hash: true, usages: { public: ['encrypt','wrapKey'], private: ['decrypt','unwrapKey'] } },\n"
"  'ECDSA': { cls: 'ec', formats: ['raw','spki','pkcs8','jwk'], usages: { public: ['verify'], private: ['sign'] } },\n"
"  'ECDH':  { cls: 'ec', formats: ['raw','spki','pkcs8','jwk'], usages: { public: [], private: ['deriveKey','deriveBits'] } },\n"
"  'Ed25519': { cls: 'okp', formats: ['raw','spki','pkcs8','jwk'], usages: { public: ['verify'], private: ['sign'] } },\n"
"  'X25519':  { cls: 'okp', formats: ['raw','spki','pkcs8','jwk'], usages: { public: [], private: ['deriveKey','deriveBits'] } }\n"
"};\n"
"var DIGESTS = ['SHA-1','SHA-256','SHA-384','SHA-512'];\n"

/* ASCII-only case fold -- see the file header for why this must NOT be
 * String.prototype.toUpperCase(). */
"function asciiUpper(s) {\n"
"  var out = '';\n"
"  for (var i = 0; i < s.length; i++) {\n"
"    var code = s.charCodeAt(i);\n"
"    if (code >= 0x61 && code <= 0x7A) code -= 32;\n"
"    out += String.fromCharCode(code);\n"
"  }\n"
"  return out;\n"
"}\n"
"var ALG_BY_UPPER = {};\n"
"for (var _k in ALG) ALG_BY_UPPER[asciiUpper(_k)] = _k;\n"
"var DIGEST_BY_UPPER = {};\n"
"DIGESTS.forEach(function (d) { DIGEST_BY_UPPER[asciiUpper(d)] = d; });\n"

/* WebIDL's `(DOMString or Object) AlgorithmIdentifier` conversion to the
 * required-member `Algorithm` dictionary: a bare string is shorthand for
 * {name: string}; an object must carry a `name`, or the WebIDL required-
 * member check fails with TypeError BEFORE algorithm lookup even runs --
 * which is exactly why `{}` gets TypeError and a typo'd name gets
 * NotSupportedError (WebCryptoAPI/generateKey/failures.js's own split). */
"function algDictFrom(input) {\n"
"  if (typeof input === 'string') return { name: input };\n"
"  if (input !== null && typeof input === 'object') {\n"
"    if (input.name === undefined)\n"
"      throw new TypeError(\"CryptoOperationData: 'name' member is required\");\n"
"    return input;\n"
"  }\n"
"  throw new TypeError('Algorithm must be a string or an object with a name');\n"
"}\n"
"function normalizeDigest(input) {\n"
"  var dict = algDictFrom(input);\n"
"  var canon = DIGEST_BY_UPPER[asciiUpper(String(dict.name))];\n"
"  if (canon === undefined)\n"
"    throw new G.DOMException(String(dict.name) + ' is not a supported digest algorithm', 'NotSupportedError');\n"
"  return canon;\n"
"}\n"
/* Returns {name, entry, hash, raw}. `hash` is the recursively-normalized
 * canonical digest name when entry.hash is true, else undefined. Throws
 * TypeError (bad dictionary) or DOMException NotSupportedError (unknown
 * name, or unknown hash for an algorithm that requires one). */
"function normalizeAlg(input) {\n"
"  var dict = algDictFrom(input);\n"
"  var canon = ALG_BY_UPPER[asciiUpper(String(dict.name))];\n"
"  if (canon === undefined)\n"
"    throw new G.DOMException(String(dict.name) + ' is not a supported algorithm name', 'NotSupportedError');\n"
"  var entry = ALG[canon];\n"
"  var hash;\n"
"  if (entry.hash) {\n"
"    if (dict.hash === undefined)\n"
"      throw new TypeError(canon + \": 'hash' member is required\");\n"
"    hash = normalizeDigest(dict.hash);\n"
"  }\n"
"  return { name: canon, entry: entry, hash: hash, raw: dict };\n"
"}\n"

/* ==== CryptoKey ==========================================================
 * Slots live in a WeakMap rather than as own properties: type/extractable/
 * algorithm/usages are spec'd as accessor properties on the PROTOTYPE, and
 * crypto_key_cached_slots.https.any.js (2 subtests) requires that repeated
 * `key.algorithm`/`key.usages` gets return the SAME object identity -- so
 * the slot's stored object is returned as-is, never rebuilt per get. */
"var arrayIndexOf = Function.prototype.call.bind(Array.prototype.indexOf);\n"
"var arraySlice = Function.prototype.call.bind(Array.prototype.slice);\n"
"var KeySlots = new WeakMap();\n"

"var getSlot = KeySlots.get.bind(KeySlots), setSlot = KeySlots.set.bind(KeySlots);\n"

"function CryptoKey() { throw new TypeError('Illegal constructor'); }\n"
"function slotOf(k) {\n"
"  var s = getSlot(k);\n"
"  if (!s) throw new TypeError('not a CryptoKey');\n"
"  return s;\n"
"}\n"
"Object.defineProperties(CryptoKey.prototype, {\n"
"  type: { configurable: true, get: function () { return slotOf(this).type; } },\n"
"  extractable: { configurable: true, get: function () { return slotOf(this).extractable; } },\n"
"  algorithm: { configurable: true, get: function () { return slotOf(this).algorithm; } },\n"
"  usages: { configurable: true, get: function () { return slotOf(this).usages; } }\n"
"});\n"
/* `material` is never exposed to script -- it is what exportKey reads. */
"function makeCryptoKey(type, extractable, algorithm, usages, material) {\n"
"  var k = Object.create(CryptoKey.prototype);\n"
/* MEASURED (util/helpers.js:213-214, assert_goodCryptoKey's own comment):
 * "The usages parameter could have repeats, but the usages property of the
 * result should not." A caller-supplied ["encrypt","decrypt","encrypt"]
 * must come back deduplicated -- storing it verbatim is a real defect this
 * corpus catches (WebCryptoAPI/import_export/symmetric_importKey), not a
 * cosmetic mismatch. */
"  var seenUsage = {}, dedupedUsages = [];\n"
"  usages.forEach(function (u) { if (!seenUsage[u]) { seenUsage[u] = true; dedupedUsages.push(u); } });\n"
"  setSlot(k, { type: type, extractable: !!extractable, algorithm: algorithm,\n"
"                     usages: arraySlice(dedupedUsages), allowedUsages: dedupedUsages,\n"
"                     authAlgorithm: {name:algorithm.name, hash:algorithm.hash ? {name:algorithm.hash.name} : undefined, length:algorithm.length, namedCurve:algorithm.namedCurve}, material: material });\n"

"  return k;\n"
"}\n"
"G.CryptoKey = CryptoKey;\n"
/* MEASURED (util/helpers.js:218): every 'Good parameters' happy-path check
 * ends with `key[Symbol.toStringTag] === 'CryptoKey'`. A plain
 * Object.create(CryptoKey.prototype) instance does not get that for free --
 * QuickJS's default toString tag falls through to 'Object' for a class with
 * no explicit tag, same as V8/SpiderMonkey. */
"Object.defineProperty(CryptoKey.prototype, Symbol.toStringTag, { value: 'CryptoKey', configurable: true });\n"
"function SubtleCrypto() { throw new TypeError('Illegal constructor'); }\n"
"G.SubtleCrypto = SubtleCrypto;\n"

/* ==== base64url, and only base64url -- JWK's own encoding (RFC 7515 App C:
 * unpadded, '-'/'_' in place of '+'/'/'). A padded-only decoder silently
 * fails every JWK fixture in this corpus, which is written unpadded. */
"function b64urlDecode(s) {\n"
"  if (typeof s !== 'string') throw new G.DOMException('JWK field is not a string', 'DataError');\n"
"  var t = s.replace(/-/g, '+').replace(/_/g, '/');\n"
"  while (t.length % 4) t += '=';\n"
"  var bin;\n"
"  try { bin = atob(t); } catch (e) { throw new G.DOMException('malformed base64url', 'DataError'); }\n"
"  var out = new Uint8Array(bin.length);\n"
"  for (var i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);\n"
"  return out;\n"
"}\n"
"function b64urlEncode(bytes) {\n"
"  var bin = '';\n"
"  for (var i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);\n"
"  return btoa(bin).replace(/\\+/g, '-').replace(/\\//g, '_').replace(/=+$/, '');\n"
"}\n"
"function toBytes(keyData) {\n"
"  if (keyData instanceof ArrayBuffer) return new Uint8Array(keyData);\n"
"  if (ArrayBuffer.isView(keyData)) {\n"
"    if (!(keyData.buffer instanceof ArrayBuffer)) throw new TypeError('shared buffers are not accepted');\n"
"    return new Uint8Array(keyData.buffer, keyData.byteOffset, keyData.byteLength);\n"
"  }\n"

"  throw new TypeError('keyData must be a BufferSource');\n"
"}\n"

/* ==== importKey: usages, THEN (raw|jwk only) the key material ==========
 * Order matters and is the whole point of this file: usages is checked
 * against the (format, key-type) pair BEFORE keyData is touched, so every
 * 'Bad usages'/'Empty usages' case fails with SyntaxError even for
 * 'spki'/'pkcs8', which this file never parses at all. */
"function usagesOk(requested, allowed) {\n"
"  for (var i = 0; i < requested.length; i++)\n"
"    if (arrayIndexOf(allowed,requested[i]) === -1) return false;\n"
"  return true;\n"
"}\n"
"function reqField(jwk, name) {\n"
"  if (jwk[name] === undefined)\n"
"    throw new G.DOMException(\"Missing JWK '\" + name + \"' parameter\", 'DataError');\n"
"  return jwk[name];\n"
"}\n"
/* jwk needs no DER at all -- it is already a parsed JS object, which is
 * exactly why this path can go all the way to a real, usable (if never
 * operated on) CryptoKey while 'spki'/'pkcs8' cannot. */
"function parseJwk(entry, algName, jwk, keyType, algorithm, extractable) {\n"
"  var kty = reqField(jwk, 'kty');\n"
"  var wantKty = (entry.cls === 'sym' || entry.cls === 'kdf') ? 'oct' :\n"
"                entry.cls === 'ec' ? 'EC' : entry.cls === 'okp' ? 'OKP' : 'RSA';\n"
"  if (kty !== wantKty)\n"
"    throw new G.DOMException(\"Invalid 'kty' field\", 'DataError');\n"
"  if (jwk.ext === false && extractable)\n"
"    throw new G.DOMException('Cannot import a non-extractable JWK as extractable', 'DataError');\n"
"  if (jwk.use !== undefined && jwk.use !== 'sig' && jwk.use !== 'enc')\n"
"    throw new G.DOMException(\"Invalid 'use' field\", 'DataError');\n"
"  var out = {};\n"
"  if (entry.cls === 'sym' || entry.cls === 'kdf') {\n"
"    out.k = b64urlDecode(reqField(jwk, 'k'));\n"
"    if (/^AES-/.test(algName) && out.k.length !== 16 && out.k.length !== 24 && out.k.length !== 32)\n"
"      throw new G.DOMException('Invalid AES key length', 'DataError');\n"
"  } else if (entry.cls === 'ec') {\n"
"    var crv = reqField(jwk, 'crv');\n"
"    var wantCrv = algorithm && typeof algorithm === 'object' ? algorithm.namedCurve : undefined;\n"
"    if (wantCrv !== undefined && crv !== wantCrv)\n"
"      throw new G.DOMException(\"Invalid 'crv' field\", 'DataError');\n"
"    var flen = EC_FLEN[crv];\n"
"    if (flen === undefined)\n"
"      throw new G.DOMException(\"Invalid 'crv' field\", 'DataError');\n"
"    out.x = b64urlDecode(reqField(jwk, 'x'));\n"
"    out.y = b64urlDecode(reqField(jwk, 'y'));\n"
"    if (out.x.length !== flen || out.y.length !== flen)\n"
"      throw new G.DOMException('Bad EC key length', 'DataError');\n"
"    if (keyType === 'private') {\n"
"      out.d = b64urlDecode(reqField(jwk, 'd'));\n"
"      if (out.d.length !== flen) throw new G.DOMException('Bad EC key length', 'DataError');\n"
"    }\n"
"  } else if (entry.cls === 'okp') {\n"
"    var crv2 = reqField(jwk, 'crv');\n"
"    if (crv2 !== algName)\n"
"      throw new G.DOMException(\"Invalid 'crv' field\", 'DataError');\n"
"    out.x = b64urlDecode(reqField(jwk, 'x'));\n"
"    if (out.x.length !== 32) throw new G.DOMException('Bad OKP key length', 'DataError');\n"
"    if (keyType === 'private') {\n"
"      out.d = b64urlDecode(reqField(jwk, 'd'));\n"
"      if (out.d.length !== 32) throw new G.DOMException('Bad OKP key length', 'DataError');\n"
"    }\n"
/* MEASURED (import_export/importKey_failures.js): 'alg' casing is checked
 * ONLY for Ed25519 in this corpus (Ed448 is unregistered here and X25519/
 * ECDH have no 'alg' semantics), and it is an EXACT string compare --
 * 'ed25519'/'ED25519' must both be rejected, so no case-folding here. */
"    if (algName === 'Ed25519' && jwk.alg !== undefined && jwk.alg !== 'EdDSA' && jwk.alg !== 'Ed25519')\n"
"      throw new G.DOMException(\"Invalid 'alg' field '\" + jwk.alg + \"'\", 'DataError');\n"
"  } else {\n" /* rsa: presence-only, no modulus/exponent validation (no RSA math here) */
"    out.n = b64urlDecode(reqField(jwk, 'n'));\n"
"    out.e = b64urlDecode(reqField(jwk, 'e'));\n"
"    if (keyType === 'private') out.d = b64urlDecode(reqField(jwk, 'd'));\n"
"  }\n"
"  return out;\n"
"}\n"
"function parseRaw(entry, algName, keyData, algorithm) {\n"
"  var bytes = toBytes(keyData);\n"
"  if (entry.cls === 'sym') {\n"
"    if (/^AES-/.test(algName) && bytes.length !== 16 && bytes.length !== 24 && bytes.length !== 32)\n"
"      throw new G.DOMException('Invalid AES key length', 'DataError');\n"
"    return { k: bytes.slice() };\n"
"  }\n"
"  if (entry.cls === 'kdf') return { k: bytes.slice() };\n" /* any length, including zero */
"  if (entry.cls === 'okp') {\n"
"    if (bytes.length !== 32) throw new G.DOMException('Invalid key length', 'DataError');\n"
"    return { x: bytes.slice() };\n"
"  }\n"
/* ec raw: uncompressed point, 0x04 || X || Y (SEC1 4.3.6). */
"  var flen = EC_FLEN[algorithm && algorithm.namedCurve];\n"
"  if (flen === undefined)\n"
"    throw new G.DOMException(String(algorithm && algorithm.namedCurve) + ' is not a supported named curve', 'NotSupportedError');\n"
"  if (bytes.length !== 1 + 2 * flen || bytes[0] !== 0x04)\n"
"    throw new G.DOMException('Invalid EC point length', 'DataError');\n"
"  return { x: bytes.slice(1, 1 + flen), y: bytes.slice(1 + flen) };\n"
"}\n"
"function buildAlgorithmObject(entry, norm, inputAlgorithm, material) {\n"
"  var out = { name: norm.name };\n"
"  if (entry.cls === 'ec') {\n"
"    out.namedCurve = inputAlgorithm && typeof inputAlgorithm === 'object' ? inputAlgorithm.namedCurve : undefined;\n"
"  } else if (entry.hash) {\n"
"    out.hash = { name: norm.hash };\n"
"    if (norm.name === 'HMAC')\n"
"      out.length = (inputAlgorithm && inputAlgorithm.length) || (material && material.k ? material.k.length * 8 : undefined);\n"
"  } else if (entry.cls === 'sym' && /^AES-/.test(norm.name)) {\n"
"    out.length = material && material.k ? material.k.length * 8 : undefined;\n"
"  }\n"
"  return out;\n"
"}\n"
"SubtleCrypto.prototype.importKey = function (format, keyData, algorithm, extractable, usages) {\n"
"  return new Promise(function (resolve, reject) {\n"
"    try {\n"
"      format = String(format);\n"
"      usages = usages ? arraySlice(usages) : [];\n"
"      var norm = normalizeAlg(algorithm);\n"
"      var entry = norm.entry;\n"
"      if (arrayIndexOf(entry.formats,format) === -1)\n"
"        throw new G.DOMException(norm.name + \" does not support the '\" + format + \"' import format\", 'NotSupportedError');\n"
"      var jwk = null, keyType;\n"
"      if (format === 'jwk') {\n"
"        if (keyData === null || typeof keyData !== 'object' || keyData instanceof ArrayBuffer || ArrayBuffer.isView(keyData))\n"
"          throw new TypeError('jwk keyData must be a JsonWebKey object');\n"
"        jwk = keyData;\n"
"        keyType = (entry.cls === 'sym' || entry.cls === 'kdf') ? 'secret' : (jwk.d !== undefined ? 'private' : 'public');\n"
"      } else if (format === 'raw') {\n"
"        keyType = (entry.cls === 'sym' || entry.cls === 'kdf') ? 'secret' : 'public';\n"
"      } else if (format === 'pkcs8') {\n"
"        keyType = 'private';\n"
"      } else {\n" /* spki */
"        keyType = 'public';\n"
"      }\n"
"      var allowed = entry.usages[keyType] || [];\n"
"      if (!usagesOk(usages, allowed))\n"
"        throw new G.DOMException('usages contains an entry not valid for this key/algorithm', 'SyntaxError');\n"
/* Only private/secret keys are required to carry at least one usage --
 * import_export/importKey_failures.js's 'Empty usages' generator only ever
 * targets pkcs8/jwk-PRIVATE, never a public-key format, and ECDH/X25519
 * public keys legitimately have an EMPTY allowed-usages set. */
"      if (usages.length === 0 && keyType !== 'public')\n"
"        throw new G.DOMException('usages must not be empty for a ' + keyType + ' key', 'SyntaxError');\n"
"      var material;\n"
"      if (format === 'jwk') material = parseJwk(entry, norm.name, jwk, keyType, algorithm, extractable);\n"
"      else if (format === 'raw') material = parseRaw(entry, norm.name, keyData, algorithm);\n"
"      else throw new G.DOMException(format + ' import is not supported in this build (no DER reader -- see js_subtle.c)', 'NotSupportedError');\n"
"      if (entry.cls === 'kdf' && extractable)\n"
"        throw new G.DOMException('KDF base keys must be non-extractable', 'SyntaxError');\n"
"      if (norm.name === 'HMAC') {\n"
"        var bits = material.k.length * 8;\n"
"        if (!bits) throw new G.DOMException('HMAC key is empty', 'DataError');\n"
"        if (norm.raw.length !== undefined) {\n"
"          var wanted = uint32(norm.raw.length, 'length');\n"
"          if (wanted > bits || wanted <= bits - 8) throw new G.DOMException('invalid HMAC length', 'DataError');\n"
"          if (wanted % 8) throw new G.DOMException('bit-granular HMAC keys are not supported', 'NotSupportedError');\n"
"        }\n"
"      }\n"
"      validateAsymmetric(norm,keyType,material);\n"
"      if (jwk) {\n"
"        if (jwk.key_ops !== undefined && (!Array.isArray(jwk.key_ops) || new Set(jwk.key_ops).size !== jwk.key_ops.length || !usagesOk(usages,jwk.key_ops)))\n"
"          throw new G.DOMException('JWK key_ops does not permit requested usages', 'DataError');\n"
"        var wantUse = norm.name==='HMAC'||norm.name==='ECDSA'||norm.name==='Ed25519' ? 'sig' : 'enc';\n"
"        if (usages.length && jwk.use !== undefined && jwk.use !== wantUse)\n"
"          throw new G.DOMException('JWK use does not match algorithm', 'DataError');\n"
"        if (entry.cls==='sym' && jwk.alg !== undefined && jwk.alg !== jwaAlg(norm.name,norm.hash,material.k))\n"
"          throw new G.DOMException('JWK alg does not match algorithm', 'DataError');\n"
"      }\n"
"      var algOut = buildAlgorithmObject(entry, norm, algorithm, material);\n"
"      resolve(makeCryptoKey(keyType, extractable, algOut, usages, material));\n"
"    } catch (e) { reject(e); }\n"
"  });\n"
"};\n"

/* RFC 7518 / the WebCrypto JWK registry's 'alg' values. equalJwk() (util/
 * helpers.js:414-429) only checks fields the FIXTURE has, so emitting 'alg'
 * where the fixture doesn't is harmless -- but where the fixture DOES carry
 * one (every HMAC/AES-* jwk fixture in this corpus does), a round trip that
 * drops it is a real 'the export lost a field' bug, not a false negative. */
"function jwaAlg(algName, hashName, keyBytes) {\n"
"  if (algName === 'HMAC')\n"
"    return { 'SHA-1': 'HS1', 'SHA-256': 'HS256', 'SHA-384': 'HS384', 'SHA-512': 'HS512' }[hashName];\n"
"  var bits = keyBytes ? keyBytes.length * 8 : undefined;\n"
"  if (algName === 'AES-CTR') return 'A' + bits + 'CTR';\n"
"  if (algName === 'AES-CBC') return 'A' + bits + 'CBC';\n"
"  if (algName === 'AES-GCM') return 'A' + bits + 'GCM';\n"
"  if (algName === 'AES-KW') return 'A' + bits + 'KW';\n"
"  if (algName === 'Ed25519') return 'EdDSA';\n"
"  return undefined;\n"
"}\n"
"function materialToJwk(entry, slot) {\n"
"  var m = slot.material, out = {};\n"
"  if (entry.cls === 'sym' || entry.cls === 'kdf') {\n"
"    out.kty = 'oct'; out.k = b64urlEncode(m.k);\n"
"  } else if (entry.cls === 'ec') {\n"
"    out.kty = 'EC'; out.crv = slot.authAlgorithm.namedCurve;\n"
"    out.x = b64urlEncode(m.x); out.y = b64urlEncode(m.y);\n"
"    if (slot.type === 'private') out.d = b64urlEncode(m.d);\n"
"  } else if (entry.cls === 'okp') {\n"
"    out.kty = 'OKP'; out.crv = slot.authAlgorithm.name;\n"
"    out.x = b64urlEncode(m.x);\n"
"    if (slot.type === 'private') out.d = b64urlEncode(m.d);\n"
"  } else {\n"
"    out.kty = 'RSA'; out.n = b64urlEncode(m.n); out.e = b64urlEncode(m.e);\n"
"    if (slot.type === 'private' && m.d) out.d = b64urlEncode(m.d);\n"
"  }\n"
"  var alg = jwaAlg(slot.authAlgorithm.name, slot.authAlgorithm.hash && slot.authAlgorithm.hash.name, m.k);\n"
"  if (alg !== undefined) out.alg = alg;\n"
"  out.ext = slot.extractable;\n"
"  out.key_ops = arraySlice(slot.allowedUsages);\n"
"  return out;\n"
"}\n"
"SubtleCrypto.prototype.exportKey = function (format, key) {\n"
"  return new Promise(function (resolve, reject) {\n"
"    try {\n"
"      var slot = slotOf(key);\n"

"      if (!slot.extractable)\n"
"        throw new G.DOMException('key is not extractable', 'InvalidAccessError');\n"
"      if (!slot.material)\n"
"        throw new G.DOMException('export of this key is not supported in this build', 'NotSupportedError');\n"
"      var entry = ALG[slot.authAlgorithm.name];\n"
"      if (format === 'jwk') { resolve(materialToJwk(entry, slot)); return; }\n"
"      if (format === 'raw') {\n"
"        var bytes = slot.material.k || (slot.type === 'public' ? slot.material.x : undefined);\n"
"        if (!bytes)\n"
"          throw new G.DOMException('raw export is not supported for this key', 'NotSupportedError');\n"
"        resolve(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));\n"
"        return;\n"
"      }\n"
"      throw new G.DOMException(format + ' export is not supported in this build (no DER writer)', 'NotSupportedError');\n"
"    } catch (e) { reject(e); }\n"
"  });\n"
"};\n"

/* ==== generateKey: algorithm normalization is the whole reachable surface.
 * Real key generation needs RNG + (for RSA) a from-scratch keygen this tree
 * does not have wired to this file; every recognized algorithm therefore
 * still rejects, honestly, rather than fabricating key material a page
 * would then try to use. */
"SubtleCrypto.prototype.generateKey = function (algorithm, extractable, usages) {\n"
"  return new Promise(function (resolve, reject) {\n"
"    try {\n"
"      var norm = normalizeAlg(algorithm);\n"
"      reject(new G.DOMException(norm.name + ' key generation is not supported in this build', 'NotSupportedError'));\n"
"    } catch (e) { reject(e); }\n"
"  });\n"
"};\n"

/* ==== digest: SHA bytes from the production hash translation units. */
"SubtleCrypto.prototype.digest = function (algorithm, data) {\n"
"  return new Promise(function (resolve, reject) {\n"
"    try {\n"
"      var bytes = new Uint8Array(toBytes(data));\n"
"      var name = normalizeDigest(algorithm);\n"
"      resolve(nativeDigest(name, bytes.buffer));\n"
"    } catch (e) { reject(e); }\n"
"  });\n"
"};\n"

/* ==== the remaining operations: normalize algorithm AND key, then refuse.
 * Checking the key (right algorithm, right usage) before refusing is not
 * decoration -- WebCryptoAPI/normalize-algorithm-name.https.any.js's
 * deriveBits case only reaches NotSupportedError because normalizeAlg()
 * itself throws first (Kelvin-mangled name), and a caller that passes a
 * key which doesn't support the operation at all should learn THAT, not a
 * generic 'not supported'. */
/* The old check read key.algorithm/key.usages directly. Those are mutable JS
 * objects, so a verify-only key could be changed into a signing key. Keep the
 * required cached public objects, but use separate unexposed metadata and a
 * captured WeakMap getter for all key authorization and export decisions. */
"function checkKeyFor(key, algName, usage) {\n"
"  var slot = slotOf(key);\n"
"  if (slot.authAlgorithm.name !== algName)\n"
"    throw new G.DOMException('key algorithm does not match', 'InvalidAccessError');\n"
"  if (arrayIndexOf(slot.allowedUsages,usage) === -1)\n"
"    throw new G.DOMException('key usages do not include ' + usage, 'InvalidAccessError');\n"
"  return slot;\n"
"}\n"
"function operationAlg(input) {\n"
"  var dict = algDictFrom(input), name = ALG_BY_UPPER[asciiUpper(String(dict.name))];\n"
"  if (name === undefined) throw new G.DOMException('unsupported algorithm', 'NotSupportedError');\n"
"  return {name:name, raw:dict};\n"
"}\n"
"function hashSize(name) {\n"
"  var n = {'SHA-256':32,'SHA-384':48,'SHA-512':64}[name];\n"
"  if (!n) throw new G.DOMException('HMAC/KDF SHA-1 backend is not available', 'NotSupportedError');\n"
"  return n;\n"
"}\n"
"function uint32(value, label) {\n"
"  var n = Number(value);\n"
"  if (!Number.isFinite(n)) throw new TypeError(label + ' must be finite');\n"
"  n = Math.trunc(n);\n"
"  if (n < 0 || n > 4294967295) throw new TypeError(label + ' is out of range');\n"
"  return n;\n"
"}\n"
"function bufferCopy(value) { return new Uint8Array(toBytes(value)).buffer; }\n"
"function operationFailure(fn) {\n"
"  try { return fn(); } catch(e) { throw new G.DOMException(String(e), 'OperationError'); }\n"
"}\n"
"function opStub(label, usage, keyArgIndex) {\n"
"  return function () {\n"
"    var args = arguments, algorithm = args[0], key = args[keyArgIndex];\n"
"    return new Promise(function (resolve, reject) {\n"
"      try {\n"
"        var norm = operationAlg(algorithm);\n"
"        checkKeyFor(key, norm.name, usage);\n"
"        reject(new G.DOMException(norm.name + ' ' + label + ' is not supported in this build', 'NotSupportedError'));\n"
"      } catch (e) { reject(e); }\n"
"    });\n"
"  };\n"
"}\n"
/* HMAC sign normalizes only the algorithm NAME. The previous generic
 * normalizeAlg incorrectly demanded a hash for sign('HMAC',...), although the
 * hash belongs to the imported key. KDF hash/salt/info are operation params. */
"SubtleCrypto.prototype.sign = function(algorithm,key,data) {\n"
"  return new Promise(function(resolve,reject){ try {\n"
"    var norm=operationAlg(algorithm), slot=checkKeyFor(key,norm.name,'sign');\n"
"    if(norm.name !== 'HMAC') throw new G.DOMException('sign algorithm is not implemented','NotSupportedError');\n"
"    var hlen=hashSize(slot.authAlgorithm.hash.name), bytes=bufferCopy(data);\n"
"    resolve(operationFailure(function(){return nativeHmac(hlen,slot.material.k.buffer,bytes)}));\n"
"  } catch(e){reject(e)} });\n"
"};\n"
"SubtleCrypto.prototype.verify = function(algorithm,key,signature,data) {\n"
"  return new Promise(function(resolve,reject){ try {\n"
"    var norm=operationAlg(algorithm), slot=checkKeyFor(key,norm.name,'verify');\n"
"    if(norm.name !== 'HMAC') throw new G.DOMException('verify algorithm is not implemented','NotSupportedError');\n"
"    var hlen=hashSize(slot.authAlgorithm.hash.name), bytes=bufferCopy(data), sig=bufferCopy(signature);\n"
"    resolve(operationFailure(function(){return nativeHmac(hlen,slot.material.k.buffer,bytes,sig)}));\n"
"  } catch(e){reject(e)} });\n"
"};\n"
"function derive(algorithm,key,length,usage) {\n"
"  var norm=operationAlg(algorithm), dict=norm.raw;\n"
"  if(norm.name !== 'HKDF' && norm.name !== 'PBKDF2') throw new G.DOMException('derivation is not implemented','NotSupportedError');\n"
"  if(dict.hash===undefined || dict.salt===undefined || (norm.name==='HKDF' ? dict.info===undefined : dict.iterations===undefined))\n"
"    throw new TypeError('derivation dictionary is missing a required member');\n"
"  var hlen=hashSize(normalizeDigest(dict.hash)), salt=bufferCopy(dict.salt);\n"
"  var info=norm.name==='HKDF'?bufferCopy(dict.info):new ArrayBuffer(0);\n"
"  var iterations=norm.name==='PBKDF2'?uint32(dict.iterations,'iterations'):0;\n"
"  var slot=checkKeyFor(key,norm.name,usage);\n"
"  if(length===null || length===undefined) throw new G.DOMException('length must be specified','OperationError');\n"
"  var bits=uint32(length,'length');\n"
"  if(bits%8 || (norm.name==='PBKDF2' && iterations===0)) throw new G.DOMException('invalid derivation length or iterations','OperationError');\n"
"  return operationFailure(function(){return nativeKdf(norm.name==='HKDF'?1:0,hlen,slot.material.k.buffer,salt,info,iterations,bits/8)});\n"
"}\n"
"SubtleCrypto.prototype.deriveBits = function(algorithm,key,length) {\n"
"  return new Promise(function(resolve,reject){try{resolve(derive(algorithm,key,length,'deriveBits'))}catch(e){reject(e)}});\n"
"};\n"
"SubtleCrypto.prototype.deriveKey = function(algorithm,key,derivedAlgorithm,extractable,usages) {\n"
"  return new Promise(function(resolve,reject){try{\n"
"    var norm=normalizeAlg(derivedAlgorithm), bits;\n"
"    if(norm.name==='HMAC') bits=norm.raw.length===undefined?(norm.hash==='SHA-1'||norm.hash==='SHA-256'?512:1024):uint32(norm.raw.length,'length');\n"
"    else if(/^AES-/.test(norm.name)) {\n"
"      if(norm.raw.length===undefined)throw new TypeError('derived AES key length is required');\n"
"      bits=uint32(norm.raw.length,'length');\n"
"      if([128,192,256].indexOf(bits)<0)throw new G.DOMException('invalid AES key length','OperationError');\n"
"    } else throw new G.DOMException('derived key algorithm is not implemented','NotSupportedError');\n"
"    if(!bits || bits%8)throw new G.DOMException('derived key requires a nonzero byte length','OperationError');\n"
"    var raw=derive(algorithm,key,bits,'deriveKey');\n"
"    resolve(SubtleCrypto.prototype.importKey.call(this,'raw',raw,derivedAlgorithm,extractable,usages));\n"
"  }catch(e){reject(e)}});\n"
"};\n"
"function crypt(decrypt,algorithm,key,data) {\n"
"  return new Promise(function(resolve,reject){try{\n"
"    var norm=operationAlg(algorithm), slot=checkKeyFor(key,norm.name,decrypt?'decrypt':'encrypt'), dict=norm.raw;\n"
"    if(norm.name!=='AES-GCM')throw new G.DOMException('cipher is not implemented','NotSupportedError');\n"
"    if(dict.iv===undefined)throw new TypeError('AES-GCM iv is required');\n"
"    var iv=bufferCopy(dict.iv), aad=dict.additionalData===undefined?new ArrayBuffer(0):bufferCopy(dict.additionalData), bytes=bufferCopy(data);\n"
"    var tag=dict.tagLength===undefined?128:uint32(dict.tagLength,'tagLength');\n"
"    if([32,64,96,104,112,120,128].indexOf(tag)<0)throw new G.DOMException('invalid GCM tag length','OperationError');\n"
"    if(tag!==128)throw new G.DOMException('truncated GCM tags are not implemented','NotSupportedError');\n"
"    resolve(operationFailure(function(){return nativeGcm(decrypt?1:0,slot.material.k.buffer,iv,aad,bytes)}));\n"
"  }catch(e){reject(e)}});\n"
"}\n"
"SubtleCrypto.prototype.encrypt = function(algorithm,key,data){return crypt(false,algorithm,key,data)};\n"
"SubtleCrypto.prototype.decrypt = function(algorithm,key,data){return crypt(true,algorithm,key,data)};\n"
"SubtleCrypto.prototype.wrapKey = function (format, key, wrappingKey, wrapAlgorithm) {\n"
"  return new Promise(function (resolve, reject) {\n"
"    try {\n"
"      var norm = normalizeAlg(wrapAlgorithm);\n"
"      checkKeyFor(wrappingKey, norm.name, 'wrapKey');\n"
"      reject(new G.DOMException('wrapKey is not supported in this build', 'NotSupportedError'));\n"
"    } catch (e) { reject(e); }\n"
"  });\n"
"};\n"
"SubtleCrypto.prototype.unwrapKey = function (format, wrappedKey, unwrappingKey, unwrapAlgorithm) {\n"
"  return new Promise(function (resolve, reject) {\n"
"    try {\n"
"      var norm = normalizeAlg(unwrapAlgorithm);\n"
"      checkKeyFor(unwrappingKey, norm.name, 'unwrapKey');\n"
"      reject(new G.DOMException('unwrapKey is not supported in this build', 'NotSupportedError'));\n"
"    } catch (e) { reject(e); }\n"
"  });\n"
"};\n"

#include "js_crypto_keys_script.inc"
"c.subtle = Object.create(SubtleCrypto.prototype);\n"
"})\n";

void js_subtle_install(JSContext *ctx)
{
    if (!ctx) return;
    JSValue fn = JS_Eval(ctx, SUBTLE_PRELUDE, strlen(SUBTLE_PRELUDE), "<subtle>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[subtle] prelude failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, fn);
        return;
    }
    JSValue native[] = {
        JS_NewCFunction(ctx, subtle_digest_native, "digest", 2),
        JS_NewCFunction(ctx, subtle_hmac_native, "hmac", 3),
        JS_NewCFunction(ctx, subtle_kdf_native, "kdf", 7),
        JS_NewCFunction(ctx, subtle_gcm_native, "gcm", 5),
        JS_NewCFunction(ctx, subtle_random_native, "random", 1),
        JS_NewCFunction(ctx, subtle_keyop_native, "keyop", 5)
    };
    const int native_count=(int)(sizeof native/sizeof native[0]);
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, native_count, (JSValueConst *)native);
    for (int i=0;i<native_count;i++) JS_FreeValue(ctx, native[i]);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[subtle] install failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, fn);
}
