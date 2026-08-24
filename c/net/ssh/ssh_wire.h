#ifndef LOGIT_SSH_WIRE_H
#define LOGIT_SSH_WIRE_H
#include <stdint.h>

/* The SSH wire primitives (RFC 4251 5): byte, boolean, uint32, string,
 * mpint, name-list. Every one of these is bounds-checked against a buffer
 * length the caller supplies, because every byte through here started on a
 * socket -- a malformed length must fail closed, not read past `buf`. */

/* --- writers: return the new offset. Never overrun `max`; a writer that
 * would returns -1 and writes nothing further (caller must check). --- */
int ssh_w_u8(uint8_t *buf, int off, int max, uint8_t v);
int ssh_w_u32(uint8_t *buf, int off, int max, uint32_t v);
int ssh_w_bool(uint8_t *buf, int off, int max, int v);
int ssh_w_bytes(uint8_t *buf, int off, int max, const uint8_t *data, int len);
/* string = uint32 length + raw bytes (RFC 4251 5) */
int ssh_w_string(uint8_t *buf, int off, int max, const uint8_t *data, int len);
int ssh_w_cstring(uint8_t *buf, int off, int max, const char *s);
/* mpint (RFC 4251 5): `raw`/`rawlen` is a big-endian unsigned magnitude
 * (leading zero bytes allowed on input and stripped here); writes the
 * MINIMAL two's-complement-positive encoding -- a 0x00 is prepended only if
 * the first remaining byte's high bit is set, and an all-zero input encodes
 * as length 0. This is the ONE encoding used both inside the exchange hash
 * and inside the key-derivation HASH(K || H || letter || session_id), and it
 * must be byte-identical in both places or H and the derived keys silently
 * stop agreeing with a real client's. */
int ssh_w_mpint(uint8_t *buf, int off, int max, const uint8_t *raw, int rawlen);
/* name-list = string, where the bytes are a comma-separated ASCII list */
int ssh_w_namelist(uint8_t *buf, int off, int max, const char *csv);

/* --- readers: return the new offset, or -1 on a truncated/malformed field.
 * String/name-list readers hand back a POINTER INTO `buf` (no copy) plus a
 * length -- the caller's buffer must outlive the pointer. --- */
int ssh_r_u8(const uint8_t *buf, int off, int len, uint8_t *out);
int ssh_r_u32(const uint8_t *buf, int off, int len, uint32_t *out);
int ssh_r_bool(const uint8_t *buf, int off, int len, int *out);
int ssh_r_string(const uint8_t *buf, int off, int len, const uint8_t **data, int *dlen);
/* Copies into a NUL-terminated caller buffer, truncating (and reporting the
 * real length via *dlen) rather than overrunning -- used for the handful of
 * fields (username, algorithm names) a caller wants as a C string. */
int ssh_r_string_cpy(const uint8_t *buf, int off, int len, char *out, int outmax, int *dlen);

/* 1 if `name` (NUL-terminated) appears as a whole comma-separated token in
 * `list`/`listlen` (NOT a substring match -- "ssh-ed25519" must not match
 * inside a hypothetical "ssh-ed25519-cert-v01@openssh.com"). */
int ssh_namelist_has(const uint8_t *list, int listlen, const char *name);

/* RFC 4253 7.1 negotiation: walk CLIENT's list in ITS preference order; the
 * first name also present in `server_csv` (our own comma list) wins. Writes
 * the chosen name (NUL-terminated) into `out` and returns its length, or -1
 * if no name is common to both. */
int ssh_negotiate(const uint8_t *client_list, int client_len,
                   const char *server_csv, char *out, int outmax);

#endif /* LOGIT_SSH_WIRE_H */
