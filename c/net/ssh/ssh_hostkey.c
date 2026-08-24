#include "ssh_hostkey.h"
#include "ssh_wire.h"
#include "base64.h"
#include "crypto.h"

static const uint8_t MAGIC[4] = { 'L', 'K', 'H', '1' };

int ssh_hostkey_encode(const uint8_t seed[32], const uint8_t pub[32], uint8_t out[SSH_HOSTKEY_RECORD_LEN])
{
    for (int i = 0; i < 4; i++) out[i] = MAGIC[i];
    for (int i = 0; i < 32; i++) out[4 + i] = seed[i];
    for (int i = 0; i < 32; i++) out[36 + i] = pub[i];
    return 0;
}

int ssh_hostkey_decode(const uint8_t *buf, int len, uint8_t seed[32], uint8_t pub[32])
{
    if (len != SSH_HOSTKEY_RECORD_LEN) return -1;
    for (int i = 0; i < 4; i++) if (buf[i] != MAGIC[i]) return -1;
    for (int i = 0; i < 32; i++) seed[i] = buf[4 + i];
    for (int i = 0; i < 32; i++) pub[i] = buf[36 + i];
    return 0;
}

int ssh_hostkey_blob(const uint8_t pub[32], uint8_t out[4 + 11 + 4 + 32])
{
    int off = ssh_w_cstring(out, 0, 4 + 11 + 4 + 32, "ssh-ed25519");
    off = ssh_w_string(out, off, 4 + 11 + 4 + 32, pub, 32);
    return off;
}

int ssh_hostkey_fingerprint(const uint8_t pub[32], char *out, int outmax)
{
    uint8_t blob[4 + 11 + 4 + 32];
    int blen = ssh_hostkey_blob(pub, blob);
    if (blen < 0) return -1;
    uint8_t digest[32];
    sha256(blob, (unsigned long)blen, digest);

    if (outmax < 7 + 43 + 1) return -1;
    const char *prefix = "SHA256:";
    int k = 0;
    for (int i = 0; prefix[i]; i++) out[k++] = prefix[i];
    int n = b64_encode(digest, 32, out + k, outmax - k, 0 /* unpadded */);
    if (n < 0) return -1;
    k += n;
    out[k] = 0;
    return k;
}
