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

/* AES-256-GCM against the McGrew-Viega GCM specification test cases 13-16 (the
 * same vectors NIST's CAVP GCM set is built from). These are the check on the
 * AES-256 key schedule specifically: the extra SubWord every fourth word is
 * invisible to a round-trip test and only shows up against a foreign
 * implementation's ciphertext. */
static void test_aes256gcm(void)
{
    uint8_t key[32], iv[12], pt[80], aad[32], ct[80], tag[16], o[80], w[80];
    int n, al;

    memset(key, 0, 32); memset(iv, 0, 12);
    aes256_gcm_seal(key, iv, 0, 0, 0, 0, ct, tag);
    unhex("530f8afbc74536b9a963b4f1c4cb738b", w);
    ok("gcm256 tc13 empty tag", eq(tag,w,16));

    memset(pt, 0, 16);
    aes256_gcm_seal(key, iv, 0, 0, pt, 16, ct, tag);
    unhex("cea7403d4d606b6e074ec5d3baf39d18", w);
    ok("gcm256 tc14 ct", eq(ct,w,16));
    unhex("d0d1c8a799996bf0265b98b5d48ab919", w);
    ok("gcm256 tc14 tag", eq(tag,w,16));

    unhex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", key);
    unhex("cafebabefacedbaddecaf888", iv);
    n = unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255", pt);
    aes256_gcm_seal(key, iv, 0, 0, pt, n, ct, tag);
    unhex("522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad", w);
    ok("gcm256 tc15 ct", eq(ct,w,n));
    unhex("b094dac5d93471bdec1a502270e3cc6c", w);
    ok("gcm256 tc15 tag", eq(tag,w,16));
    ok("gcm256 tc15 open", aes256_gcm_open(key, iv, 0, 0, ct, n, tag, o) == 0 && eq(o, pt, n));

    n = unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", pt);
    al = unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad);
    aes256_gcm_seal(key, iv, aad, al, pt, n, ct, tag);
    unhex("522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662", w);
    ok("gcm256 tc16 ct", eq(ct,w,n));
    unhex("76fc6ece0f4e1768cddf8853bb2d551b", w);
    ok("gcm256 tc16 tag", eq(tag,w,16));
    ok("gcm256 tc16 open", aes256_gcm_open(key, iv, aad, al, ct, n, tag, o) == 0 && eq(o, pt, n));
    tag[7] ^= 1;
    ok("gcm256 bad tag rejected", aes256_gcm_open(key, iv, aad, al, ct, n, tag, o) != 0);
    tag[7] ^= 1; ct[5] ^= 1;
    ok("gcm256 bad ct rejected", aes256_gcm_open(key, iv, aad, al, ct, n, tag, o) != 0);

    /* A 16-byte key must NOT be reinterpreted as a 256-bit one: seal the same
     * plaintext under aes128 and aes256 with a key whose first half matches and
     * require different ciphertext. This catches a keylen plumbing mistake that
     * every "encrypt then decrypt" test would sail past. */
    uint8_t k16[16]; memcpy(k16, key, 16);
    uint8_t c128[16], t128[16], c256[16], t256[16];
    memset(pt, 0x5a, 16);
    aes128_gcm_seal(k16, iv, 0, 0, pt, 16, c128, t128);
    aes256_gcm_seal(key, iv, 0, 0, pt, 16, c256, t256);
    ok("gcm128 != gcm256 for a shared key prefix", memcmp(c128, c256, 16) != 0);
}

/* TLS 1.2 PRF (RFC 5246 5) against the published test vectors circulated on the
 * IETF TLS list (the ones every 1.2 stack is checked against). Both hashes are
 * covered because the suite chooses: SHA-256 for *_SHA256, SHA-384 for the
 * AES_256_GCM_SHA384 suites that 1.2 servers commonly prefer. */
