#include "ssh_pubkey.h"
#include "ssh_wire.h"
#include "crypto.h"

void ssh_pubkey_init(void)
{
    /* A zero signature fails before any point arithmetic but after the
     * engine initializes all three curves. Do this before worker creation,
     * since concurrent first-use would race the Barrett table population. */
    const uint8_t zero[64]={0};
    (void)ecdsa_verify(256,zero,zero,zero,32);
}

static int equal(const uint8_t *p,int n,const char *s)
{int i=0;while(s[i])i++;if(n!=i)return 0;for(i=0;i<n;i++)if(p[i]!=(uint8_t)s[i])return 0;return 1;}
static int name(const char *a,const char *b)
{int i=0;while(a[i]&&a[i]==b[i])i++;return a[i]==b[i];}
/* Positive canonical SSH mpint -> unsigned magnitude. The leading sign byte
 * belongs to SSH encoding, never to the fixed-width crypto representation. */
static int magnitude(const uint8_t *b,int o,int n,const uint8_t **p,int *len)
{
    o=ssh_r_string(b,o,n,p,len);if(o<0||!*len||((*p)[0]&128))return -1;
    if(!(*p)[0]){if(*len<2||!((*p)[1]&128))return -1;(*p)++;(*len)--;}
    return o;
}
struct public_key {int curve,width;const uint8_t *point,*n,*e;int nlen,elen;};
static int parse(const char *alg,const uint8_t *blob,int len,struct public_key *key)
{
    const uint8_t *type,*field;int tlen,flen;
    if(len<1||len>SSH_AUTH_KEY_MAX)return 0;
    int o=ssh_r_string(blob,0,len,&type,&tlen);if(o<0)return 0;
    key->curve=0;key->width=0;
    if(name(alg,"ssh-ed25519")){
        o=ssh_r_string(blob,o,len,&key->point,&flen);
        return o==len&&flen==32&&equal(type,tlen,alg);
    }
#ifdef SSH_AUTH_ED25519_ONLY
    return 0; /* Private feature-absence control, never a product build flag. */
#endif
    if(name(alg,"rsa-sha2-256")||name(alg,"rsa-sha2-512")){
        if(!equal(type,tlen,"ssh-rsa"))return 0;
        o=magnitude(blob,o,len,&key->e,&key->elen);o=magnitude(blob,o,len,&key->n,&key->nlen);
        /* Bound work to ordinary 2048..4096-bit keys and public exponents
         * up to 64 bits. No SHA-1 signature fallback is advertised. */
        if(o!=len||o<0||key->nlen<256||key->nlen>512||key->elen>8)return 0;
        if(key->nlen==256&&!(key->n[0]&128))return 0;
        if(!(key->n[key->nlen-1]&1)||!(key->e[key->elen-1]&1)||(key->elen==1&&key->e[0]<3))return 0;
        key->width=-1;return 1;
    }
    const char *curve;
    if(name(alg,"ecdsa-sha2-nistp256")){key->curve=256;key->width=32;curve="nistp256";}
    else if(name(alg,"ecdsa-sha2-nistp384")){key->curve=384;key->width=48;curve="nistp384";}
    else if(name(alg,"ecdsa-sha2-nistp521")){key->curve=521;key->width=66;curve="nistp521";}
    else return 0;
    if(!equal(type,tlen,alg))return 0;
    o=ssh_r_string(blob,o,len,&field,&flen);if(o<0||!equal(field,flen,curve))return 0;
    o=ssh_r_string(blob,o,len,&key->point,&flen);
    return o==len&&flen==1+2*key->width&&key->point[0]==4;
}
int ssh_pubkey_supported(const char *alg,const uint8_t *blob,int len)
{struct public_key key;return parse(alg,blob,len,&key);}
int ssh_pubkey_verify(const char *alg,const uint8_t *blob,int len,
                     const uint8_t *signature,int siglen,const uint8_t *data,int datalen)
{
    struct public_key key;
    if(!parse(alg,blob,len,&key)||datalen<0)return 0;
    const uint8_t *type,*sig;int tlen,n;
    int o=ssh_r_string(signature,0,siglen,&type,&tlen);o=ssh_r_string(signature,o,siglen,&sig,&n);
    if(o<0||o!=siglen||!equal(type,tlen,alg))return 0;
    if(!key.width)return n==64&&ed25519_verify(sig,data,(unsigned long)datalen,key.point);
    int hlen=key.curve==256||name(alg,"rsa-sha2-256")?32:key.curve==384?48:64;
    uint8_t hash[64];if(hlen==32)sha256(data,datalen,hash);else if(hlen==48)sha384(data,datalen,hash);else sha512(data,datalen,hash);
    if(key.width<0)return rsa_pkcs1_verify(key.n,key.nlen,key.e,key.elen,sig,n,hash,hlen);
    const uint8_t *r,*s;int rn,sn;
    o=magnitude(sig,0,n,&r,&rn);o=magnitude(sig,o,n,&s,&sn);
    if(o<0||o!=n||rn>key.width||sn>key.width)return 0;
    uint8_t fixed[132]={0};
    for(int i=0;i<rn;i++)fixed[key.width-rn+i]=r[i];
    for(int i=0;i<sn;i++)fixed[2*key.width-sn+i]=s[i];
    return ecdsa_verify(key.curve,key.point+1,fixed,hash,hlen);
}
int ssh_pubkey_ext_info(uint8_t *out,int max)
{
    int o=ssh_w_u8(out,0,max,7); /* SSH_MSG_EXT_INFO, RFC 8308 */
    o=ssh_w_u32(out,o,max,1);o=ssh_w_cstring(out,o,max,"server-sig-algs");
    return ssh_w_cstring(out,o,max,SSH_AUTH_ALGORITHMS);
}
