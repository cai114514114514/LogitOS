/* c/net/ssh/ssh_wire.c: the RFC 4251 5 primitives every other file in
 * c/net/ssh is built on. mpint's edge cases matter more than the rest of
 * this file put together -- it is the ONE encoding shared between the
 * exchange hash and the KDF (see ssh_kex.h), so a bug here would move H and
 * every derived key together and never show up as a "keys disagree" defect,
 * only as "nothing this server does interops", which is a much harder
 * failure to place. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ssh_wire.h"

static int checks, failed;
static void ok(int cond, const char *what)
{
    checks++;
    if (cond) printf("ok   %s\n", what);
    else { printf("FAIL %s\n", what); failed++; }
}

static void check_mpint(const uint8_t *raw, int rawlen, const uint8_t *want, int wantlen, const char *label)
{
    uint8_t buf[64];
    int n = ssh_w_mpint(buf, 0, sizeof buf, raw, rawlen);
    char msg[160];
    snprintf(msg, sizeof msg, "mpint(%s): length %d matches expected %d", label, n, 4 + wantlen);
    ok(n == 4 + wantlen, msg);
    uint32_t declared;
    ssh_r_u32(buf, 0, n, &declared);
    snprintf(msg, sizeof msg, "mpint(%s): declared length field matches the body", label);
    ok((int)declared == wantlen, msg);
    snprintf(msg, sizeof msg, "mpint(%s): body bytes match RFC 4251's minimal encoding", label);
    ok(n == 4 + wantlen && memcmp(buf + 4, want, (size_t)wantlen) == 0, msg);
}

int main(void)
{
    /* RFC 4251 5, the mpint examples given IN the RFC, verbatim: */
    { uint8_t v[] = { 0x00 }; check_mpint(v, 0, (uint8_t*)"", 0, "0 (empty input)"); }
    { uint8_t v[] = { 0x09, 0xa3, 0x78, 0xf9, 0xb2, 0xe3, 0x32, 0xa7 };
      check_mpint(v, sizeof v, v, sizeof v, "0x9a378f9b2e332a7 (no pad needed)"); }
    { uint8_t v[] = { 0x80 }; uint8_t want[] = { 0x00, 0x80 };
      check_mpint(v, sizeof v, want, sizeof want, "0x80 (high bit set -> padded)"); }
    { uint8_t v[] = { 0x00, 0x00, 0x00, 0x2a }; uint8_t want[] = { 0x2a };
      check_mpint(v, sizeof v, want, sizeof want, "leading zero bytes stripped"); }
    { uint8_t v[32] = {0}; v[0] = 0; /* all zero, longer than one byte */
      check_mpint(v, sizeof v, (uint8_t*)"", 0, "all-zero input of any length -> empty string"); }
    { uint8_t v[32]; for (int i = 0; i < 32; i++) v[i] = 0xFF; /* X25519-shaped: high bit set */
      uint8_t want[33]; want[0] = 0; memcpy(want + 1, v, 32);
      check_mpint(v, 32, want, 33, "a 32-byte all-0xFF shared secret (the X25519 shape)"); }

    /* string round-trip, incl. zero-length and near-buffer-edge. */
    {
        uint8_t buf[64];
        int n = ssh_w_string(buf, 0, sizeof buf, (const uint8_t *)"hi", 2);
        ok(n == 6, "string: 4-byte length + 2 bytes");
        const uint8_t *p; int plen;
        int r = ssh_r_string(buf, 0, n, &p, &plen);
        ok(r == n && plen == 2 && memcmp(p, "hi", 2) == 0, "string: round trip");
    }
    {
        /* A string claiming to be longer than the buffer: refused, not
         * read past the end. This is the field every message in c/net/ssh
         * decodes an attacker-controlled length through. */
        uint8_t buf[8] = { 0, 0, 0, 100, 'a', 'b', 'c', 'd' };
        const uint8_t *p; int plen;
        int r = ssh_r_string(buf, 0, sizeof buf, &p, &plen);
        ok(r < 0, "string: an oversized declared length is REFUSED, not trusted");
    }

    /* namelist token-boundary matching: a real regression risk given the
     * captured client's OWN list has this exact shape (see ssh.h's
     * captured KEXINIT: "ssh-ed25519-cert-v01@openssh.com,...,ssh-ed25519,..."). */
    {
        const char *list = "ssh-ed25519-cert-v01@openssh.com,ecdsa-sha2-nistp256,ssh-ed25519";
        ok(ssh_namelist_has((const uint8_t *)list, (int)strlen(list), "ssh-ed25519") == 1,
           "namelist_has: finds ssh-ed25519 as its OWN token, not as a substring of the cert variant");
        ok(ssh_namelist_has((const uint8_t *)list, (int)strlen(list), "ssh-ed25519-cert-v01@openssh.com") == 1,
           "namelist_has: also finds the longer name that happens to start the same way");
        ok(ssh_namelist_has((const uint8_t *)list, (int)strlen(list), "rsa-sha2-256") == 0,
           "namelist_has: absent name is absent");
        ok(ssh_namelist_has((const uint8_t *)list, (int)strlen(list), "ssh-ed") == 0,
           "namelist_has: a PREFIX of a real token does not match");
    }

    /* RFC 4253 7.1 negotiation order: the CLIENT's preference wins, not
     * ours -- this is the exact rule that makes ssh.h's captured KEXINIT
     * (mlkem768.../sntrup761.../curve25519-sha256/...) resolve to
     * curve25519-sha256 rather than something earlier in OUR list. */
    {
        const char *client = "mlkem768x25519-sha256,sntrup761x25519-sha512,curve25519-sha256@libssh.org,curve25519-sha256";
        char out[64];
        int n = ssh_negotiate((const uint8_t *)client, (int)strlen(client),
                              "curve25519-sha256,curve25519-sha256@libssh.org", out, sizeof out);
        ok(n > 0 && strcmp(out, "curve25519-sha256@libssh.org") == 0,
           "negotiate: client's FIRST mutually-supported name wins (@libssh.org, not the plain name after it)");
    }
    {
        const char *client = "diffie-hellman-group14-sha256,diffie-hellman-group1-sha1";
        char out[64];
        int n = ssh_negotiate((const uint8_t *)client, (int)strlen(client), "curve25519-sha256", out, sizeof out);
        ok(n < 0, "negotiate: no common algorithm -> refused, not a silent default");
    }

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