static void test_tls12_prf(void)
{
    uint8_t secret[64], seed[64], out[256], w[256];
    int sl, dl;

    sl = unhex("9bbe436ba940f017b17652849a71db35", secret);
    dl = unhex("a0ba9f936cda311827a6f796ffd5198c", seed);
    tls12_prf(32, secret, sl, "test label", seed, dl, out, 100);
    unhex("e3f229ba727be17b8d122620557cd453c2aab21d07c3d495329b52d4e61edb5a6b301791e90d35c9c9a46b4e14baf9af0fa022f7077def17abfd3797c0564bab4fbc91666e9def9b97fce34f796789baa48082d122ee42c5a72e5a5110fff70187347b66", w);
    ok("tls12 prf sha256 100B", eq(out, w, 100));

    sl = unhex("b80b733d6ceefcdc71566ea48e5567df", secret);
    dl = unhex("cd665cf6a8447dd6ff8b27555edb7465", seed);
    tls12_prf(48, secret, sl, "test label", seed, dl, out, 148);
    unhex("7b0c18e9ced410ed1804f2cfa34a336a1c14dffb4900bb5fd7942107e81c83cde9ca0faa60be9fe34f82b1233c9146a0e534cb400fed2700884f9dc236f80edd8bfa961144c9e8d792eca722a7b32fc3d416d473ebc2c5fd4abfdad05d9184259b5bf8cd4d90fa0d31e2dec479e4f1a26066f2eea9a69236a3e52655c9e9aee691c8f3a26854308d5eaa3be85e0990703d73e56f", w);
    ok("tls12 prf sha384 148B", eq(out, w, 148));

    /* A short request must be a prefix of a long one -- P_hash is a stream, and
     * an off-by-one in the A() chain shows up here and nowhere else. */
    tls12_prf(32, secret, sl, "test label", seed, dl, w, 96);
    tls12_prf(32, secret, sl, "test label", seed, dl, out, 12);
    ok("tls12 prf 12B is a prefix of 96B", eq(out, w, 12));
    /* 12 bytes is exactly the Finished verify_data length, and 48 the master
     * secret's; neither is a multiple of either digest size, so both exercise
     * the partial-block tail. */
    tls12_prf(48, secret, sl, "master secret", seed, dl, out, 48);
    tls12_prf(48, secret, sl, "master secret", seed, dl, w, 144);
    ok("tls12 prf 48B is a prefix of 144B", eq(out, w, 48));
    /* The label is inside the PRF input, not decoration: changing it must
     * change every byte of the output. */
    tls12_prf(32, secret, sl, "client finished", seed, dl, out, 32);
    tls12_prf(32, secret, sl, "server finished", seed, dl, w, 32);
    ok("tls12 prf label separates outputs", memcmp(out, w, 32) != 0);
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

/* ---- negative / boundary tests (Wycheproof-style): what must reject ---- */

/* rsa.c test hook (not in crypto.h; declared the same way rsa_test.c does). */
int rsa_modexp_be(const uint8_t*,int,const uint8_t*,int,const uint8_t*,int,uint8_t*);

static int allzero(const uint8_t *a, int n) { for (int i = 0; i < n; i++) if (a[i]) return 0; return 1; }

/* AEAD: tampering with tag/ct/aad, wrong nonce/key must all fail open;
 * empty pt and empty aad must roundtrip. Both suites, same contract. */
static void test_aead_negative(void)
{
    /* AES-128-GCM on the McGrew-Viega TC3 vector */
    uint8_t key[16], iv[12], aad[32], pt[64], ct[64], tag[16], o[64];
    unhex("feffe9928665731c6d6a8f9467308308", key);
    unhex("cafebabefacedbaddecaf888", iv);
    int al = unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad);
    int n = unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", pt);
    aes128_gcm_seal(key, iv, aad, al, pt, n, ct, tag);
    ok("neg gcm open valid", aes128_gcm_open(key, iv, aad, al, ct, n, tag, o) == 0 && eq(o, pt, n));
    uint8_t t2[16], c2[64], a2[32], k2[16], v2[12];
    for (int i = 0; i < 3; i++) {                       /* tag head/mid/tail */
        int pos = i == 0 ? 0 : (i == 1 ? 8 : 15);
        memcpy(t2, tag, 16); t2[pos] ^= 1;
        char b[64]; snprintf(b, sizeof b, "neg gcm tag flip[%d] rejected", pos);
        ok(b, aes128_gcm_open(key, iv, aad, al, ct, n, t2, o) == -1);
    }
    memcpy(c2, ct, n); c2[0] ^= 1;
    ok("neg gcm ct flip rejected", aes128_gcm_open(key, iv, aad, al, c2, n, tag, o) == -1);
    memcpy(a2, aad, al); a2[0] ^= 1;
    ok("neg gcm aad flip rejected", aes128_gcm_open(key, iv, a2, al, ct, n, tag, o) == -1);
    aes128_gcm_seal(key, iv, aad, al, pt, 0, ct, tag);
    ok("neg gcm empty pt roundtrip", aes128_gcm_open(key, iv, aad, al, ct, 0, tag, o) == 0);
    aes128_gcm_seal(key, iv, 0, 0, pt, n, ct, tag);
    ok("neg gcm empty aad roundtrip", aes128_gcm_open(key, iv, 0, 0, ct, n, tag, o) == 0 && eq(o, pt, n));
    memcpy(v2, iv, 12); v2[0] ^= 1;
    ok("neg gcm wrong nonce rejected", aes128_gcm_open(key, v2, aad, al, ct, n, tag, o) == -1);
    memcpy(k2, key, 16); k2[0] ^= 1;
    ok("neg gcm wrong key rejected", aes128_gcm_open(k2, iv, aad, al, ct, n, tag, o) == -1);

    /* ChaCha20-Poly1305 on the RFC 8439 A.5 vector */
    uint8_t ck[32], cn[12], ca[16], cp[128], cc[128], ct2[16], co[128];
    unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", ck);
    unhex("070000004041424344454647", cn);
    int cal = unhex("50515253c0c1c2c3c4c5c6c7", ca);
    const char *m = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    int cn2 = (int)strlen(m); memcpy(cp, m, cn2);
    chacha20_poly1305_seal(ck, cn, ca, cal, cp, cn2, cc, ct2);
    ok("neg chacha open valid", chacha20_poly1305_open(ck, cn, ca, cal, cc, cn2, ct2, co) == 0 && eq(co, cp, cn2));
    for (int i = 0; i < 3; i++) {
        int pos = i == 0 ? 0 : (i == 1 ? 8 : 15);
        memcpy(t2, ct2, 16); t2[pos] ^= 1;
        char b[64]; snprintf(b, sizeof b, "neg chacha tag flip[%d] rejected", pos);
        ok(b, chacha20_poly1305_open(ck, cn, ca, cal, cc, cn2, t2, co) == -1);
    }
    memcpy(c2, cc, cn2); c2[cn2 - 1] ^= 1;
    ok("neg chacha ct flip rejected", chacha20_poly1305_open(ck, cn, ca, cal, c2, cn2, ct2, co) == -1);
    memcpy(a2, ca, cal); a2[cal - 1] ^= 1;
    ok("neg chacha aad flip rejected", chacha20_poly1305_open(ck, cn, ca, cal, cc, cn2, ct2, co) == -1);
    chacha20_poly1305_seal(ck, cn, ca, cal, cp, 0, cc, ct2);
    ok("neg chacha empty pt roundtrip", chacha20_poly1305_open(ck, cn, ca, cal, cc, 0, ct2, co) == 0);
    chacha20_poly1305_seal(ck, cn, 0, 0, cp, cn2, cc, ct2);
    ok("neg chacha empty aad roundtrip", chacha20_poly1305_open(ck, cn, 0, 0, cc, cn2, ct2, co) == 0 && eq(co, cp, cn2));
    memcpy(v2, cn, 12); v2[11] ^= 1;
    ok("neg chacha wrong nonce rejected", chacha20_poly1305_open(ck, v2, ca, cal, cc, cn2, ct2, co) == -1);
    uint8_t ck2[32]; memcpy(ck2, ck, 32); ck2[31] ^= 1;
    ok("neg chacha wrong key rejected", chacha20_poly1305_open(ck2, cn, ca, cal, cc, cn2, ct2, co) == -1);
}

