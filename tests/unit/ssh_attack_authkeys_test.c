/* authorized_keys parsing under hostile input, and the truncation edges of
 * the auth parsers -- pinned as DESIRED behavior.
 *
 * Three checks below are RED until their fixes land (the run that watched
 * them fail is the point -- AGENTS.md rule 5):
 *   - a password longer than the parse buffer must be REFUSED, not silently
 *     truncated: today a 200-byte password copies its first 127 bytes and
 *     authenticates against an account whose whole password IS those bytes,
 *     i.e. auth under a string the user never chose;
 *   - a username longer than struct ssh_authreq's user[64] must be REFUSED,
 *     not truncated: two wire usernames sharing a 63-byte prefix map to the
 *     same account, which is identity confusion (narrow, but real: the
 *     attacker picks the suffix);
 *   - an authorized_keys file with CRLF line endings must still match its
 *     keys: today the base64-field scan stops only at ' '/'\t', so the \r
 *     rides into b64_decode, which rejects it, and the line is skipped --
 *     a whole Windows-authored file parses to zero keys (fail-closed, an
 *     interop defect rather than a hole, found by the hostile-surroundings
 *     case below failing while the plain line passed).
 * The rest are green today and guard the authorized_keys walk: garbage must
 * be skipped without a crash, a single flipped base64 character must turn a
 * match into a non-match (a wrong accept is an auth bypass), and a genuine
 * key must still be found in a sea of hostile lines. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "ssh.h"
#include "ssh_wire.h"
#include "ssh_auth.h"
#include "base64.h"

static int checks, failed;
static void ok(int cond, const char *what)
{
    checks++;
    if (cond) printf("ok   %s\n", what);
    else { printf("FAIL %s\n", what); failed++; }
}

/* A real-shaped ed25519 blob: string "ssh-ed25519" + string 32 bytes. */
static uint8_t g_blob[128]; static int g_bloblen;
static void build_blob(void)
{
    int o = ssh_w_cstring(g_blob, 0, (int)sizeof g_blob, "ssh-ed25519");
    for (int i = 0; i < 32; i++) g_blob[4 + 11 + 4 + i] = (uint8_t)(i * 7 + 1);
    g_bloblen = o + 4 + 32;
}

/* Separate buffers per call -- a static here made `good` and `bad` aliases
 * of one array, so every "must not match" passed and every "must match"
 * failed, the exact inverted picture of the truth (found the hard way). */
static int b64_of(const uint8_t *b, int bl, char *out, int outmax)
{
    int n = b64_encode(b, bl, out, outmax - 1, 1);
    out[n > 0 ? n : 0] = 0;
    return n > 0 ? n : 0;
}

