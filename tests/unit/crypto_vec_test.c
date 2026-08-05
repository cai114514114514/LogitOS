#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "crypto.h"
#include "crypto_vectors.h"

static int fails, passes;
static void ok(const char *name, int cond)
{ if (cond) { passes++; printf("ok   %s\n", name); } else { fails++; printf("FAIL %s\n", name); } }
static int eq(const uint8_t *a, const uint8_t *b, int n) { return memcmp(a, b, n) == 0; }
static int hv(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; return c-'A'+10; }
static int unhex(const char *h, uint8_t *o){ int n=0; while(h[2*n]){ o[n]=(uint8_t)(hv(h[2*n])<<4|hv(h[2*n+1])); n++; } return n; }

static void test_sha(void)
{
    uint8_t o[64], w[64];
    sha256("", 0, o); unhex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", w);
    ok("sha256 empty", eq(o,w,32));
    sha256("abc", 3, o); unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", w);
    ok("sha256 abc", eq(o,w,32));
    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, o);
    unhex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", w);
    ok("sha256 56B", eq(o,w,32));
    /* 1,000,000 x 'a' fed in odd-size chunks: exercises buffering + len count */
    struct sha256 c; sha256_init(&c);
    uint8_t chunk[7777]; memset(chunk, 'a', sizeof chunk);
    int left = 1000000;
    while (left > 0) { int t = left > 7777 ? 7777 : left; sha256_update(&c, chunk, t); left -= t; }
    sha256_final(&c, o);
    unhex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", w);
    ok("sha256 1M a chunked", eq(o,w,32));

    sha384("", 0, o); unhex("38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b", w);
    ok("sha384 empty", eq(o,w,48));
    sha384("abc", 3, o); unhex("cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7", w);
    ok("sha384 abc", eq(o,w,48));
    sha384("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", 112, o);
    unhex("09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa91746039", w);
    ok("sha384 112B", eq(o,w,48));

    sha512("", 0, o); unhex("cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e", w);
    ok("sha512 empty", eq(o,w,64));
    sha512("abc", 3, o); unhex("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", w);
    ok("sha512 abc", eq(o,w,64));
}