/* X25519 low-order points: the function never rejects, so callers (tls.c
 * contributory check) depend on the all-zero output for every one of these. */
static void test_x25519_loworder(void)
{
    uint8_t s[32], u[32], o[32];
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", s);
    /* p = 2^255-19 little-endian: ec ff..ff 7f */
    struct { const char *nm; int first; int top; } pts[] = {
        { "u=0",       0x00, 0x00 },
        { "u=1",       0x01, 0x00 },
        { "u=p-1",     0xec, 0x7f },
        { "u=p",       0xed, 0x7f },
        { "u=p+1",     0xee, 0x7f },
        { "u=0|bit255",0x00, 0x80 },    /* bit255 must be masked -> same as u=0 */
    };
    for (int i = 0; i < 6; i++) {
        memset(u, 0, 32);
        u[0] = (uint8_t)pts[i].first;
        if (pts[i].top == 0x7f) { memset(u + 1, 0xff, 30); u[31] = 0x7f; }
        else u[31] = (uint8_t)pts[i].top;
        x25519(o, s, u);
        char b[64]; snprintf(b, sizeof b, "neg x25519 %s -> zero", pts[i].nm);
        ok(b, allzero(o, 32));
    }
    memset(u, 0, 32); u[0] = 9;                 /* sanity: base point must not fold */
    x25519(o, s, u);
    ok("neg x25519 u=9 nonzero (sanity)", !allzero(o, 32));
}

