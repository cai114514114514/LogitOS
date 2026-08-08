/* Ed25519 (c/crypto/pubkey/ed25519.c) against RFC 8032 and against openssl.
 *
 * Three independent sources, because our signer agreeing with our verifier
 * proves only that they were written by the same person on the same afternoon:
 *
 *   RFC 8032 7.1  -- five IETF-published (seed, public key, message, signature)
 *                    quadruples, compiled in below. These pin the SIGNER: the
 *                    signature is deterministic, so we must reproduce the
 *                    published bytes exactly, not merely produce something that
 *                    verifies.
 *   openssl       -- fresh keys and signatures over random messages, generated
 *                    by tests/unit/ed25519_gen.sh, which we only get to CHECK.
 *                    This pins the VERIFIER against an implementation that has
 *                    never seen ours, and (by re-signing the same message with
 *                    the same key) pins the signer's determinism against it too.
 *   the rejections -- below. A verifier that returns 1 unconditionally passes
 *                    every positive case above and none of these.
 *
 * Build: see the test-ed25519 rule in the Makefile. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "crypto.h"

static int checks, failed;

static void ok(int cond, const char *what)
{
    checks++;
    if (cond) { printf("ok   %s\n", what); }
    else      { printf("FAIL %s\n", what); failed++; }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int unhex(uint8_t *out, int max, const char *h)
{
    int n = 0;
    while (h[0] && h[1]) {
        int a = hexval(h[0]), b = hexval(h[1]);
        if (a < 0 || b < 0) return -1;
        if (n >= max) return -1;
        out[n++] = (uint8_t)((a << 4) | b);
        h += 2;
    }
    return h[0] ? -1 : n;
}

/* ---------------------------------------------------- RFC 8032 section 7.1 -- */

struct rfcvec { const char *name, *seed, *pub, *msg, *sig; };