int main(void)
{
    build_blob();
    char good[256], bad[256];
    b64_of(g_blob, g_bloblen, good, (int)sizeof good);

    /* a blob differing in ONE byte, for the flipped-line cases */
    uint8_t bad_blob[128]; memcpy(bad_blob, g_blob, (size_t)g_bloblen);
    bad_blob[4 + 11 + 4 + 5] ^= 0x40;
    b64_of(bad_blob, g_bloblen, bad, (int)sizeof bad);

    char text[4096];

    /* --- hostile authorized_keys corpora --- */
    ok(ssh_authkeys_match("", 0, g_blob, g_bloblen) == 0, "empty file: no match, no crash");
    ok(ssh_authkeys_match("\n\n\n  \t \n", 8, g_blob, g_bloblen) == 0, "blank lines only");
    ok(ssh_authkeys_match("# ssh-ed25519 AAAA comment\n", 28, g_blob, g_bloblen) == 0,
       "comment line that would otherwise match shape: skipped");
    ok(ssh_authkeys_match("ssh-ed25519\n", 12, g_blob, g_bloblen) == 0, "type with no key");
    ok(ssh_authkeys_match("ssh-ed25519 \n", 13, g_blob, g_bloblen) == 0, "type + space + newline");
    ok(ssh_authkeys_match("ssh-ed25519 !!!!not-base64!!!!\n", 31, g_blob, g_bloblen) == 0,
       "invalid base64 alphabet: skipped, not crashed");
    ok(ssh_authkeys_match("ssh-ed25519 QUJDREVG", 20, g_blob, g_bloblen) == 0,
       "truncated base64: no accidental match");
    ok(ssh_authkeys_match("ssh-ed25519 ", 12, g_blob, g_bloblen) == 0, "prefix only, no newline at EOF");

    snprintf(text, sizeof text, "ssh-rsa %s c1\n", good);
    ok(ssh_authkeys_match(text, (int)strlen(text), g_blob, g_bloblen) == 0,
       "right base64 under the WRONG type: must not match");

    snprintf(text, sizeof text, "ssh-ed25519 %s\n", bad);
    ok(ssh_authkeys_match(text, (int)strlen(text), g_blob, g_bloblen) == 0,
       "ONE flipped payload byte: must NOT match (a wrong accept is a bypass)");

    snprintf(text, sizeof text,
             "\n"
             "  # trap comment\n"
             "garbage line with no tab\n"
             "ssh-ed25519 %s first@host\n"
             "ssh-ed25519 %s\r\n"
             "\tssh-ed25519 %s\t after-tab\n",
             bad, good, bad);
    ok(ssh_authkeys_match(text, (int)strlen(text), g_blob, g_bloblen) == 1,
       "genuine key found among hostile lines (CRLF, tabs, no-final-newline)");

    snprintf(text, sizeof text, "ssh-ed25519 %s c1\nssh-ed25519 %s c2\n", good, good);
    ok(ssh_authkeys_match(text, (int)strlen(text), g_blob, g_bloblen) == 1,
       "duplicate entries: still one match");

    { /* a >4 KB single line: the decoded[79] walker must not read past the
       * LINE it is on, whatever the line length */
        static char big[8192];
        int n = snprintf(big, sizeof big, "ssh-ed25519 ");
        for (int i = 0; n < (int)sizeof big - 2; i++) big[n++] = (char)('A' + (i % 26));
        big[n++] = '\n'; big[n] = 0;
        ok(ssh_authkeys_match(big, n, g_blob, g_bloblen) == 0,
           "8 KB single line of base64-ish junk: no match, no crash");
    }

    /* --- the truncation edges (RED until the fixes land) --- */
    {
        uint8_t rest[512];
        int o = 0;
        o = ssh_w_bool(rest, o, (int)sizeof rest, 0);
        char longpw[300];
        for (int i = 0; i < 260; i++) longpw[i] = 'a';
        longpw[260] = 0;
        o = ssh_w_cstring(rest, o, (int)sizeof rest, longpw);
        char pw[128];
        int pl = ssh_auth_parse_password(rest, o, pw, (int)sizeof pw);
        ok(pl < 0, "password longer than the buffer is REFUSED, not truncated "
                   "(RED until the fix: prefix-auth under a string the user never chose)");
    }
    {
        uint8_t payload[512];
        int o = ssh_w_u8(payload, 0, (int)sizeof payload, SSH_MSG_USERAUTH_REQUEST);
        char longuser[100];
        for (int i = 0; i < 90; i++) longuser[i] = 'u';
        longuser[90] = 0;
        o = ssh_w_cstring(payload, o, (int)sizeof payload, longuser);
        o = ssh_w_cstring(payload, o, (int)sizeof payload, "ssh-connection");
        o = ssh_w_cstring(payload, o, (int)sizeof payload, "none");
        struct ssh_authreq ar;
        int rc = ssh_authreq_parse(payload, o, &ar);
        ok(rc < 0, "username longer than user[64] is REFUSED, not truncated "
                   "(RED until the fix: two names sharing a 63-byte prefix are one account)");
    }

    /* --- control: well-formed short inputs still parse --- */
    {
        uint8_t payload[256];
        int o = ssh_w_u8(payload, 0, (int)sizeof payload, SSH_MSG_USERAUTH_REQUEST);
        o = ssh_w_cstring(payload, o, (int)sizeof payload, "alice");
        o = ssh_w_cstring(payload, o, (int)sizeof payload, "ssh-connection");
        o = ssh_w_cstring(payload, o, (int)sizeof payload, "password");
        o = ssh_w_bool(payload, o, (int)sizeof payload, 0);
        o = ssh_w_cstring(payload, o, (int)sizeof payload, "hunter2");
        struct ssh_authreq ar;
        ok(ssh_authreq_parse(payload, o, &ar) == 0 && strcmp(ar.user, "alice") == 0,
           "control: a normal request still parses");
        char pw[128];
        ok(ssh_auth_parse_password(ar.rest, ar.restlen, pw, (int)sizeof pw) == 7,
           "control: a normal password still parses to its exact length");
    }

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
