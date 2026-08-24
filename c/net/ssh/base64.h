#ifndef LOGIT_SSH_BASE64_H
#define LOGIT_SSH_BASE64_H
#include <stdint.h>

/* RFC 4648 base64 -- the encoding OpenSSH uses for public-key blobs in
 * authorized_keys (padded) and for a SHA256 host-key fingerprint (unpadded,
 * matching `ssh-keygen -l`'s default format). Two small functions, not a
 * general codec: no line wrapping, no whitespace tolerance beyond what
 * `ssh_authkeys_*` strips itself. */

/* Writes ceil(len/3)*4 bytes (plus '=' padding if `pad`), NOT NUL-terminated
 * by itself -- callers that want a C string add one. Returns bytes written,
 * or -1 if `out`/`outmax` is too small. */
int b64_encode(const uint8_t *data, int len, char *out, int outmax, int pad);

/* Decodes `in`/`inlen` (padding optional -- both forms accepted, since a
 * pasted authorized_keys line always has it and our own fingerprint output
 * never does). Returns bytes written, or -1 on any character outside the
 * base64 alphabet (whitespace is NOT allowed here; the caller trims first)
 * or if `out` is too small. */
int b64_decode(const char *in, int inlen, uint8_t *out, int outmax);

#endif /* LOGIT_SSH_BASE64_H */
