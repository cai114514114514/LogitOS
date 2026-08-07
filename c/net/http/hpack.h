#ifndef LOGIT_HPACK_H
#define LOGIT_HPACK_H

#include <stdint.h>
#include <stddef.h>

/* hpack -- HPACK header compression (RFC 7541), the part of HTTP/2 that has
 * the security bugs.
 *
 * WHY THIS IS THE DANGEROUS MODULE.  Everything else in HTTP/2 is framing:
 * a length, a type, a payload, and if you get one wrong you drop a connection.
 * HPACK is different in two ways.
 *
 *   1. IT IS A DECOMPRESSOR FED BY THE PEER.  Every string in a header block
 *      carries a length the server chose, and a Huffman-coded string expands
 *      to something LARGER than the bytes on the wire -- up to 8/5 of them,
 *      because the shortest code is 5 bits.  A decoder that reserves the
 *      encoded length and then writes the decoded bytes overflows the buffer
 *      by 60% on attacker-chosen input.  So every length here is checked
 *      against what is actually present BEFORE anything is reserved, the
 *      Huffman decoder grows its output as it emits, and both the per-string
 *      and per-block sizes are capped.
 *
 *   2. IT IS STATEFUL, AND THE STATE IS SHARED WITH THE PEER.  The dynamic
 *      table is built by both ends in lockstep: the encoder says "field 62"
 *      and means whatever its table has at 62, and if our table disagrees by
 *      one entry then every indexed header for the rest of the connection is
 *      silently the wrong header -- not a parse error, a WRONG ANSWER.  That
 *      makes the eviction discipline (below) a correctness requirement rather
 *      than a memory-management detail, and it is why a decode error is a
 *      CONNECTION error in HTTP/2: once the tables diverge there is no way to
 *      resynchronise, so the only safe move is to stop.
 *
 * THE EVICTION DISCIPLINE, exactly (RFC 7541 sections 4.1-4.4):
 *   - An entry's size is nlen + vlen + 32.  The 32 is a fixed allowance for
 *     overhead and is part of the wire contract -- shrink it and our table
 *     evicts at a different moment from the peer's.
 *   - Insertion is at the FRONT: the newest entry is dynamic index 1, which is
 *     absolute index 62.  Every older entry's index shifts up by one.
 *   - Before an insert, entries are evicted from the BACK (oldest first) until
 *     the new entry fits.  Eviction happens BEFORE the insert, never after.
 *   - An entry larger than the whole table capacity is not an error: the table
 *     is emptied and the entry is simply not stored (4.4).  A decoder that
 *     errors here rejects legal streams.
 *   - A dynamic table size update sets a new capacity and evicts down to it
 *     immediately.  A capacity above the one we announced in SETTINGS is a
 *     decode error (4.2) -- that is the only bound on how much memory a peer
 *     can make us hold.
 *   - The subtle one: a literal-with-incremental-indexing may name its field
 *     by an index that eviction is about to destroy, and its value may be the
 *     entry that gets evicted to make room for itself.  So name and value are
 *     COPIED before any eviction runs.  Doing it the other way round is a
 *     use-after-free that only fires when the table happens to be full.
 *
 * The encoder keeps the mirror-image table so that what we index is what the
 * peer will look up.  It is deliberately conservative: it indexes fields it
 * has already sent, but sends anything named as sensitive (authorization,
 * cookie, set-cookie, proxy-authorization) as never-indexed literals so a
 * secret never enters a compression context shared with an attacker-influenced
 * request -- that is CRIME, and it is why the "never indexed" representation
 * exists at all.
 */

/* ---- errors ---------------------------------------------------------- */

enum {
    HPACK_OK        =  0,
    HPACK_E_TRUNC   = -1,   /* the block ended in the middle of something */
    HPACK_E_INDEX   = -2,   /* index 0, or past the end of the tables */
    HPACK_E_HUFF    = -3,   /* bad padding, EOS in the stream, or a dangling code */
    HPACK_E_INT     = -4,   /* integer overflowed, or used too many continuation octets */
    HPACK_E_SIZE    = -5,   /* a string or the header list exceeded its cap */
    HPACK_E_NOMEM   = -6,
    HPACK_E_UPDATE  = -7,   /* size update too large, or not at the block start */
    HPACK_E_ARG     = -8,
    HPACK_E_NUL     = -9    /* an embedded NUL in a name or value; see hpack.c */
};

const char *hpack_strerror(int err);

/* ---- bounds ---------------------------------------------------------- */

/* The largest dynamic table capacity we will ever accept from a peer. We
 * announce 4096 (the protocol default) in SETTINGS, so a compliant peer never
 * asks for more; this is the ceiling that makes a NON-compliant one bounded
 * rather than a memory-exhaustion vector. */
#define HPACK_CAP_MAX      16384
#define HPACK_DEFAULT_CAP   4096
/* Each entry costs at least 32 bytes, so this is the most entries any legal
 * capacity can hold. The table is a fixed ring: no allocation for the spine. */
#define HPACK_MAX_ENTRIES  (HPACK_CAP_MAX / 32 + 1)

#define HPACK_STR_MAX      (16 * 1024)   /* one name or one value, decoded */
#define HPACK_LIST_MAX     (32 * 1024)   /* whole decoded header list */
#define HPACK_MAX_FIELDS   256
#define HPACK_STATIC_N     61

