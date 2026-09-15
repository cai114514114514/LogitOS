/* SPDX-License-Identifier: MIT */
/* Native key operations for the algorithms the OS already implements. The
 * JS layer owns CryptoKey usages/extractability; this layer checks lengths,
 * derives a private key's public half itself, validates imported EC points,
 * and never substitutes another algorithm or weaker source of randomness. */
#include "web_entropy.h"
extern void ed25519_pubkey(uint8_t *, const uint8_t *) LOGIT_WEAK;
extern void ed25519_sign(uint8_t *, const uint8_t *, size_t, const uint8_t *, const uint8_t *) LOGIT_WEAK;
extern int ed25519_verify(const uint8_t *, const uint8_t *, size_t, const uint8_t *) LOGIT_WEAK;
extern void x25519_base(uint8_t *, const uint8_t *) LOGIT_WEAK;
extern void x25519(uint8_t *, const uint8_t *, const uint8_t *) LOGIT_WEAK;
extern int ecdh_keygen(int, const uint8_t *, uint32_t, uint8_t *) LOGIT_WEAK;
extern int ecdh_shared(int, const uint8_t *, uint32_t, const uint8_t *, int, uint8_t *) LOGIT_WEAK;
extern int ecdsa_verify(int, const uint8_t *, const uint8_t *, const uint8_t *, int) LOGIT_WEAK;
extern int ecdsa_sign(int, const uint8_t *, const uint8_t *, int, uint32_t,
    void (*)(int,const uint8_t *,int,const uint8_t *,int,uint8_t *), uint8_t *) LOGIT_WEAK;
LOGIT_WEAK_STUB(ed25519_pubkey); LOGIT_WEAK_STUB(ed25519_sign); LOGIT_WEAK_STUB(ed25519_verify);
LOGIT_WEAK_STUB(x25519_base); LOGIT_WEAK_STUB(x25519);
LOGIT_WEAK_STUB(ecdh_keygen); LOGIT_WEAK_STUB(ecdh_shared);
LOGIT_WEAK_STUB(ecdsa_sign); LOGIT_WEAK_STUB(ecdsa_verify);

static JSValue subtle_random_native(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; int n;
    if (argc != 1 || JS_ToInt32(ctx,&n,argv[0])) return JS_EXCEPTION;
    if (n < 1 || n > 65536) return JS_ThrowRangeError(ctx,"key length exceeds entropy limit");
    uint8_t *out=js_malloc(ctx,(size_t)n); if(!out)return JS_EXCEPTION;
    int rc=web_entropy(out,n);
    JSValue v=rc ? JS_ThrowTypeError(ctx,"strong entropy unavailable") : JS_NewArrayBufferCopy(ctx,out,(size_t)n);
    subtle_wipe(out,(size_t)n);js_free(ctx,out);return v;
}
static int subtle_curve_len(int curve)
{ return curve==-1||curve==-2||curve==256?32:curve==384?48:curve==521?66:0; }
static int subtle_public(int curve, const uint8_t *priv, uint8_t *pub)
{
    if(curve==-1 && LOGIT_HAVE(ed25519_pubkey)){ed25519_pubkey(pub,priv);return 0;}
    if(curve==-2 && LOGIT_HAVE(x25519_base)){x25519_base(pub,priv);return 0;}
    uint32_t blind;
    if(curve>0 && LOGIT_HAVE(ecdh_keygen) && !web_entropy(&blind,sizeof blind))
        return ecdh_keygen(curve,priv,blind,pub);
    return -1;
}
/* op: 0 public-from-private, 1 sign, 2 verify, 3 derive, 4 validate public.
 * Ed25519 signs messages; ECDSA receives the selected hash from nativeDigest. */
static JSValue subtle_keyop_native(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; int op,curve;
    if(argc<3 || JS_ToInt32(ctx,&op,argv[0]) || JS_ToInt32(ctx,&curve,argv[1]))return JS_EXCEPTION;
    int n=subtle_curve_len(curve), plen=curve>0?1+2*n:n;
    if(!n)return JS_ThrowTypeError(ctx,"unsupported curve");
    size_t an,bn=0,cn=0;
    uint8_t *a=subtle_buffer(ctx,argv[2],&an);if(!a)return JS_EXCEPTION;
    uint8_t *b=NULL,*c=NULL;
    if(argc>3){b=subtle_buffer(ctx,argv[3],&bn);if(!b)return JS_EXCEPTION;}
    if(argc>4){c=subtle_buffer(ctx,argv[4],&cn);if(!c)return JS_EXCEPTION;}
    uint8_t out[133]={0},pub[133]={0};int rc=-1,outlen=0,boolean=-1;
    if(op==0 && an==(size_t)n){rc=subtle_public(curve,a,out);outlen=plen;}
    else if(op==1 && an==(size_t)n && b){
        if(curve==-1 && LOGIT_HAVE(ed25519_sign) && !subtle_public(curve,a,pub)){
            ed25519_sign(out,b,bn,a,pub);rc=0;outlen=64;
        } else if(curve>0 && LOGIT_HAVE(ecdsa_sign) && LOGIT_HAVE(hmac) && (bn==32||bn==48||bn==64)){
            uint32_t blind;if(!web_entropy(&blind,sizeof blind)){rc=ecdsa_sign(curve,a,b,(int)bn,blind,hmac,out);outlen=2*n;}
        }
    } else if(op==2 && an==(size_t)plen && b && c){
        if(curve==-1 && LOGIT_HAVE(ed25519_verify)){boolean=cn==64?ed25519_verify(c,b,bn,a):0;rc=0;}
        else if(curve>0 && LOGIT_HAVE(ecdsa_verify) && (bn==32||bn==48||bn==64)){
            boolean=cn==(size_t)(2*n)&&a[0]==4?ecdsa_verify(curve,a+1,c,b,(int)bn):0;rc=0;
        }
    } else if(op==3 && an==(size_t)n && b && bn==(size_t)plen){
        if(curve==-2 && LOGIT_HAVE(x25519)){
            x25519(out,a,b);unsigned nonzero=0;for(int i=0;i<n;i++)nonzero|=out[i];rc=nonzero?0:-1;outlen=n;
        } else if(curve>0 && LOGIT_HAVE(ecdh_shared)){
            uint32_t blind;if(!web_entropy(&blind,sizeof blind)){rc=ecdh_shared(curve,a,blind,b,(int)bn,out);outlen=n;}
        }
    } else if(op==4 && an==(size_t)plen){
        if(curve<0){rc=0;boolean=1;} /* RFC 7748 accepts every encoded coordinate. */
        else if(LOGIT_HAVE(ecdh_shared)){
            uint8_t one[66]={0};one[n-1]=1;
            rc=ecdh_shared(curve,one,0,a,plen,out);boolean=rc==0;
        }
    }
#ifdef JS_CRYPTO_KEYS_NEGCTL
    if(rc==0 && outlen>0)out[0]^=1;
#endif
    JSValue v=rc?JS_ThrowTypeError(ctx,"key operation failed"):
        boolean>=0?JS_NewBool(ctx,boolean):JS_NewArrayBufferCopy(ctx,out,(size_t)outlen);
    subtle_wipe(out,sizeof out);subtle_wipe(pub,sizeof pub);return v;
}
