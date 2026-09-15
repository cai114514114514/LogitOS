/* The operational half of SubtleCrypto.digest. The primitives are the same
 * translation units linked by the OS, not a JS reimplementation. Weak imports
 * preserve intentionally narrow host fixtures; the shipping source list and
 * digest gate link all four algorithms. A missing backend throws, never hashes
 * with a different algorithm. Input is copied by the binding before invoking
 * native code, including a TypedArray/DataView's offset and exact length. */
#include "../../../include/weaksym.h"
#include <stdint.h>
extern void ocsp_sha1(const void *, size_t, uint8_t[20]) LOGIT_WEAK;
extern void sha256(const void *, size_t, uint8_t[32]) LOGIT_WEAK;
extern void sha384(const void *, size_t, uint8_t[48]) LOGIT_WEAK;
extern void sha512(const void *, size_t, uint8_t[64]) LOGIT_WEAK;
LOGIT_WEAK_STUB(ocsp_sha1);
LOGIT_WEAK_STUB(sha256);
LOGIT_WEAK_STUB(sha384);
LOGIT_WEAK_STUB(sha512);
static JSValue subtle_digest_native(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    if (argc != 2) return JS_ThrowTypeError(ctx, "digest expects algorithm and buffer");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    size_t size = 0; uint8_t *data = JS_GetArrayBuffer(ctx, &size, argv[1]);
    /* QuickJS allocates even a zero-length ArrayBuffer, and NULL is its error
     * signal (invalid object or detached buffer), not a valid empty buffer. */
    if (!data) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    uint8_t out[64]; size_t n = 0;
    if (!strcmp(name,"SHA-1") && LOGIT_HAVE(ocsp_sha1)) { ocsp_sha1(data,size,out); n=20; }
    else if (!strcmp(name,"SHA-256") && LOGIT_HAVE(sha256)) { sha256(data,size,out); n=32; }
    else if (!strcmp(name,"SHA-384") && LOGIT_HAVE(sha384)) { sha384(data,size,out); n=48; }
    else if (!strcmp(name,"SHA-512") && LOGIT_HAVE(sha512)) { sha512(data,size,out); n=64; }
    JS_FreeCString(ctx, name);
    if (!n) return JS_ThrowTypeError(ctx,"digest backend unavailable");
#ifdef JS_DIGEST_NEGCTL
    out[0] ^= 1;
#endif
    return JS_NewArrayBufferCopy(ctx, out, n);
}
