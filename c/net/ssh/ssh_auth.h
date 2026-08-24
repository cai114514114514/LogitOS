#ifndef LOGIT_SSH_AUTH_H
#define LOGIT_SSH_AUTH_H
#include <stdint.h>

/* RFC 4252 userauth: password against the SAME /etc/passwd store
 * c/apps/coreutils/accounts.h already parses (login.c's format, unchanged --
 * this file does not invent a second account store), and publickey
 * (ssh-ed25519 only) against an authorized_keys text buffer in OpenSSH's own
 * format, so a real user's existing `~/.ssh/id_ed25519.pub` line works
 * unedited.
 *
 * NO I/O HERE. The account-store bytes and the authorized_keys bytes are
 * READ by the caller (sshd.c does the file open/read; a host test hands in a
 * literal buffer) and handed in -- same shape as accounts.h's own
 * acct_find(text, len, ...), and the reason a host test can drive this
 * exactly like the device does. */

struct ssh_authreq {
    char user[64];
    char service[32];
    char method[32];
    const uint8_t *rest;   /* method-specific fields, pointer INTO the caller's payload buffer */
    int  restlen;
};

/* Parse an SSH_MSG_USERAUTH_REQUEST payload (msg type already consumed by
 * the caller's dispatch, but this checks it anyway). Returns 0, or -1 on a
 * malformed message. */
int ssh_authreq_parse(const uint8_t *payload, int len, struct ssh_authreq *out);

/* "password" method: `rest` is boolean(FALSE) + string(password). Copies the
 * password into `pw` (NUL-terminated, truncated at pwmax-1 -- a password
 * longer than that is refused by the length check itself failing to matter:
 * PWMAX in accounts.h-linked code is the real bound). Returns the password
 * length, or -1 if malformed or a change-password request (boolean TRUE,
 * which this server does not implement and must not silently accept as a
 * login). */
int ssh_auth_parse_password(const uint8_t *rest, int restlen, char *pw, int pwmax);

/* "publickey" method: `rest` is boolean(has_sig) + string(algorithm) +
 * string(key blob) [+ string(signature) if has_sig]. `blob`/`bloblen` and
 * `sig`/`siglen` point INTO `rest`. Returns 0, or -1 if malformed. */
int ssh_auth_parse_publickey(const uint8_t *rest, int restlen,
                             int *has_sig, char *alg, int algmax,
                             const uint8_t **blob, int *bloblen,
                             const uint8_t **sig, int *siglen);

/* 1 if `blob`/`bloblen` (the exact wire-format key blob from the request)
 * appears, base64-decoded, as the key field of any non-comment,
 * non-`ssh-ed25519`-mismatched line of `authkeys_text`/`len` (OpenSSH
 * authorized_keys format: optional leading options this parser skips over by
 * requiring the line to START with "ssh-ed25519 ", one space, base64, then
 * an optional comment). Everything else (a line with option prefixes such as
 * `command=...`, or a different key type) is skipped, not misread. */
int ssh_authkeys_match(const char *authkeys_text, int len,
                       const uint8_t *blob, int bloblen);

/* The exact bytes ssh-ed25519 publickey auth signs (RFC 4252 7):
 *   string session_id || byte SSH_MSG_USERAUTH_REQUEST || string user ||
 *   string service || string "publickey" || boolean TRUE || string alg ||
 *   string blob
 * Written into `out` (caller sizes it -- see SSH_AUTH_SIGDATA_MAX). Returns
 * the length, or -1 if it would not fit. */
#define SSH_AUTH_SIGDATA_MAX 512  /* comfortably above session_id(36)+msgtype(1)+
                                   * user(<=68)+service(<=20)+"publickey"(13)+
                                   * bool(1)+"ssh-ed25519"(15)+blob(<=59) ~= 213 */
int ssh_auth_pubkey_signdata(const uint8_t session_id[32],
                             const char *user, const char *service,
                             const char *alg, const uint8_t *blob, int bloblen,
                             uint8_t *out, int outmax);

/* --- message builders --- */
int ssh_build_service_accept(const char *name, uint8_t *out, int outmax);
int ssh_build_userauth_failure(const char *methods_csv, int partial_success, uint8_t *out, int outmax);
int ssh_build_userauth_success(uint8_t *out, int outmax);
int ssh_build_userauth_pk_ok(const char *alg, const uint8_t *blob, int bloblen, uint8_t *out, int outmax);

#endif /* LOGIT_SSH_AUTH_H */