static void test_hmac_hkdf(void)
{
    uint8_t o[128], w[128], key[256], msg[256];
    /* RFC 4231 TC1 */
    memset(key, 0x0b, 20);
    hmac(32, key, 20, (const uint8_t *)"Hi There", 8, o);
    unhex("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", w);
    ok("hmac256 rfc4231 tc1", eq(o,w,32));
    /* TC2 "Jefe" */
    hmac(32, (const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, o);
    unhex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", w);
    ok("hmac256 rfc4231 tc2", eq(o,w,32));
    /* TC6: key 131x 0xaa (> blocksize) */
    memset(key, 0xaa, 131);
    hmac(32, key, 131, (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First", 54, o);
    unhex("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", w);
    ok("hmac256 rfc4231 tc6 longkey", eq(o,w,32));
    /* TC7 via python reference */
    hmac(32, hmac256_key, 131, hmac256_tc7_msg, (int)sizeof(hmac256_tc7_msg), o);
    ok("hmac256 tc7 (python ref)", eq(o, hmac256_tc7_out, 32));
    /* HMAC-SHA384, key > 128 blocksize (python ref) */
    hmac(48, hmac384_key, 131, hmac384_msg, (int)sizeof(hmac384_msg), o);
    ok("hmac384 longkey (python ref)", eq(o, hmac384_out, 48));

    /* RFC 5869 Case 1 (SHA-256) */
    memset(key, 0x0b, 22);
    unhex("000102030405060708090a0b0c", msg);
    uint8_t prk[48];
    hkdf_extract(32, msg, 13, key, 22, prk);
    unhex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", w);
    ok("hkdf256 extract rfc5869 case1", eq(prk,w,32));
    uint8_t info[16]; int il = unhex("f0f1f2f3f4f5f6f7f8f9", info);
    hkdf_expand(32, prk, info, il, o, 42);
    unhex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", w);
    ok("hkdf256 expand rfc5869 case1", eq(o,w,42));
    /* HKDF-SHA384 multi-block (L=100 > 2*48) via python ref */
    hkdf_extract(48, hk384_salt, (int)sizeof(hk384_salt), hk384_ikm, (int)sizeof(hk384_ikm), prk);
    ok("hkdf384 extract (python ref)", eq(prk, hk384_prk, 48));
    hkdf_expand(48, prk, hk384_info, (int)sizeof(hk384_info), o, 100);
    ok("hkdf384 expand L=100 (python ref)", eq(o, hk384_okm, 100));
    /* extract with NULL salt -> HashLen zeros (TLS 1.3 early secret pattern) */
    hkdf_extract(32, 0, 0, key, 22, prk);
    uint8_t zeros[32]; memset(zeros, 0, 32);
    hmac(32, zeros, 32, key, 22, w);
    ok("hkdf256 extract null salt = zeros", eq(prk, w, 32));

    /* HKDF-Expand-Label (RFC 8446 HkdfLabel) via python ref */
    hkdf_expand_label(32, el256_secret, "c hs traffic", el256_ctx, (int)sizeof(el256_ctx), o, 32);
    ok("expand_label 256 c hs traffic", eq(o, el256_out, 32));
    hkdf_expand_label(48, el384_secret, "s ap traffic", el384_ctx, (int)sizeof(el384_ctx), o, 48);
    ok("expand_label 384 s ap traffic", eq(o, el384_out, 48));
    hkdf_expand_label(48, el384_secret, "key", 0, 0, o, 32);
    ok("expand_label 384 key empty ctx", eq(o, el384_keyout, 32));
    hkdf_expand_label(32, el256_secret, "iv", 0, 0, o, 12);
    ok("expand_label 256 iv empty ctx", eq(o, el256_ivout, 12));
}

static void test_aesgcm(void)
{
    uint8_t key[16], iv[12], pt[80], aad[32], ct[80], tag[16], o[80], w[80];
    int n;
    /* NIST: key=0, iv=0, empty pt/aad */
    memset(key,0,16); memset(iv,0,12);
    aes128_gcm_seal(key, iv, 0, 0, 0, 0, ct, tag);
    unhex("58e2fccefa7e3061367f1d57a4e7455a", w);
    ok("gcm zero-key empty tag", eq(tag,w,16));
    /* key=0 iv=0 pt=0^16 */
    memset(pt,0,16);
    aes128_gcm_seal(key, iv, 0, 0, pt, 16, ct, tag);
    unhex("0388dace60b6a392f328c2b971b2fe78", w);
    ok("gcm zero-key 16B ct", eq(ct,w,16));
    unhex("ab6e47d42cec13bdf53a67b21257bddf", w);
    ok("gcm zero-key 16B tag", eq(tag,w,16));
    /* McGrew-Viega TC3: 60-byte pt, 20-byte aad */
    unhex("feffe9928665731c6d6a8f9467308308", key);
    unhex("cafebabefacedbaddecaf888", iv);
    n = unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", pt);
    int al = unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad);
    aes128_gcm_seal(key, iv, aad, al, pt, n, ct, tag);
    unhex("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091", w);
    ok("gcm tc3 ct", eq(ct,w,n));
    unhex("5bc94fbc3221a5db94fae95ae7121a47", w);
    ok("gcm tc3 tag", eq(tag,w,16));
    /* open must roundtrip */
    ok("gcm tc3 open", aes128_gcm_open(key, iv, aad, al, ct, n, tag, o) == 0 && eq(o, pt, n));
    /* tampered tag and ct must be rejected */
    tag[0] ^= 1;
    ok("gcm bad tag rejected", aes128_gcm_open(key, iv, aad, al, ct, n, tag, o) != 0);
    tag[0] ^= 1; ct[3] ^= 1;
    ok("gcm bad ct rejected", aes128_gcm_open(key, iv, aad, al, ct, n, tag, o) != 0);
    /* TC4: 64-byte pt */
    n = unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255", pt);
    aes128_gcm_seal(key, iv, aad, al, pt, n, ct, tag);
    unhex("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985", w);
    ok("gcm tc4 ct", eq(ct,w,n));
    unhex("da80ce830cfda02da2a218a1744f4c76", w);
    ok("gcm tc4 tag", eq(tag,w,16));
}

static void test_chacha(void)
{
    uint8_t key[32], nonce[12], aad[16], pt[320], ct[320], tag[16], o[320], w[320];
    /* RFC 8439 A.5 */
    unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key);
    unhex("070000004041424344454647", nonce);
    int al = unhex("50515253c0c1c2c3c4c5c6c7", aad);
    int n = 0;
    const char *m = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    int ml = (int)strlen(m); memcpy(pt, m, ml); n = ml;
    chacha20_poly1305_seal(key, nonce, aad, al, pt, n, ct, tag);
    unhex("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116", w);
    ok("chacha a5 ct", eq(ct,w,n));
    unhex("1ae10b594f09e26a7e902ecbd0600691", w);
    ok("chacha a5 tag", eq(tag,w,16));
    ok("chacha a5 open", chacha20_poly1305_open(key, nonce, aad, al, ct, n, tag, o) == 0 && eq(o, pt, n));
    /* RFC 8439 2.8.2 (aad/ct not 16-aligned) */
    unhex("1c9240a5eb55d38af333888604f6b5f0473917c1402b80099dca5cbc207075c0", key);
    unhex("000000000102030405060708", nonce);
    al = unhex("f33388860000000000004e91", aad);
    n = unhex("496e7465726e65742d4472616674732061726520647261667420646f63756d656e74732076616c696420666f722061206d6178696d756d206f6620736978206d6f6e74687320616e64206d617920626520757064617465642c207265706c616365642c206f72206f62736f6c65746564206279206f7468657220646f63756d656e747320617420616e792074696d652e20497420697320696e617070726f70726961746520746f2075736520496e7465726e65742d447261667473206173207265666572656e6365206d6174657269616c206f7220746f2063697465207468656d206f74686572207468616e206173202fe2809c776f726b20696e2070726f67726573732e2fe2809d", pt);
    chacha20_poly1305_seal(key, nonce, aad, al, pt, n, ct, tag);
    unhex("64a0861575861af460f062c79be643bd5e805cfd345cf389f108670ac76c8cb24c6cfc18755d43eea09ee94e382d26b0bdb7b73c321b0100d4f03b7f355894cf332f830e710b97ce98c8a84abd0b948114ad176e008d33bd60f982b1ff37c8559797a06ef4f0ef61c186324e2b3506383606907b6a7c02b0f9f6157b53c867e4b9166c767b804d46a59b5216cde7a4e99040c5a40433225ee282a1b0a06c523eaf4534d7f83fa1155b0047718cbc546a0d072b04b3564eea1b422273f548271a0bb2316053fa76991955ebd63159434ecebb4e466dae5a1073a6727627097a1049e617d91d361094fa68f0ff77987130305beaba2eda04df997b714d6c6f2c29a6ad5cb4022b02709b", w);
    ok("chacha 2.8.2 ct", eq(ct,w,n));
    unhex("eead9d67890cbb22392336fea1851f38", w);
    ok("chacha 2.8.2 tag", eq(tag,w,16));
    ok("chacha 2.8.2 open", chacha20_poly1305_open(key, nonce, aad, al, ct, n, tag, o) == 0 && eq(o, pt, n));
    tag[15] ^= 0x80;
    ok("chacha bad tag rejected", chacha20_poly1305_open(key, nonce, aad, al, ct, n, tag, o) != 0);
}

static void test_x25519(void)
{
    uint8_t s[32], u[32], o[32], w[32];
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", s);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u);
    x25519(o, s, u);
    unhex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", w);
    ok("x25519 rfc7748 #1", eq(o,w,32));
    unhex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", s);
    unhex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", u);
    x25519(o, s, u);
    unhex("95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957", w);
    ok("x25519 rfc7748 #2", eq(o,w,32));
    /* base point vector */
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", s);
    x25519_base(o, s);
    unhex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", w);
    ok("x25519 base", eq(o,w,32));
    /* RFC 7748 iterated: k=u=9, 1 and 1000 iterations */
    uint8_t k[32], t[32];
    memset(k,0,32); memset(u,0,32); k[0]=9; u[0]=9;
    x25519(t, k, u); memcpy(u,k,32); memcpy(k,t,32);
    unhex("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079", w);
    ok("x25519 iter 1", eq(k,w,32));
    for (int i = 1; i < 1000; i++) { x25519(t, k, u); memcpy(u,k,32); memcpy(k,t,32); }
    unhex("684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51", w);
    ok("x25519 iter 1000", eq(k,w,32));
    /* low-order points yield all-zero shared secret (RFC 7748 sec 6) */
    memset(u, 0, 32);
    x25519(o, s, u);
    int allzero = 1; for (int i = 0; i < 32; i++) if (o[i]) allzero = 0;
    ok("x25519 u=0 -> zero (caller must check)", allzero);
    memset(u, 0, 32); u[0] = 1;
    x25519(o, s, u);
    allzero = 1; for (int i = 0; i < 32; i++) if (o[i]) allzero = 0;
    ok("x25519 u=1 -> zero (caller must check)", allzero);
    /* non-canonical u = p (should equal u=0 behavior: folds to 0) */
    uint8_t uu[32]; memset(uu,0xff,32);
    x25519(o, s, uu);          /* u = 2^255-1, must not crash */
    ok("x25519 u=2^255-1 no crash", 1);
}

static void ec_case(int cvid, int hlen, const uint8_t *pub, const uint8_t *sig, const uint8_t *hash, const char *nm)
{
    char b[96];
    ok(nm, ecdsa_verify(cvid, pub, sig, hash, hlen) == 1);
    int fl = cvid/8;
    uint8_t bad[96];
    memcpy(bad, sig, 2*fl); bad[0] ^= 1;
    snprintf(b, sizeof b, "%s reject corrupt r", nm); ok(b, ecdsa_verify(cvid, pub, bad, hash, hlen) == 0);
    memcpy(bad, sig, 2*fl); bad[2*fl-1] ^= 1;
    snprintf(b, sizeof b, "%s reject corrupt s", nm); ok(b, ecdsa_verify(cvid, pub, bad, hash, hlen) == 0);
    memcpy(bad, sig, 2*fl); memset(bad, 0, fl);
    snprintf(b, sizeof b, "%s reject r=0", nm); ok(b, ecdsa_verify(cvid, pub, bad, hash, hlen) == 0);
    memcpy(bad, sig, 2*fl); memset(bad+fl, 0, fl);
    snprintf(b, sizeof b, "%s reject s=0", nm); ok(b, ecdsa_verify(cvid, pub, bad, hash, hlen) == 0);
    uint8_t wrongh[64]; memcpy(wrongh, hash, hlen); wrongh[0] ^= 1;
    snprintf(b, sizeof b, "%s reject wrong hash", nm); ok(b, ecdsa_verify(cvid, pub, sig, wrongh, hlen) == 0);
}

static void test_ecdsa(void)
{
    ec_case(256, 32, ec256_256_pub, ec256_256_sig, ec256_256_hash, "ecdsa p256 sha256");
    ec_case(256, 48, ec256_384_pub, ec256_384_sig, ec256_384_hash, "ecdsa p256 sha384");
    ec_case(384, 48, ec384_384_pub, ec384_384_sig, ec384_384_hash, "ecdsa p384 sha384");
    ec_case(384, 64, ec384_512_pub, ec384_512_sig, ec384_512_hash, "ecdsa p384 sha512");
    /* r >= n must be rejected */
    uint8_t sig[64]; memcpy(sig, p256_n, 32); memcpy(sig+32, ec256_256_sig+32, 32);
    ok("ecdsa reject r=n", ecdsa_verify(256, ec256_256_pub, sig, ec256_256_hash, 32) == 0);
    /* qx >= p must be rejected */
    uint8_t pub[64]; memcpy(pub, p256_p, 32); memcpy(pub+32, ec256_256_pub+32, 32);
    ok("ecdsa reject qx=p", ecdsa_verify(256, pub, ec256_256_sig, ec256_256_hash, 32) == 0);
    /* off-curve public key with otherwise-valid sig must not validate */
    ok("ecdsa offcurve pub rejects sig", ecdsa_verify(256, ec256_offcurve_pub, ec256_256_sig, ec256_256_hash, 32) == 0);
}

static void test_rsa(void)
{
    const int nl = 256, el = 3;
    ok("rsa pkcs1 sha256 valid", rsa_pkcs1_verify(rsa_n, nl, rsa_e, el, rsa_sig256, nl, rsa_h256, 32) == 1);
    ok("rsa pkcs1 sha384 valid", rsa_pkcs1_verify(rsa_n, nl, rsa_e, el, rsa_sig384, nl, rsa_h384, 48) == 1);
    ok("rsa pkcs1 sha512 valid", rsa_pkcs1_verify(rsa_n, nl, rsa_e, el, rsa_sig512, nl, rsa_h512, 64) == 1);
    uint8_t bad[256];
    memcpy(bad, rsa_sig256, nl); bad[100] ^= 1;
    ok("rsa pkcs1 reject corrupt sig", rsa_pkcs1_verify(rsa_n, nl, rsa_e, el, bad, nl, rsa_h256, 32) == 0);
    uint8_t wrongh[64]; memcpy(wrongh, rsa_h256, 32); wrongh[0] ^= 1;
    ok("rsa pkcs1 reject wrong hash", rsa_pkcs1_verify(rsa_n, nl, rsa_e, el, rsa_sig256, nl, wrongh, 32) == 0);
    ok("rsa pkcs1 reject short PS (7xFF)", rsa_pkcs1_verify(rsa_n, nl, rsa_e, el, rsa_sig_shortps, nl, rsa_h256, 32) == 0);
    ok("rsa pkcs1 reject trailing garbage", rsa_pkcs1_verify(rsa_n, nl, rsa_e, el, rsa_sig_garbage, nl, rsa_h256, 32) == 0);
    /* wrong DigestInfo: sha256 sig presented as sha384 must fail prefix match */
    ok("rsa pkcs1 reject alg confusion", rsa_pkcs1_verify(rsa_n, nl, rsa_e, el, rsa_sig256, nl, rsa_h384, 48) == 0);
    /* sig >= n rejected */
    ok("rsa pkcs1 reject sig=n", rsa_pkcs1_verify(rsa_n, nl, rsa_e, el, rsa_n, nl, rsa_h256, 32) == 0);

    ok("rsa pss sha256 valid", rsa_pss_verify(rsa_n, nl, rsa_e, el, rsa_pss256, nl, rsa_h256, 32) == 1);
    ok("rsa pss sha384 valid", rsa_pss_verify(rsa_n, nl, rsa_e, el, rsa_pss384, nl, rsa_h384, 48) == 1);
    ok("rsa pss sha512 valid", rsa_pss_verify(rsa_n, nl, rsa_e, el, rsa_pss512, nl, rsa_h512, 64) == 1);
    memcpy(bad, rsa_pss256, nl); bad[42] ^= 1;
    ok("rsa pss reject corrupt sig", rsa_pss_verify(rsa_n, nl, rsa_e, el, bad, nl, rsa_h256, 32) == 0);
    ok("rsa pss reject wrong hash", rsa_pss_verify(rsa_n, nl, rsa_e, el, rsa_pss256, nl, wrongh, 32) == 0);
    ok("rsa pss reject bad trailer", rsa_pss_verify(rsa_n, nl, rsa_e, el, rsa_pss_badtrailer, nl, rsa_h256, 32) == 0);
}

int main(void)
{
    test_sha();
    test_hmac_hkdf();
    test_aesgcm();
    test_chacha();
    test_x25519();
    test_ecdsa();
    test_rsa();
    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