/* ---- header list ----------------------------------------------------- */

struct hpack_hdr {
    char *name, *value;         /* NUL-terminated, owned by the list */
    int   nlen, vlen;
    int   sensitive;            /* arrived as, or must be sent as, never-indexed */
};

struct hpack_list {
    struct hpack_hdr *v;
    int n, cap;
    int bytes;                  /* running sum of nlen+vlen+32, the RFC's metric */
};

void        hpack_list_init(struct hpack_list *l);
void        hpack_list_free(struct hpack_list *l);
/* Copies both strings. nlen/vlen may be -1 for NUL-terminated. */
int         hpack_list_add(struct hpack_list *l, const char *name, int nlen,
                           const char *value, int vlen, int sensitive);
/* First match, case-sensitive: HTTP/2 field names are lowercase by definition
 * and a name that is not lowercase is a malformed message, not a variant. */
const char *hpack_list_get(const struct hpack_list *l, const char *name);
int         hpack_list_count(const struct hpack_list *l, const char *name);
const char *hpack_list_nth(const struct hpack_list *l, const char *name, int idx);

/* ---- table ----------------------------------------------------------- */

struct hpack_entry { char *name, *value; int nlen, vlen; };

struct hpack_table {
    struct hpack_entry v[HPACK_MAX_ENTRIES];
    int head;                   /* ring index of the NEWEST entry */
    int count;
    int size;                   /* sum of entry sizes, RFC metric */
    int cap;                    /* current capacity */
    int cap_max;                /* the largest capacity we allow (announced) */
};

void hpack_table_init(struct hpack_table *t, int cap_max);
void hpack_table_free(struct hpack_table *t);
/* Set the capacity, evicting down to it. Rejects cap > cap_max. */
int  hpack_table_resize(struct hpack_table *t, int cap);
/* Insert at the front, evicting from the back first. Copies the strings BEFORE
 * evicting -- see the header comment. Returns HPACK_OK; an entry too big for
 * the table empties it and is not stored, which is not an error. */
int  hpack_table_add(struct hpack_table *t, const char *name, int nlen,
                     const char *value, int vlen);
int  hpack_table_count(const struct hpack_table *t);
int  hpack_table_size(const struct hpack_table *t);
/* Absolute index (1-based, static then dynamic). NULL if out of range. */
const struct hpack_entry *hpack_lookup(const struct hpack_table *t, int idx);
/* Static table, for tests and for the encoder. idx is 1..61. */
const struct hpack_entry *hpack_static(int idx);

/* ---- decoder --------------------------------------------------------- */

struct hpack_dec {
    struct hpack_table tab;
    int list_max;               /* cap on one decoded header list */
    int blocks;                 /* header blocks decoded, for diagnostics */
};

void hpack_dec_init(struct hpack_dec *d, int cap_max);
void hpack_dec_free(struct hpack_dec *d);
/* Decode ONE COMPLETE header block (HEADERS + all its CONTINUATIONs already
 * concatenated -- a header block is not resumable, which is exactly why RFC
 * 7540 forbids interleaving anything between the fragments). Appends to `out`.
 *
 * On any negative return the decoder's table is left as it was when the error
 * was hit; the caller MUST NOT reuse the connection, because the tables have
 * diverged. That is what H2_ERR_COMPRESSION_ERROR is for. */
int  hpack_decode(struct hpack_dec *d, const uint8_t *in, int len,
                  struct hpack_list *out);

/* ---- encoder --------------------------------------------------------- */

struct hpack_enc {
    struct hpack_table tab;
    int  pending_cap;           /* >= 0: emit a size update before the next block */
    int  huffman;               /* 1 = Huffman-code literals when it helps */
};

void hpack_enc_init(struct hpack_enc *e, int cap_max);
void hpack_enc_free(struct hpack_enc *e);
/* The peer's SETTINGS_HEADER_TABLE_SIZE changed: remember it and emit the size
 * update at the start of the next block, which is the only place RFC 7541
 * allows one. */
void hpack_enc_set_capacity(struct hpack_enc *e, int cap);
/* Encode a header list into a malloc'd buffer the caller frees. */
int  hpack_encode(struct hpack_enc *e, const struct hpack_list *in,
                  uint8_t **out, int *outlen);

/* ---- primitives (exposed so the fuzzer and the tests can aim at them) -- */

/* RFC 7541 5.1. Reads a prefix-coded integer. *off advances. `prefix` is the
 * number of value bits in the first octet (1..8). */
int hpack_int_decode(const uint8_t *in, int len, int *off, int prefix, uint32_t *out);
/* Appends the prefix-coded integer; `flags` supplies the high bits of the
 * first octet. Returns bytes written, or < 0. */
int hpack_int_encode(uint8_t *out, int max, int prefix, uint8_t flags, uint32_t val);
/* Huffman. decode() allocates; encode() writes into `out` and returns the
 * length, or -1 if it would not fit. */
int hpack_huff_decode(const uint8_t *in, int len, char **out, int *outlen, int cap);
int hpack_huff_len(const char *s, int n);
int hpack_huff_encode(uint8_t *out, int max, const char *s, int n);

#endif /* LOGIT_HPACK_H */
