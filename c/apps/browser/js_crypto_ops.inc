/* Browser operations reuse the OS HMAC and AES primitives. The kernel KDF
 * helpers have stack-buffer limits (PBKDF2 salt <=256; HKDF info <=256) and
 * silently return on overflow. Web inputs are not TLS-sized: dynamically size
 * the RFC 8018 / RFC 5869 concatenations here, and fail before allocating or
 * computing outside the limits below. No second hash implementation lives here.
 *
 * Current limits are deliberate: 16 MiB derived output, 4M HMAC invocations per
 * PBKDF2 call (the primitive still runs synchronously), GCM IV 1..1024 and a
 * 128-bit tag. Truncated-tag GCM is refused because the existing authenticated
 * open verifies all 16 bytes; zero-padding a short tag would be wrong. */
#include <limits.h>
extern void hmac(int, const uint8_t *, int, const uint8_t *, int, uint8_t *) LOGIT_WEAK;
LOGIT_WEAK_STUB(hmac);
#define SUBTLE_GCM_DECL(bits) \
extern void aes##bits##_gcm_seal_iv(const uint8_t *, const uint8_t *, int, const uint8_t *, int, const uint8_t *, int, uint8_t *, uint8_t *) LOGIT_WEAK; \
extern int aes##bits##_gcm_open_iv(const uint8_t *, const uint8_t *, int, const uint8_t *, int, const uint8_t *, int, const uint8_t *, uint8_t *) LOGIT_WEAK; \
LOGIT_WEAK_STUB(aes##bits##_gcm_seal_iv); \
LOGIT_WEAK_STUB(aes##bits##_gcm_open_iv)
SUBTLE_GCM_DECL(128);
SUBTLE_GCM_DECL(192);
SUBTLE_GCM_DECL(256);
static void subtle_wipe(void *p, size_t n)
{ volatile uint8_t *v=p; while(n--) *v++=0; }
static uint8_t *subtle_buffer(JSContext *ctx, JSValueConst value, size_t *n)
{
    uint8_t *p=JS_GetArrayBuffer(ctx,n,value);
    if(p && *n>INT_MAX-128) { JS_ThrowRangeError(ctx,"crypto input exceeds native length range"); return NULL; }
    return p;
}
static JSValue subtle_hmac_native(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; int hlen; size_t kn,mn,sn=0;
    if(argc<3 || JS_ToInt32(ctx,&hlen,argv[0])) return JS_EXCEPTION;
    if(!LOGIT_HAVE(hmac) || (hlen!=32 && hlen!=48 && hlen!=64)) return JS_ThrowTypeError(ctx,"HMAC backend unavailable");
    uint8_t *key=subtle_buffer(ctx,argv[1],&kn); if(!key)return JS_EXCEPTION;
    uint8_t *msg=subtle_buffer(ctx,argv[2],&mn); if(!msg)return JS_EXCEPTION;
    uint8_t *sig=NULL;
    if(argc>3) { sig=subtle_buffer(ctx,argv[3],&sn); if(!sig)return JS_EXCEPTION; }
    uint8_t out[64]; hmac(hlen,key,(int)kn,msg,(int)mn,out);
#ifdef JS_CRYPTO_OPS_NEGCTL
    out[0]^=1;
#endif
    JSValue result;
    if(sig) {
        /* Signature length is public; equal-length comparison visits every
         * byte, unlike JS Array.every() or memcmp()'s early-out. */
        unsigned diff=(sn!=(size_t)hlen);
        if(sn==(size_t)hlen) for(int i=0;i<hlen;i++)diff|=sig[i]^out[i];
        result=JS_NewBool(ctx,!diff);
    } else result=JS_NewArrayBufferCopy(ctx,out,(size_t)hlen);
    subtle_wipe(out,sizeof out); return result;
}
static JSValue subtle_kdf_native(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; int kind,hlen; uint32_t iterations,n;
    if(argc!=7 || JS_ToInt32(ctx,&kind,argv[0]) || JS_ToInt32(ctx,&hlen,argv[1]) ||
       JS_ToUint32(ctx,&iterations,argv[5]) || JS_ToUint32(ctx,&n,argv[6]))return JS_EXCEPTION;
    if(!LOGIT_HAVE(hmac) || (hlen!=32 && hlen!=48 && hlen!=64))return JS_ThrowTypeError(ctx,"KDF backend unavailable");
    size_t kn,sn,in;
    uint8_t *key=subtle_buffer(ctx,argv[2],&kn);if(!key)return JS_EXCEPTION;
    uint8_t *salt=subtle_buffer(ctx,argv[3],&sn);if(!salt)return JS_EXCEPTION;
    uint8_t *info=subtle_buffer(ctx,argv[4],&in);if(!info)return JS_EXCEPTION;
    if(n>16*1024*1024 || (kind==0 && (!iterations || (uint64_t)iterations*((n+hlen-1)/hlen)>4000000)) ||
       (kind==1 && n>255u*(unsigned)hlen) || (kind!=0 && kind!=1))return JS_ThrowRangeError(ctx,"KDF resource or output limit exceeded");
    uint8_t *out=js_malloc(ctx,n?n:1);if(!out)return JS_EXCEPTION;
    size_t scratch_len=kind==0?sn+4:in+(size_t)hlen+1;
    uint8_t *scratch=js_malloc(ctx,scratch_len);if(!scratch){js_free(ctx,out);return JS_EXCEPTION;}
    uint8_t u[64]={0},t[64]={0},prk[64]={0}; uint32_t done=0,block=1;
    if(kind==1)hmac(hlen,salt,(int)sn,key,(int)kn,prk);
    while(done<n) {
        if(kind==0) {
            memcpy(scratch,salt,sn);
            scratch[sn]=(uint8_t)(block>>24);scratch[sn+1]=(uint8_t)(block>>16);
            scratch[sn+2]=(uint8_t)(block>>8);scratch[sn+3]=(uint8_t)block;
            hmac(hlen,key,(int)kn,scratch,(int)sn+4,u);memcpy(t,u,(size_t)hlen);
            for(uint32_t i=1;i<iterations;i++) { hmac(hlen,key,(int)kn,u,hlen,u);for(int j=0;j<hlen;j++)t[j]^=u[j]; }
        } else {
            size_t prior=block==1?0:(size_t)hlen;
            memcpy(scratch,t,prior);memcpy(scratch+prior,info,in);scratch[prior+in]=(uint8_t)block;
            hmac(hlen,prk,hlen,scratch,(int)(prior+in+1),t);
        }
        uint32_t take=n-done;if(take>(uint32_t)hlen)take=(uint32_t)hlen;
        memcpy(out+done,t,take);done+=take;block++;
    }
#ifdef JS_CRYPTO_KDF_NEGCTL
    if(n)out[0]^=1;
#endif
    JSValue result=JS_NewArrayBufferCopy(ctx,out,n);
    subtle_wipe(u,sizeof u);subtle_wipe(t,sizeof t);subtle_wipe(prk,sizeof prk);
    subtle_wipe(scratch,scratch_len);js_free(ctx,scratch);subtle_wipe(out,n);js_free(ctx,out);
    return result;
}
static JSValue subtle_gcm_native(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;int decrypt;size_t kn,ivn,an,dn;
    if(argc!=5 || JS_ToInt32(ctx,&decrypt,argv[0]))return JS_EXCEPTION;
    uint8_t *key=subtle_buffer(ctx,argv[1],&kn);if(!key)return JS_EXCEPTION;
    uint8_t *iv=subtle_buffer(ctx,argv[2],&ivn);if(!iv)return JS_EXCEPTION;
    uint8_t *aad=subtle_buffer(ctx,argv[3],&an);if(!aad)return JS_EXCEPTION;
    uint8_t *data=subtle_buffer(ctx,argv[4],&dn);if(!data)return JS_EXCEPTION;
    if((kn!=16 && kn!=24 && kn!=32) || ivn<1 || ivn>1024 || (decrypt && dn<16))return JS_ThrowRangeError(ctx,"invalid GCM sizes");
    size_t n=decrypt?dn-16:dn;
    uint8_t *out=js_malloc(ctx,n+16);if(!out)return JS_EXCEPTION;
    int result=-2;
#define SUBTLE_GCM_RUN(bits, bytes) \
    if(kn==bytes && LOGIT_HAVE(aes##bits##_gcm_seal_iv) && LOGIT_HAVE(aes##bits##_gcm_open_iv)) { \
        if(decrypt)result=aes##bits##_gcm_open_iv(key,iv,(int)ivn,aad,(int)an,data,(int)n,data+n,out); \
        else {aes##bits##_gcm_seal_iv(key,iv,(int)ivn,aad,(int)an,data,(int)n,out,out+n);result=0;} \
    }
    SUBTLE_GCM_RUN(128,16);SUBTLE_GCM_RUN(192,24);SUBTLE_GCM_RUN(256,32);
#undef SUBTLE_GCM_RUN
#ifdef JS_CRYPTO_GCM_NEGCTL
    if(!decrypt && result==0)out[0]^=1;
#endif
    JSValue value;
    if(result)value=JS_ThrowTypeError(ctx,result==-2?"AES-GCM backend unavailable":"AES-GCM authentication failed");
    else value=JS_NewArrayBufferCopy(ctx,out,decrypt?n:n+16);
    subtle_wipe(out,n+16);js_free(ctx,out);return value;
}