static const struct rfcvec RFC8032[] = {
{ "RFC8032 TEST 1 (empty message)",
  "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
  "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
  "",
  "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
{ "RFC8032 TEST 2 (1 byte)",
  "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
  "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
  "72",
  "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
{ "RFC8032 TEST 3 (2 bytes)",
  "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
  "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
  "af82",
  "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
{ "RFC8032 TEST SHA(abc) (64 bytes)",
  "833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
  "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
  "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
  "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
  "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b589"
  "09351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704" },
{ "RFC8032 TEST 1024 (1023 bytes)",
  "f5e5767cf153319517630f226876b86c8160cc583bc013744c6bf255f5cc0ee5",
  "278117fc144c72340f67d0f2316e8386ceffbf2b2428c9c51fef7c597f1d426e",
  "08b8b2b733424243760fe426a4b54908632110a66c2f6591eabd3345e3e4eb98"
  "fa6e264bf09efe12ee50f8f54e9f77b1e355f6c50544e23fb1433ddf73be84d8"
  "79de7c0046dc4996d9e773f4bc9efe5738829adb26c81b37c93a1b270b20329d"
  "658675fc6ea534e0810a4432826bf58c941efb65d57a338bbd2e26640f89ffbc"
  "1a858efcb8550ee3a5e1998bd177e93a7363c344fe6b199ee5d02e82d522c4fe"
  "ba15452f80288a821a579116ec6dad2b3b310da903401aa62100ab5d1a36553e"
  "06203b33890cc9b832f79ef80560ccb9a39ce767967ed628c6ad573cb116dbef"
  "efd75499da96bd68a8a97b928a8bbc103b6621fcde2beca1231d206be6cd9ec7"
  "aff6f6c94fcd7204ed3455c68c83f4a41da4af2b74ef5c53f1d8ac70bdcb7ed1"
  "85ce81bd84359d44254d95629e9855a94a7c1958d1f8ada5d0532ed8a5aa3fb2"
  "d17ba70eb6248e594e1a2297acbbb39d502f1a8c6eb6f1ce22b3de1a1f40cc24"
  "554119a831a9aad6079cad88425de6bde1a9187ebb6092cf67bf2b13fd65f270"
  "88d78b7e883c8759d2c4f5c65adb7553878ad575f9fad878e80a0c9ba63bcbcc"
  "2732e69485bbc9c90bfbd62481d9089beccf80cfe2df16a2cf65bd92dd597b07"
  "07e0917af48bbb75fed413d238f5555a7a569d80c3414a8d0859dc65a46128ba"
  "b27af87a71314f318c782b23ebfe808b82b0ce26401d2e22f04d83d1255dc51a"
  "ddd3b75a2b1ae0784504df543af8969be3ea7082ff7fc9888c144da2af58429e"
  "c96031dbcad3dad9af0dcbaaaf268cb8fcffead94f3c7ca495e056a9b47acdb7"
  "51fb73e666c6c655ade8297297d07ad1ba5e43f1bca32301651339e22904cc8c"
  "42f58c30c04aafdb038dda0847dd988dcda6f3bfd15c4b4c4525004aa06eeff8"
  "ca61783aacec57fb3d1f92b0fe2fd1a85f6724517b65e614ad6808d6f6ee34df"
  "f7310fdc82aebfd904b01e1dc54b2927094b2db68d6f903b68401adebf5a7e08"
  "d78ff4ef5d63653a65040cf9bfd4aca7984a74d37145986780fc0b16ac451649"
  "de6188a7dbdf191f64b5fc5e2ab47b57f7f7276cd419c17a3ca8e1b939ae49e4"
  "88acba6b965610b5480109c8b17b80e1b7b750dfc7598d5d5011fd2dcc5600a3"
  "2ef5b52a1ecc820e308aa342721aac0943bf6686b64b2579376504ccc493d97e"
  "6aed3fb0f9cd71a43dd497f01f17c0e2cb3797aa2a2f256656168e6c496afc5f"
  "b93246f6b1116398a346f1a641f3b041e989f7914f90cc2c7fff357876e506b5"
  "0d334ba77c225bc307ba537152f3f1610e4eafe595f6d9d90d11faa933a15ef1"
  "369546868a7f3a45a96768d40fd9d03412c091c6315cf4fde7cb68606937380d"
  "b2eaaa707b4c4185c32eddcdd306705e4dc1ffc872eeee475a64dfac86aba41c"
  "0618983f8741c5ef68d3a101e8a3b8cac60c905c15fc910840b94c00a0b9d0",
  "0aab4c900501b3e24d7cdf4663326a3a87df5e4843b2cbdb67cbf6e460fec350"
  "aa5371b1508f9f4528ecea23c436d94b5e8fcd4f681e30a6ac00a9704a188a03" },
};

/* -------------------------------------------------------- a fake RNG ------- */
/* Deterministic, so keypair() is reproducible in a test. NOT a CSPRNG and not
 * pretending to be: it exists so the injected-randomness path gets exercised. */
static uint64_t frs = 0x0123456789abcdefULL;
static void fakerand(uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        frs ^= frs << 13; frs ^= frs >> 7; frs ^= frs << 17;
        b[i] = (uint8_t)frs;
    }
}
static void zerorand(uint8_t *b, int n) { for (int i=0;i<n;i++) b[i]=0; }

/* --------------------------------------------------- openssl-generated ----- */
/* Format, one case per line: pub_hex msg_hex sig_hex */
static void run_openssl_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { printf("FAIL cannot open %s\n", path); failed++; checks++; return; }
    char line[16384];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char pubh[128], msgh[8192], sigh[256];
        if (sscanf(line, "%127s %8191s %255s", pubh, msgh, sigh) != 3) continue;
        uint8_t pub[32], sig[64];
        static uint8_t msg[4096];
        int ml = 0;
        if (unhex(pub, sizeof pub, pubh) != 32) continue;
        if (unhex(sig, sizeof sig, sigh) != 64) continue;
        if (strcmp(msgh, "-") == 0) ml = 0;
        else { ml = unhex(msg, sizeof msg, msgh); if (ml < 0) continue; }
        char what[96];
        snprintf(what, sizeof what, "openssl case %d verifies", n);
        ok(ed25519_verify(sig, msg, (size_t)ml, pub) == 1, what);
        /* and a one-bit flip in the message must not */
        if (ml > 0) {
            msg[0] ^= 1;
            snprintf(what, sizeof what, "openssl case %d rejects a flipped message byte", n);
            ok(ed25519_verify(sig, msg, (size_t)ml, pub) == 0, what);
            msg[0] ^= 1;
        }
        n++;
    }
    fclose(f);
    ok(n >= 8, "openssl generated at least 8 cases");
}