/* ECDSA boundary rejects, driven per curve. n/p copied from curves_init() in
 * c/crypto/pubkey/ecdsa.c. */
static void ec_neg_case(int cvid, const uint8_t *nn, const uint8_t *pp,
                        const uint8_t *pub, const uint8_t *sig, const uint8_t *hash,
                        int hlen, const char *nm)
{
    int fl = cvid / 8;
    char b[128];
    uint8_t m[96];
    snprintf(b, sizeof b, "%s valid", nm);
    ok(b, ecdsa_verify(cvid, pub, sig, hash, hlen) == 1);

    memcpy(m, sig, 2 * fl); memset(m, 0, fl);
    snprintf(b, sizeof b, "%s reject r=0", nm); ok(b, ecdsa_verify(cvid, pub, m, hash, hlen) == 0);
    memcpy(m, sig, 2 * fl); memset(m + fl, 0, fl);
    snprintf(b, sizeof b, "%s reject s=0", nm); ok(b, ecdsa_verify(cvid, pub, m, hash, hlen) == 0);
    memcpy(m, nn, fl); memcpy(m + fl, sig + fl, fl);
    snprintf(b, sizeof b, "%s reject r=n", nm); ok(b, ecdsa_verify(cvid, pub, m, hash, hlen) == 0);
    memcpy(m, sig, 2 * fl); memcpy(m + fl, nn, fl);
    snprintf(b, sizeof b, "%s reject s=n", nm); ok(b, ecdsa_verify(cvid, pub, m, hash, hlen) == 0);
    memcpy(m, nn, fl); m[fl - 1]++;             /* n+1 (last byte of both n's is odd, no carry) */
    memcpy(m + fl, sig + fl, fl);
    snprintf(b, sizeof b, "%s reject r=n+1", nm); ok(b, ecdsa_verify(cvid, pub, m, hash, hlen) == 0);

    memcpy(m, pp, fl); memcpy(m + fl, pub + fl, fl);
    snprintf(b, sizeof b, "%s reject qx=p", nm); ok(b, ecdsa_verify(cvid, m, sig, hash, hlen) == 0);
    memcpy(m, pub, 2 * fl); memcpy(m + fl, pp, fl);
    snprintf(b, sizeof b, "%s reject qy=p", nm); ok(b, ecdsa_verify(cvid, m, sig, hash, hlen) == 0);
    memcpy(m, pub, 2 * fl); m[2 * fl - 1] ^= 1;  /* still < p, but off the curve */
    snprintf(b, sizeof b, "%s reject offcurve pub", nm); ok(b, ecdsa_verify(cvid, m, sig, hash, hlen) == 0);
    memset(m, 0, 2 * fl);                        /* point at infinity encoding */
    snprintf(b, sizeof b, "%s reject zero pub", nm); ok(b, ecdsa_verify(cvid, m, sig, hash, hlen) == 0);

    memcpy(m, sig, 2 * fl); m[fl / 2] ^= 1;
    snprintf(b, sizeof b, "%s reject sig flip", nm); ok(b, ecdsa_verify(cvid, pub, m, hash, hlen) == 0);
    uint8_t wh[64]; memcpy(wh, hash, hlen); wh[hlen - 1] ^= 1;
    snprintf(b, sizeof b, "%s reject hash flip", nm); ok(b, ecdsa_verify(cvid, pub, sig, wh, hlen) == 0);
}

