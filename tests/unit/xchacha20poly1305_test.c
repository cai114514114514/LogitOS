/* XChaCha20-Poly1305 host unit test.
 *
 * THREE INDEPENDENT CHECKS, DELIBERATELY KEPT SEPARATE:
 *
 *  1. XCHACHA_HCHACHA20_STANDALONE -- draft-irtf-cfrg-xchacha-03 section
 *     2.2.1's own vector for the HChaCha20 block function ALONE, with no
 *     Poly1305 or nonce split involved. This is the ONLY thing in this file
 *     that can catch "forgot to skip the final state addition" (the bug
 *     XCHACHA_BUG_ADD_BACK injects): that bug produces a fully
 *     self-consistent XChaCha20-Poly1305 (seal and open still agree with
 *     each other, and even agree with a differential run against an
 *     independently-computed keystream for the SAME wrong subkey), so
 *     nothing downstream of subkey derivation can see it. Only comparing
 *     the subkey itself against an external answer can.
 *
 *  2. XCHACHA_AEAD_A1 -- the same draft's Appendix A.1, the full AEAD
 *     worked example, checked byte for byte through the real seal/open
 *     entry points (not just the subkey).
 *
 *  3. XCHACHA_KAT[] (tests/unit/xchacha_kat.inc) -- all 306 vectors from
 *     Wycheproof's (C2SP fork) xchacha20_poly1305_test.json, ivSize=192/
 *     keySize=256/tagSize=128 group. tcId 1 there IS the draft's A.1
 *     vector (its own "comment" field says so) -- kept as a second,
 *     independent check of #2 rather than removed as a duplicate, because
 *     the KAT file is machine-generated and #2 is hand-transcribed
 *     straight from the draft text; if they ever disagreed that would be
 *     the bug worth finding. 246 of the 306 are "valid" (seal must
 *     reproduce ct+tag, open must recover the plaintext); 60 are "invalid"
 *     (bit-flipped tags) and open() must reject every one.
 *
 * A further, harder check -- that ours agrees with a SECOND, independently
 * written implementation (openssl) over many random inputs, not just the
 * fixed corpus above -- is tests/unit/run-xchacha-openssl.sh
 * (`make test-xchacha-openssl`), which also carries the negative controls;
 * see that script's header for why openssl can only be driven as a plain
 * stream cipher here and not as the AEAD.
 *
 * WHAT NEITHER GATE CAN SEE: constant-time behaviour. See
 * xchacha20poly1305.c's header for exactly which parts touch secret
 * material and why the ones that are not constant-time say so in the
 * source instead of leaving it silent.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "crypto.h"
#include "xchacha20poly1305.h"

static int pass, fail;
static void ck(int cond, const char *what)
{
    if (cond) { pass++; }
    else { fail++; printf("FAIL: %s\n", what); }
}

static int unhex(uint8_t *o, const char *h, int max)
{
    int n = (int)strlen(h) / 2;
    if (n > max) return -1;
    for (int i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); o[i] = (uint8_t)v; }
    return n;
}

#include "xchacha_kat.inc"

/* --- 1. HChaCha20 standalone (draft section 2.2.1) --- */
static void test_hchacha20_standalone(void)
{
    uint8_t key[32], nonce[16], want[32], got[32];
    unhex(key, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", 32);
    unhex(nonce, "000000090000004a0000000031415927", 16);
    unhex(want, "82413b4227b27bfed30e42508a877d73a0f9e4d58a74a853c12ec41326d3ecdc", 32);
    hchacha20(key, nonce, got);
    ck(memcmp(got, want, 32) == 0, "hchacha20 standalone vector (draft 2.2.1)");
}

/* --- 2. Full AEAD (draft Appendix A.1) --- */
static void test_aead_a1(void)
{
    uint8_t key[32], nonce[24], aad[12], pt[114], want_ct[114], want_tag[16];
    unhex(key, "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", 32);
    unhex(nonce, "404142434445464748494a4b4c4d4e4f5051525354555657", 24);
    unhex(aad, "50515253c0c1c2c3c4c5c6c7", 12);
    unhex(pt,
        "4c616469657320616e642047656e746c656d656e206f662074686520636c6173"
        "73206f66202739393a204966204920636f756c64206f6666657220796f75206f"
        "6e6c79206f6e652074697020666f7220746865206675747572652c2073756e73"
        "637265656e20776f756c642062652069742e", 114);
    unhex(want_ct,
        "bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb"
        "731c7f1b0b4aa6440bf3a82f4eda7e39ae64c6708c54c216cb96b72e1213b4522"
        "f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b2383565d3fff921"
        "f9664c97637da9768812f615c68b13b52e", 114);
    unhex(want_tag, "c0875924c1c7987947deafd8780acf49", 16);

    uint8_t ct[114], tag[16];
    xchacha20_poly1305_seal(key, nonce, aad, 12, pt, 114, ct, tag);
    ck(memcmp(ct, want_ct, 114) == 0, "AEAD A.1 ciphertext");
    ck(memcmp(tag, want_tag, 16) == 0, "AEAD A.1 tag");

    uint8_t got_pt[114];
    int r = xchacha20_poly1305_open(key, nonce, aad, 12, ct, 114, tag, got_pt);
    ck(r == 0, "AEAD A.1 open accepts genuine tag");
    ck(memcmp(got_pt, pt, 114) == 0, "AEAD A.1 open recovers plaintext");

    uint8_t bad_tag[16]; memcpy(bad_tag, tag, 16); bad_tag[0] ^= 1;
    ck(xchacha20_poly1305_open(key, nonce, aad, 12, ct, 114, bad_tag, got_pt) == -1,
       "AEAD A.1 open rejects a flipped tag bit");
}

/* --- 3. Wycheproof battery --- */
static void test_wycheproof(void)
{
    int valid_checked = 0, invalid_checked = 0;
    for (int i = 0; i < XCHACHA_KAT_N; i++) {
        const struct xchacha_kat *t = &XCHACHA_KAT[i];
        uint8_t key[32], nonce[24], aad[600], msg[600], ct[600], tag[16];
        int kl = unhex(key, t->key, 32);
        int nl = unhex(nonce, t->iv, 24);
        int al = unhex(aad, t->aad, sizeof aad);
        int ml = unhex(msg, t->msg, sizeof msg);
        int cl = unhex(ct, t->ct, sizeof ct);
        int tl = unhex(tag, t->tag, 16);
        char label[128];
        if (kl != 32 || nl != 24 || al < 0 || ml < 0 || cl != ml || tl != 16) {
            snprintf(label, sizeof label, "wycheproof tcId=%d: vector did not parse as expected", t->tcid);
            ck(0, label);
            continue;
        }

        if (t->valid) {
            uint8_t our_ct[600], our_tag[16];
            xchacha20_poly1305_seal(key, nonce, aad, al, msg, ml, our_ct, our_tag);
            snprintf(label, sizeof label, "wycheproof tcId=%d seal ct", t->tcid);
            ck(ml == 0 || memcmp(our_ct, ct, (size_t)ml) == 0, label);
            snprintf(label, sizeof label, "wycheproof tcId=%d seal tag", t->tcid);
            ck(memcmp(our_tag, tag, 16) == 0, label);

            uint8_t our_pt[600];
            int r = xchacha20_poly1305_open(key, nonce, aad, al, ct, ml, tag, our_pt);
            snprintf(label, sizeof label, "wycheproof tcId=%d open accepts + recovers", t->tcid);
            ck(r == 0 && (ml == 0 || memcmp(our_pt, msg, (size_t)ml) == 0), label);
            valid_checked++;
        } else {
            uint8_t our_pt[600];
            snprintf(label, sizeof label, "wycheproof tcId=%d open rejects (%s)", t->tcid, t->comment);
            ck(xchacha20_poly1305_open(key, nonce, aad, al, ct, ml, tag, our_pt) == -1, label);
            invalid_checked++;
        }
    }
    printf("wycheproof: %d valid + %d invalid vectors checked (of %d)\n",
           valid_checked, invalid_checked, XCHACHA_KAT_N);
}

int main(void)
{
    test_hchacha20_standalone();
    test_aead_a1();
    test_wycheproof();
    printf("%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
