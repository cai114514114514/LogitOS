/* THE ATTACK: a hostile KEX_ECDH_INIT whose 32 bytes are a low-order point
 * (or any of the non-canonical spellings of one -- u+k*p that fits, the
 * high-bit-set spelling) hands the attacker a shared secret they chose. If
 * c/crypto's ladder reduces those to the all-zero secret AND ssh_kex.c's
 * all-zero refusal fires, every one of them must come back as a refusal.
 * The other half of the file -- ordinary points in NON-canonical spellings
 * -- must be ACCEPTED with exactly the RFC 7748 masked-secret the
 * independent pure-Python ladder computes (ssh_attack_kex_gen.py: its own
 * oracle, cross-checked against python-cryptography, sharing no code with
 * this tree), because a ladder that mishandles the high bit is a spec
 * violation even where it is not exploitable.
 *
 * The NEGATIVE CONTROL is the vector set itself: `refuse` flags come from
 * the Python oracle, not from this tree, so a C-side regression in either
 * direction (accepting an attacker-chosen secret, or refusing a legitimate
 * non-canonical encoding) shows up as a mismatch, not as silence. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "ssh.h"
#include "ssh_wire.h"
#include "ssh_kex.h"
#include "crypto.h"

static int checks, failed;
static void ok(int cond, const char *what)
{
    checks++;
    if (cond) printf("ok   %s\n", what);
    else { printf("FAIL %s\n", what); failed++; }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int hex2bin(const char *s, uint8_t *out, int max)
{
    int n = 0;
    while (s[0] && s[1] && n < max) {
        int hi = hexval(s[0]), lo = hexval(s[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n;
}

static uint8_t g_server_priv[32];
static void fixed_priv(uint8_t *p, int n)
{
    /* The same SERVER_PRIV the generator pinned, so expected secrets line
     * up. Fills the whole request the way sshd's rnd callback would. */
    for (int i = 0; i < n && i < 32; i++) p[i] = g_server_priv[i];
    for (int i = 32; i < n; i++) p[i] = (uint8_t)i;
}

int main(int argc, char **argv)
{
    const char *vecpath = argc > 1 ? argv[1] : "build/ssh_attack_kex_vectors.txt";
    FILE *f = fopen(vecpath, "r");
    if (!f) { fprintf(stderr, "cannot open %s (run ssh_attack_kex_gen.py first)\n", vecpath); return 1; }

    /* Fixed, boring transcript inputs -- this test is about the ECDH input,
     * not the hash, which ssh_kex_test.c already pins against its oracle. */
    static const uint8_t V_C[] = "SSH-2.0-attacker";
    static const uint8_t V_S[] = "SSH-2.0-LogitOS_1.0";
    uint8_t I_C[64], I_S[64];
    int iclen = 0, islen = 0;
    for (int i = 0; i < 40; i++) { I_C[iclen++] = (uint8_t)i; I_S[islen++] = (uint8_t)(i ^ 0x5a); }

    uint8_t host_seed[32], host_pub[32];
    for (int i = 0; i < 32; i++) host_seed[i] = (uint8_t)(i * 3 + 1);
    x25519_base(host_pub, host_seed); /* any fixed pair; ed25519_sign is deterministic */

    char line[1024];
    int nvec = 0, nref = 0, nacc = 0;
    while (fgets(line, sizeof line, f)) {
        char tag[16], name[128], qchex[128], refuse_s[8], khex[128];
        if (sscanf(line, "%15s %127s %127s %7s %127s", tag, name, qchex, refuse_s, khex) >= 4 &&
            strcmp(tag, "VEC") == 0) {
            uint8_t qc[32], k_out[32], h_out[32], reply[512];
            int qclen = hex2bin(qchex, qc, 32);
            int refuse = atoi(refuse_s);
            int replylen = 0;
            memset(k_out, 0xA5, 32);
            memset(h_out, 0xA5, 32);
            nvec++;
            if (qclen != 32) { printf("FAIL %s: vector qc is %d bytes, not 32\n", name, qclen); failed++; continue; }

            int rc = ssh_kex_ecdh_reply_build(V_C, (int)sizeof V_C - 1, V_S, (int)sizeof V_S - 1,
                                              I_C, iclen, I_S, islen,
                                              host_pub, host_seed, qc,
                                              fixed_priv, reply, (int)sizeof reply, &replylen,
                                              k_out, h_out);
            char what[256];
            if (refuse) {
                nref++;
                snprintf(what, sizeof what, "%s: hostile point REFUSED (rc=%d)", name, rc);
                ok(rc != 0, what);
                /* And no session keys may have been written on the refused
                 * path: the caller must not be able to proceed on the
                 * attacker-chosen secret even by ignoring the return code.
                 * The buffers are pre-poisoned and must come back untouched
                 * on refusal (the all-zero return path writes neither). */
                int touched = 0;
                for (int i = 0; i < 32; i++) { touched |= (k_out[i] != 0xA5); touched |= (h_out[i] != 0xA5); }
                snprintf(what, sizeof what, "%s: k/h outputs untouched on refusal", name);
                ok(!touched, what);
            } else {
                nacc++;
                snprintf(what, sizeof what, "%s: non-canonical-but-valid point ACCEPTED (rc=%d)", name, rc);
                ok(rc == 0, what);
                if (rc == 0) {
                    uint8_t k_exp[32];
                    hex2bin(khex, k_exp, 32);
                    int same = memcmp(k_out, k_exp, 32) == 0;
                    snprintf(what, sizeof what, "%s: shared secret == independent ladder (masking correct)", name);
                    ok(same, what);
                }
            }
        } else {
            char key[64], val[256];
            if (sscanf(line, "%63s %255s", key, val) == 2 && strcmp(key, "SERVER_PRIV") == 0)
                hex2bin(val, g_server_priv, 32);
        }
    }
    fclose(f);

    printf("%d checks, %d failed  (%d vectors: %d hostile-refused, %d non-canonical-accepted)\n",
           checks, failed, nvec, nref, nacc);
    if (nvec < 20) { printf("FAIL suspiciously few vectors (%d) -- generator output missing?\n", nvec); failed++; }
    return failed ? 1 : 0;
}