static void test_ecdsa_negative(void)
{
    uint8_t p384n[48], p384p[48];
    unhex("ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973", p384n);
    unhex("fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff", p384p);
    ec_neg_case(256, p256_n, p256_p, ec256_256_pub, ec256_256_sig, ec256_256_hash, 32, "neg ecdsa p256");
    ec_neg_case(384, p384n, p384p, ec384_384_pub, ec384_384_sig, ec384_384_hash, 48, "neg ecdsa p384");
}

static void test_rsa_negative(void)
{
    const int nl = 256, el = 3;
    uint8_t bad[256], even_n[256], wh[64];
    ok("neg rsa pkcs1 valid", rsa_pkcs1_verify(rsa_n, nl, rsa_e, el, rsa_sig256, nl, rsa_h256, 32) == 1);
    ok("neg rsa pss valid", rsa_pss_verify(rsa_n, nl, rsa_e, el, rsa_pss256, nl, rsa_h256, 32) == 1);

    memcpy(bad, rsa_sig256, nl); bad[200] ^= 1;
    ok("neg rsa pkcs1 sig flip", rsa_pkcs1_verify(rsa_n, nl, rsa_e, el, bad, nl, rsa_h256, 32) == 0);
    memcpy(bad, rsa_pss256, nl); bad[200] ^= 1;
    ok("neg rsa pss sig flip", rsa_pss_verify(rsa_n, nl, rsa_e, el, bad, nl, rsa_h256, 32) == 0);
    memcpy(wh, rsa_h256, 32); wh[31] ^= 1;
    ok("neg rsa pkcs1 hash flip", rsa_pkcs1_verify(rsa_n, nl, rsa_e, el, rsa_sig256, nl, wh, 32) == 0);
    ok("neg rsa pss hash flip", rsa_pss_verify(rsa_n, nl, rsa_e, el, rsa_pss256, nl, wh, 32) == 0);

    /* siglen > nlen must be refused before any bignum work */
    ok("neg rsa pkcs1 siglen>nlen", rsa_pkcs1_verify(rsa_n, nl, rsa_e, el, rsa_sig256, nl + 1, rsa_h256, 32) == 0);
    ok("neg rsa pss siglen>nlen", rsa_pss_verify(rsa_n, nl, rsa_e, el, rsa_pss256, nl + 1, rsa_h256, 32) == 0);
    /* Montgomery requires an odd modulus */
    memcpy(even_n, rsa_n, nl); even_n[nl - 1] &= (uint8_t)~1;
    ok("neg rsa pkcs1 even modulus", rsa_pkcs1_verify(even_n, nl, rsa_e, el, rsa_sig256, nl, rsa_h256, 32) == 0);
    ok("neg rsa pss even modulus", rsa_pss_verify(even_n, nl, rsa_e, el, rsa_pss256, nl, rsa_h256, 32) == 0);
    /* sig >= n refused */
    ok("neg rsa pkcs1 sig=n", rsa_pkcs1_verify(rsa_n, nl, rsa_e, el, rsa_n, nl, rsa_h256, 32) == 0);
    ok("neg rsa pss sig=n", rsa_pss_verify(rsa_n, nl, rsa_e, el, rsa_n, nl, rsa_h256, 32) == 0);

    /* rsa_modexp_be hook boundaries (RL=130 -> nl must be <= 516) */
    uint8_t big[520], outm[520], e3[1] = {3};
    memset(big, 0x11, sizeof big);
    ok("neg rsa modexp nl=517", rsa_modexp_be(big, 1, e3, 1, big, 517, outm) == -1);
    uint8_t n32[32], b32[32];
    memset(n32, 0, 32); n32[31] = 0x65;         /* odd */
    memset(b32, 0, 32); b32[31] = 2;
    n32[31] &= (uint8_t)~1;                      /* even modulus */
    ok("neg rsa modexp even n", rsa_modexp_be(b32, 32, e3, 1, n32, 32, outm) == -2);
    n32[31] = 0x65;
    ok("neg rsa modexp base>=n", rsa_modexp_be(n32, 32, e3, 1, n32, 32, outm) == -3);
    memset(b32, 0, 32);                          /* base = 0 */
    int rc = rsa_modexp_be(b32, 32, e3, 1, n32, 32, outm);
    ok("neg rsa modexp 0^3 = 0", rc == 0 && allzero(outm, 32));
}

