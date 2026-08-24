#include "ssh_auth.h"
#include "ssh_wire.h"
#include "ssh.h"
#include "base64.h"

int ssh_authreq_parse(const uint8_t *payload, int len, struct ssh_authreq *out)
{
    if (len < 1 || payload[0] != SSH_MSG_USERAUTH_REQUEST) return -1;
    int off = 1;
    off = ssh_r_string_cpy(payload, off, len, out->user, (int)sizeof out->user, 0);
    if (off < 0) return -1;
    off = ssh_r_string_cpy(payload, off, len, out->service, (int)sizeof out->service, 0);
    if (off < 0) return -1;
    off = ssh_r_string_cpy(payload, off, len, out->method, (int)sizeof out->method, 0);
    if (off < 0) return -1;
    out->rest = payload + off;
    out->restlen = len - off;
    return 0;
}

int ssh_auth_parse_password(const uint8_t *rest, int restlen, char *pw, int pwmax)
{
    int change;
    int off = ssh_r_bool(rest, 0, restlen, &change);
    if (off < 0) return -1;
    if (change) return -1; /* CHANGE-REQUEST, not a plain login -- not implemented */
    int dlen;
    off = ssh_r_string_cpy(rest, off, restlen, pw, pwmax, &dlen);
    if (off < 0) return -1;
    return dlen;
}

int ssh_auth_parse_publickey(const uint8_t *rest, int restlen,
                             int *has_sig, char *alg, int algmax,
                             const uint8_t **blob, int *bloblen,
                             const uint8_t **sig, int *siglen)
{
    int off = ssh_r_bool(rest, 0, restlen, has_sig);
    if (off < 0) return -1;
    off = ssh_r_string_cpy(rest, off, restlen, alg, algmax, 0);
    if (off < 0) return -1;
    off = ssh_r_string(rest, off, restlen, blob, bloblen);
    if (off < 0) return -1;
    if (*has_sig) {
        off = ssh_r_string(rest, off, restlen, sig, siglen);
        if (off < 0) return -1;
    } else {
        *sig = 0; *siglen = 0;
    }
    return 0;
}

int ssh_authkeys_match(const char *authkeys_text, int len,
                       const uint8_t *blob, int bloblen)
{
    const char *prefix = "ssh-ed25519 ";
    int plen = 12;
    int i = 0;
    while (i < len) {
        int start = i;
        while (i < len && authkeys_text[i] != '\n') i++;
        int linelen = i - start;
        const char *line = authkeys_text + start;
        int j = 0;
        while (j < linelen && (line[j] == ' ' || line[j] == '\t')) j++;
        if (i < len) i++; /* skip the '\n' for next round */

        if (linelen - j < plen || line[j] == '#') continue;
        int match_prefix = 1;
        for (int k = 0; k < plen; k++) if (line[j + k] != prefix[k]) { match_prefix = 0; break; }
        if (!match_prefix) continue;

        int b64start = j + plen;
        int b64end = b64start;
        while (b64end < linelen && line[b64end] != ' ' && line[b64end] != '\t') b64end++;
        int b64len = b64end - b64start;
        if (b64len <= 0) continue;

        uint8_t decoded[4 + 11 + 4 + 64]; /* room to spare; ed25519 blob is 51 */
        int dn = b64_decode(line + b64start, b64len, decoded, (int)sizeof decoded);
        if (dn == bloblen) {
            int eq = 1;
            for (int k = 0; k < dn; k++) if (decoded[k] != blob[k]) { eq = 0; break; }
            if (eq) return 1;
        }
    }
    return 0;
}

int ssh_auth_pubkey_signdata(const uint8_t session_id[32],
                             const char *user, const char *service,
                             const char *alg, const uint8_t *blob, int bloblen,
                             uint8_t *out, int outmax)
{
    int off = ssh_w_string(out, 0, outmax, session_id, 32);
    off = ssh_w_u8(out, off, outmax, SSH_MSG_USERAUTH_REQUEST);
    off = ssh_w_cstring(out, off, outmax, user);
    off = ssh_w_cstring(out, off, outmax, service);
    off = ssh_w_cstring(out, off, outmax, "publickey");
    off = ssh_w_bool(out, off, outmax, 1);
    off = ssh_w_cstring(out, off, outmax, alg);
    off = ssh_w_string(out, off, outmax, blob, bloblen);
    return off;
}

int ssh_build_service_accept(const char *name, uint8_t *out, int outmax)
{
    int off = ssh_w_u8(out, 0, outmax, SSH_MSG_SERVICE_ACCEPT);
    return ssh_w_cstring(out, off, outmax, name);
}

int ssh_build_userauth_failure(const char *methods_csv, int partial_success, uint8_t *out, int outmax)
{
    int off = ssh_w_u8(out, 0, outmax, SSH_MSG_USERAUTH_FAILURE);
    off = ssh_w_namelist(out, off, outmax, methods_csv);
    return ssh_w_bool(out, off, outmax, partial_success);
}

int ssh_build_userauth_success(uint8_t *out, int outmax)
{
    return ssh_w_u8(out, 0, outmax, SSH_MSG_USERAUTH_SUCCESS);
}

int ssh_build_userauth_pk_ok(const char *alg, const uint8_t *blob, int bloblen, uint8_t *out, int outmax)
{
    int off = ssh_w_u8(out, 0, outmax, SSH_MSG_USERAUTH_PK_OK);
    off = ssh_w_cstring(out, off, outmax, alg);
    return ssh_w_string(out, off, outmax, blob, bloblen);
}
