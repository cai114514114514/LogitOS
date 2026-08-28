#ifndef LOGIT_HASH_BLAKE3_H
#define LOGIT_HASH_BLAKE3_H

#include <stdint.h>
#include <stddef.h>

/* BLAKE3 (the team's own spec, v1.0: https://github.com/BLAKE3-team/BLAKE3-specs).
 *
 * This has NO caller in this tree today -- see CLAUDE.md's category (b) and
 * the rule this file was built under: a primitive with a real known-answer
 * gate claims nothing to anybody, and breadth is the point of a crypto
 * library. THIS FILE MUST NOT BE REACHED FROM c/crypto/trust, TLS, aex.c's
 * package check, or the login path. Wiring is a separate decision with its
 * own argument; this is not that decision.
 *
 * Three modes share one tree structure, distinguished only by which flags and
 * initial chaining value the top-level hasher starts with:
 *   hash          key_words = IV,               flags = 0
 *   keyed_hash    key_words = the 32-byte key,   flags = KEYED_HASH
 *   derive_key    key_words = derived from the context string (itself a
 *                 hash of the context under DERIVE_KEY_CONTEXT), flags =
 *                 DERIVE_KEY_MATERIAL
 *
 * Output is an XOF: blake3_finalize_seek can produce any number of bytes
 * starting at any byte offset, by re-running the ROOT compression of the
 * (fixed) root node with an incrementing 64-bit output-block counter. That
 * counter is completely independent of the chunk counter used while hashing
 * the input -- see the block comment above root_output_bytes() in the .c for
 * the trap this causes if the two get confused.
 */

#define BLAKE3_KEY_LEN   32
#define BLAKE3_OUT_LEN   32   /* default (non-extended) output length */
#define BLAKE3_BLOCK_LEN 64
#define BLAKE3_CHUNK_LEN 1024

/* 54 entries is not a round number: 2^54 chunks * 1024 bytes/chunk = 2^64
 * bytes, the whole 64-bit input-length space, so 54 is provably enough and
 * one entry is never wasted margin. Matches the reference implementation's
 * own sizing argument (reference_impl.rs, `cv_stack: [[u32; 8]; 54]`). */
#define BLAKE3_MAX_STACK 54

struct blake3_chunk_state {
    uint32_t cv[8];              /* chaining value carried across this chunk's blocks */
    uint64_t chunk_counter;      /* CHUNK index (not block index -- see .c) */
    uint8_t  block[BLAKE3_BLOCK_LEN];
    uint8_t  block_len;          /* bytes currently buffered, 0..64 */
    uint8_t  blocks_compressed;  /* full blocks compressed so far in this chunk, 0..15 */
};

struct blake3_hasher {
    struct blake3_chunk_state chunk;
    uint32_t key_words[8];       /* IV, the keyed_hash key, or the derived key */
    uint32_t flags;              /* 0, BLAKE3_KEYED_HASH, or BLAKE3_DERIVE_KEY_MATERIAL */
    uint32_t cv_stack[BLAKE3_MAX_STACK][8];
    uint8_t  cv_stack_len;
};

void blake3_init(struct blake3_hasher *h);
void blake3_init_keyed(struct blake3_hasher *h, const uint8_t key[BLAKE3_KEY_LEN]);
/* context is hardcoded, globally-unique, application-specific bytes -- NOT a
 * salt the caller can vary per call. See the .c for why a single extra hash
 * step here is what makes the derived key domain-separated from every other
 * caller of derive_key on this machine. */
void blake3_init_derive_key(struct blake3_hasher *h, const void *context, size_t context_len);

void blake3_update(struct blake3_hasher *h, const void *input, size_t len);

/* Default 32-byte output. */
void blake3_finalize(const struct blake3_hasher *h, uint8_t out[BLAKE3_OUT_LEN]);

/* Extended (XOF) output: out_len bytes starting at byte offset `seek` of the
 * conceptually-infinite output stream. blake3_finalize(h, out) is exactly
 * blake3_finalize_seek(h, 0, out, 32). Does not mutate *h -- finalize can be
 * called repeatedly (with different seeks) on the same hasher, and update()
 * can keep being called before it, exactly as in the reference. */
void blake3_finalize_seek(const struct blake3_hasher *h, uint64_t seek,
                          uint8_t *out, size_t out_len);

#endif