static void test_hkdf_bounds(void)
{
    static uint8_t o[12240];
    uint8_t ref[48], info[2 + 1 + 6 + 64 + 1 + 64];
    char label[66];
    memset(label, 'L', 65); label[65] = 0;
    ok("neg expand_label label 65", hkdf_expand_label(32, el256_secret, label, 0, 0, o, 32) == -1);
    uint8_t ctx[65]; memset(ctx, 0x55, 65);
    ok("neg expand_label ctx 65", hkdf_expand_label(32, el256_secret, "k", ctx, 65, o, 32) == -1);
    ok("neg expand_label outlen 65536", hkdf_expand_label(32, el256_secret, "k", 0, 0, o, 65536) == -1);
    /* 8161 = 255*32+1: above the RFC 5869 cap hkdf_expand() enforces, so
     * expand_label must reject it too -- returning 0 would leave `o` unwritten */
    ok("neg expand_label outlen 255*hlen+1", hkdf_expand_label(32, el256_secret, "k", 0, 0, o, 8161) == -1);
    ok("neg expand_label hlen 16", hkdf_expand_label(16, el256_secret, "k", 0, 0, o, 32) == -1);

    /* boundary accepts: label/ctx exactly 64 bytes, outlen 32. Verify against a
     * hand-built RFC 8446 HkdfLabel fed through plain hkdf_expand. */
    label[64] = 0;                               /* now exactly 64 chars */
    int n = 0;
    info[n++] = 0; info[n++] = 32;               /* outlen BE */
    info[n++] = (uint8_t)(6 + 64);
    memcpy(info + n, "tls13 ", 6); n += 6;
    memcpy(info + n, label, 64); n += 64;
    info[n++] = 64;
    memcpy(info + n, ctx, 64); n += 64;
    ok("neg expand_label 64/64/32 rc", hkdf_expand_label(32, el256_secret, label, ctx, 64, o, 32) == 0);
    hkdf_expand(32, el256_secret, info, n, ref, 32);
    ok("neg expand_label 64/64/32 matches manual HkdfLabel", eq(o, ref, 32));

    /* hkdf_expand at the RFC 5869 ceiling: outlen = 255*hlen must succeed */
    uint8_t prk[32], i10[10];
    unhex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", prk);
    int il = unhex("f0f1f2f3f4f5f6f7f8f9", i10);
    uint8_t w42[42];
    unhex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", w42);
    memset(o, 0xAA, 8160);
    hkdf_expand(32, prk, i10, il, o, 8160);      /* 255*32 */
    ok("neg hkdf_expand L=255*32 head", eq(o, w42, 42));
    ok("neg hkdf_expand L=255*32 tail written", o[8159] != 0xAA);
    memset(o, 0xAA, 12240);
    hkdf_expand(48, hk384_prk, hk384_info, (int)sizeof(hk384_info), o, 12240);  /* 255*48 */
    ok("neg hkdf_expand L=255*48 head", eq(o, hk384_okm, 100));
    ok("neg hkdf_expand L=255*48 tail written", o[12239] != 0xAA);
    /* hlen other than 32/48: documented silent no-op (early return, no writes) */
    memset(o, 0xAA, 64);
    hkdf_expand(16, prk, i10, il, o, 64);
    ok("neg hkdf_expand hlen16 silent no-op", o[0] == 0xAA && o[63] == 0xAA);
}

int main(void)
{
    test_sha();
    test_hmac_hkdf();
    test_aesgcm();
    test_aes256gcm();
    test_tls12_prf();
    test_chacha();
    test_x25519();
    test_ecdsa();
    test_rsa();
    test_aead_negative();
    test_x25519_loworder();
    test_ecdsa_negative();
    test_rsa_negative();
    test_hkdf_bounds();
    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