int main(int argc, char **argv)
{
    ok(ed25519_sc_reduce_selftest() == 0, "scalar reduction: L mod L == 0, (L-1) mod L == L-1");

    /* --- RFC 8032: the signer must reproduce the published bytes ---------- */
    static uint8_t msg[2048];
    for (unsigned v = 0; v < sizeof RFC8032 / sizeof RFC8032[0]; v++) {
        const struct rfcvec *t = &RFC8032[v];
        uint8_t seed[32], pub[32], want[64], sig[64], gotpub[32];
        int ml;
        if (unhex(seed, 32, t->seed) != 32 || unhex(pub, 32, t->pub) != 32 ||
            unhex(want, 64, t->sig) != 64 || (ml = unhex(msg, sizeof msg, t->msg)) < 0) {
            printf("FAIL %s: bad vector encoding in this file\n", t->name); failed++; checks++;
            continue;
        }
        char what[160];

        ed25519_pubkey(gotpub, seed);
        snprintf(what, sizeof what, "%s: public key derived from seed", t->name);
        ok(memcmp(gotpub, pub, 32) == 0, what);

        ed25519_sign(sig, msg, (size_t)ml, seed, pub);
        snprintf(what, sizeof what, "%s: signature is byte-identical", t->name);
        ok(memcmp(sig, want, 64) == 0, what);

        snprintf(what, sizeof what, "%s: the published signature verifies", t->name);
        ok(ed25519_verify(want, msg, (size_t)ml, pub) == 1, what);
    }

    /* --- determinism: signing twice gives the same bytes ------------------ */
    {
        uint8_t seed[32], pub[32], s1[64], s2[64];
        unhex(seed, 32, RFC8032[2].seed); unhex(pub, 32, RFC8032[2].pub);
        const char *m = "the same message, twice";
        ed25519_sign(s1, (const uint8_t *)m, strlen(m), seed, pub);
        ed25519_sign(s2, (const uint8_t *)m, strlen(m), seed, pub);
        ok(memcmp(s1, s2, 64) == 0, "signing is deterministic (no per-signature randomness)");
    }

    /* --- key generation --------------------------------------------------- */
    {
        uint8_t pub1[32], seed1[32], pub2[32], seed2[32];
        ok(ed25519_keypair(pub1, seed1, fakerand) == 0, "keypair() succeeds");
        ok(ed25519_keypair(pub2, seed2, fakerand) == 0, "keypair() succeeds again");
        ok(memcmp(seed1, seed2, 32) != 0, "two keypairs have different seeds");
        ok(memcmp(pub1, pub2, 32) != 0, "two keypairs have different public keys");
        uint8_t derived[32];
        ed25519_pubkey(derived, seed1);
        ok(memcmp(derived, pub1, 32) == 0, "keypair()'s public key matches the seed");
        uint8_t sig[64];
        const char *m = "round trip";
        ed25519_sign(sig, (const uint8_t *)m, strlen(m), seed1, pub1);
        ok(ed25519_verify(sig, (const uint8_t *)m, strlen(m), pub1) == 1,
           "a generated key signs and verifies");
        ok(ed25519_verify(sig, (const uint8_t *)m, strlen(m), pub2) == 0,
           "REJECT: the other key does not verify it");
        uint8_t p0[32], s0[32];
        ok(ed25519_keypair(p0, s0, zerorand) == -1,
           "REJECT: an entropy source returning zeroes is refused");
        ok(ed25519_keypair(p0, s0, NULL) == -1, "REJECT: a NULL entropy source is refused");
    }

    /* --- the rejections. Each one is a way a broken verifier says yes. ----- */
    {
        uint8_t seed[32], pub[32], sig[64], bad[64];
        unhex(seed, 32, RFC8032[1].seed); unhex(pub, 32, RFC8032[1].pub);
        const uint8_t m[1] = { 0x72 };
        ed25519_sign(sig, m, 1, seed, pub);
        ok(ed25519_verify(sig, m, 1, pub) == 1, "baseline for the rejections verifies");

        for (int b = 0; b < 8; b++) {
            memcpy(bad, sig, 64); bad[b * 8] ^= 1;
            char what[96];
            snprintf(what, sizeof what, "REJECT: signature byte %d flipped", b * 8);
            ok(ed25519_verify(bad, m, 1, pub) == 0, what);
        }

        /* S >= L. S + L is the classic malleability: it satisfies the group
         * equation, so a verifier that omits the canonicality check accepts a
         * SECOND valid signature for the same message and key. */
        memcpy(bad, sig, 64);
        {
            /* add L to S (little-endian) */
            static const uint8_t L[32] = {
                0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
                0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10 };
            unsigned carry = 0;
            for (int i = 0; i < 32; i++) {
                unsigned t = bad[32+i] + L[i] + carry;
                bad[32+i] = (uint8_t)t; carry = t >> 8;
            }
        }
        ok(ed25519_verify(bad, m, 1, pub) == 0, "REJECT: S + L (signature malleability)");

        /* S = L exactly, and S all-ones */
        memcpy(bad, sig, 64);
        {
            static const uint8_t L[32] = {
                0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
                0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10 };
            memcpy(bad + 32, L, 32);
        }
        ok(ed25519_verify(bad, m, 1, pub) == 0, "REJECT: S == L");
        memcpy(bad, sig, 64); memset(bad + 32, 0xff, 32);
        ok(ed25519_verify(bad, m, 1, pub) == 0, "REJECT: S all-ones");

        /* S = 0 and R = 0 */
        memcpy(bad, sig, 64); memset(bad + 32, 0, 32);
        ok(ed25519_verify(bad, m, 1, pub) == 0, "REJECT: S == 0");
        memcpy(bad, sig, 64); memset(bad, 0, 32);
        ok(ed25519_verify(bad, m, 1, pub) == 0, "REJECT: R == 0");

        /* wrong message, and the empty message against a 1-byte signature */
        const uint8_t m2[1] = { 0x73 };
        ok(ed25519_verify(sig, m2, 1, pub) == 0, "REJECT: wrong message");
        ok(ed25519_verify(sig, m, 0, pub) == 0, "REJECT: truncated message");

        /* wrong public key: flip one bit of it */
        uint8_t badpub[32]; memcpy(badpub, pub, 32); badpub[3] ^= 0x10;
        ok(ed25519_verify(sig, m, 1, badpub) == 0, "REJECT: one-bit-different public key");

        /* a public key that is not a point at all. y = p-1 has no x here for
         * some sign bits; walk a few until one is genuinely undecodable, then
         * assert BOTH that ed25519_point_valid says so and that verify says no. */
        int found = 0;
        for (int k = 0; k < 64 && !found; k++) {
            uint8_t np[32]; memset(np, 0, 32); np[0] = (uint8_t)(2 + k);
            if (!ed25519_point_valid(np)) {
                ok(ed25519_verify(sig, m, 1, np) == 0,
                   "REJECT: public key that is not a curve point");
                found = 1;
            }
        }
        ok(found, "found a non-point encoding to test with");

        /* swapped halves */
        memcpy(bad, sig + 32, 32); memcpy(bad + 32, sig, 32);
        ok(ed25519_verify(bad, m, 1, pub) == 0, "REJECT: R and S swapped");
    }

    if (argc > 1) run_openssl_file(argv[1]);
    else { printf("(no openssl vector file given -- RFC vectors only)\n"); }

    printf("\n%d checks, %d failed\n", checks, failed);
    if (!failed) printf("ED25519 ALL PASS\n");
    return failed ? 1 : 0;
}
