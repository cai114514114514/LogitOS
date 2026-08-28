#include "blake3.h"

/* BLAKE3 -- see blake3.h for the mode summary and the do-not-wire notice.
 *
 * IMPLEMENTED TO THE SPEC (BLAKE3-specs blake3.pdf section 2), not fitted to
 * the test vectors: it was written before the known-answer test was run even
 * once, per CLAUDE.md's own rule for mini-libc ("a reference, not a
 * hand-written expectation that only records what its author already
 * believed"). The reference Rust implementation
 * (BLAKE3-team/BLAKE3/reference_impl/reference_impl.rs) was read AFTER a
 * first draft to check the tree-merge bit-trick below, which is the one part
 * of the spec's prose (5.1.2) that is genuinely easy to get subtly wrong;
 * nothing here is copied from it -- the algorithm is the spec's, not that
 * file's expression of it, and any correct implementation has the same shape
 * for the same reason two correct SHA-256 implementations do.
 *
 * FOUR WAYS TO GET THIS SILENTLY WRONG, and why the code below avoids each:
 *
 * (1) THE ROOT FLAG. ROOT is set ONLY on the compression(s) that actually
 *     produce output bytes, and there is exactly one node in the whole tree
 *     that is ever asked for output bytes: the root. For a message of one
 *     chunk (<=1024 bytes) the root COINCIDES with that chunk's own final
 *     block, so it is tempting to set ROOT inside chunk_state_output()
 *     unconditionally -- and that agrees with every one-chunk vector and
 *     diverges silently the moment a message needs a second chunk, because
 *     now a NON-root chunk output is being asked to also carry ROOT. Here,
 *     ROOT is added exactly once, in blake3_finalize_seek, to whichever
 *     `struct output` the merge loop below determines is actually the root
 *     -- the chunk output directly if the stack is empty, or the final
 *     parent otherwise.
 *
 * (2) CHUNK CHAINING. CHUNK_START belongs on the chunk's first BLOCK only
 *     (blocks_compressed == 0), CHUNK_END on its last block only, and the
 *     COUNTER passed to compress() is the CHUNK index -- constant across all
 *     16 blocks of one chunk, then incremented once per chunk, never once
 *     per block. chunk_state_update() below never touches chunk_counter;
 *     only blake3_update()'s "chunk is full" branch does.
 *
 * (3) THE SUBTREE SPLIT is not computed explicitly here at all, which is
 *     exactly what makes it impossible to get the "largest power of two
 *     STRICTLY LESS THAN the chunk count" rule wrong: add_chunk_cv folds the
 *     completed chunk's CV into the stack once per trailing zero bit of the
 *     new total chunk count, which is the same recurrence as maintaining a
 *     binary counter. That is provably equivalent to the power-of-two split
 *     (spec 5.1.2) without ever naming a split point -- a balanced left/right
 *     split would need a different, wrong, merge order.
 *
 * (4) XOF. root_output_bytes/blake3_finalize_seek never chains off a
 *     previous output block; every output block is an INDEPENDENT
 *     compression of the same (fixed) root chaining value and root block,
 *     with the ROOT|whatever flags fixed and only the 64-bit
 *     output_block_counter changing. That counter starts at seek/64
 *     regardless of how many chunks the input had -- it shares no state with
 *     chunk_counter. The vectors' 131-byte extended outputs (>128 = >2
 *     blocks) are what catch a counter that starts at 1 or reuses the last
 *     chunk_counter instead of 0.
 *
 * CONSTANT TIME: nothing here branches on the CONTENT of the input, key, or
 * chaining values -- every branch is on public lengths and counters
 * (block_len, blocks_compressed, chunk_counter, cv_stack_len, out_len), which
 * is also true of the reference and of every other BLAKE3 implementation:
 * a hash function's compression schedule is not secret-dependent in the
 * first place, so there is no additional constant-time discipline to apply
 * here beyond "don't branch on the message", which the algorithm already
 * satisfies structurally. keyed_hash's key IS secret, but it only ever
 * flows into key_words as DATA (added into the compression state the same
 * way message words are), never into a branch or an array index -- so no
 * loop below is shaped by it.
 */

#define CHUNK_START           1u
#define CHUNK_END             2u
#define PARENT                4u
#define ROOT                  8u
#define KEYED_HASH            16u
#define DERIVE_KEY_CONTEXT    32u
#define DERIVE_KEY_MATERIAL   64u

static const uint32_t IV[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

static const uint8_t MSG_PERMUTATION[16] = {
    2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8
};

static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void g(uint32_t st[16], int a, int b, int c, int d, uint32_t mx, uint32_t my)
{
    st[a] = st[a] + st[b] + mx;
    st[d] = rotr32(st[d] ^ st[a], 16);
    st[c] = st[c] + st[d];
    st[b] = rotr32(st[b] ^ st[c], 12);
    st[a] = st[a] + st[b] + my;
    st[d] = rotr32(st[d] ^ st[a], 8);
    st[c] = st[c] + st[d];
    st[b] = rotr32(st[b] ^ st[c], 7);
}

static void round_fn(uint32_t st[16], const uint32_t m[16])
{
    g(st, 0, 4,  8, 12, m[0],  m[1]);
    g(st, 1, 5,  9, 13, m[2],  m[3]);
    g(st, 2, 6, 10, 14, m[4],  m[5]);
    g(st, 3, 7, 11, 15, m[6],  m[7]);
    g(st, 0, 5, 10, 15, m[8],  m[9]);
    g(st, 1, 6, 11, 12, m[10], m[11]);
    g(st, 2, 7,  8, 13, m[12], m[13]);
    g(st, 3, 4,  9, 14, m[14], m[15]);
}

static void permute(uint32_t m[16])
{
    uint32_t t[16];
    for (int i = 0; i < 16; i++) t[i] = m[MSG_PERMUTATION[i]];
    for (int i = 0; i < 16; i++) m[i] = t[i];
}

/* The one compression function every node in the tree goes through, chunk
 * blocks and parent nodes alike -- a parent node is just a compression whose
 * 16 message words are its two children's 8-word chaining values back to
 * back, block_len fixed at 64 and counter fixed at 0 (PARENT set in flags is
 * what actually distinguishes it; see parent_output below). `out` receives
 * all 16 words: callers that only want a chaining value take out[0..8]. */
static void compress(const uint32_t cv[8], const uint32_t block_words[16],
                      uint64_t counter, uint32_t block_len, uint32_t flags,
                      uint32_t out[16])
{
    uint32_t st[16] = {
        cv[0], cv[1], cv[2], cv[3], cv[4], cv[5], cv[6], cv[7],
        IV[0], IV[1], IV[2], IV[3],
        (uint32_t)counter, (uint32_t)(counter >> 32), block_len, flags,
    };
    uint32_t m[16];
    for (int i = 0; i < 16; i++) m[i] = block_words[i];

    for (int r = 0; r < 7; r++) {
        round_fn(st, m);
        if (r != 6) permute(m);
    }
    for (int i = 0; i < 8; i++) {
        st[i]     ^= st[i + 8];
        st[i + 8] ^= cv[i];
    }
    for (int i = 0; i < 16; i++) out[i] = st[i];
}

static void words_from_le_bytes(const uint8_t *b, uint32_t *w, int nwords)
{
    for (int i = 0; i < nwords; i++)
        w[i] = (uint32_t)b[4*i] | ((uint32_t)b[4*i+1] << 8) |
               ((uint32_t)b[4*i+2] << 16) | ((uint32_t)b[4*i+3] << 24);
}

static void le_bytes_from_words(const uint32_t *w, int nwords, uint8_t *b)
{
    for (int i = 0; i < nwords; i++) {
        b[4*i]   = (uint8_t)(w[i]);
        b[4*i+1] = (uint8_t)(w[i] >> 8);
        b[4*i+2] = (uint8_t)(w[i] >> 16);
        b[4*i+3] = (uint8_t)(w[i] >> 24);
    }
}

/* Describes a node whose chaining value HAS NOT been decided to be internal
 * or final yet -- chaining_value() re-runs the same inputs without ROOT for
 * an interior node; root bytes are produced by re-running them WITH ROOT and
 * a walking output-block counter. Nothing about the node's five inputs
 * (cv, block_words, counter, block_len, flags) differs between those two
 * uses -- only whether/how many times compress() is called and with what
 * output-block index. That non-duplication is what makes trap (1) above
 * structural rather than a discipline the caller has to remember. */
struct output {
    uint32_t cv[8];
    uint32_t block_words[16];
    uint64_t counter;
    uint32_t block_len;
    uint32_t flags;
};

static void output_chaining_value(const struct output *o, uint32_t cv[8])
{
    uint32_t full[16];
    compress(o->cv, o->block_words, o->counter, o->block_len, o->flags, full);
    for (int i = 0; i < 8; i++) cv[i] = full[i];
}

/* seek/out_len select a byte range of the conceptually infinite output
 * stream. Trap (4): output_block_counter starts at seek/64 and increments
 * once per 64-byte block produced -- it is NOT o->counter (the chunk
 * counter baked into *o for a leaf root, always 0 for a parent root) and
 * does not derive from it. */
static void root_output_bytes(const struct output *o, uint64_t seek,
                              uint8_t *out, size_t out_len)
{
    uint64_t block_counter = seek / BLAKE3_BLOCK_LEN;
    size_t skip = (size_t)(seek % BLAKE3_BLOCK_LEN);
    while (out_len > 0) {
        uint32_t words[16];
        compress(o->cv, o->block_words, block_counter, o->block_len,
                 o->flags | ROOT, words);
        uint8_t block_out[BLAKE3_BLOCK_LEN];
        le_bytes_from_words(words, 16, block_out);
        size_t avail = BLAKE3_BLOCK_LEN - skip;
        size_t n = avail < out_len ? avail : out_len;
        for (size_t i = 0; i < n; i++) out[i] = block_out[skip + i];
        out += n; out_len -= n; block_counter++; skip = 0;
    }
}

/* --------------------------------------------------------- chunk state --- */

static void chunk_state_init(struct blake3_chunk_state *c, const uint32_t key_words[8],
                             uint64_t chunk_counter)
{
    for (int i = 0; i < 8; i++) c->cv[i] = key_words[i];
    c->chunk_counter = chunk_counter;
    c->block_len = 0;
    c->blocks_compressed = 0;
    for (int i = 0; i < BLAKE3_BLOCK_LEN; i++) c->block[i] = 0;
}

static size_t chunk_state_len(const struct blake3_chunk_state *c)
{
    return (size_t)BLAKE3_BLOCK_LEN * c->blocks_compressed + c->block_len;
}

static uint32_t chunk_start_flag(const struct blake3_chunk_state *c)
{
    return c->blocks_compressed == 0 ? CHUNK_START : 0;
}

/* Absorbs input into the current chunk, compressing full 64-byte blocks as
 * they fill -- but ONLY when more input is still coming (the caller in
 * blake3_update never hands this more bytes than are left in the chunk, so
 * "block full AND more to absorb" here can only mean "this was not the
 * chunk's last block"). The chunk's actual final block is left buffered for
 * chunk_state_output() to compress WITH CHUNK_END, which is trap (2): the
 * counter used for every one of these mid-chunk compressions is
 * c->chunk_counter, read but never written here. */
static void chunk_state_update(struct blake3_chunk_state *c, uint32_t flags,
                               const uint8_t *in, size_t len)
{
    while (len > 0) {
        if (c->block_len == BLAKE3_BLOCK_LEN) {
            uint32_t block_words[16];
            words_from_le_bytes(c->block, block_words, 16);
            uint32_t full[16];
            compress(c->cv, block_words, c->chunk_counter, BLAKE3_BLOCK_LEN,
                     flags | chunk_start_flag(c), full);
            for (int i = 0; i < 8; i++) c->cv[i] = full[i];
            c->blocks_compressed++;
            c->block_len = 0;
            /* The just-compressed block's bytes must not leak into the NEXT
             * block's padding: words_from_le_bytes always reads all 64 bytes
             * of c->block regardless of how many are meaningful, so any
             * stale byte past the next block's block_len would be fed to
             * compress() as nonzero message input instead of the spec's
             * required zero pad -- silently wrong only for chunks whose
             * final block is partial, i.e. every input length that is not
             * an exact multiple of 64. Caught by the official vectors at
             * length 65 (one byte into a second block). */
            for (int i = 0; i < BLAKE3_BLOCK_LEN; i++) c->block[i] = 0;
        }
        size_t want = BLAKE3_BLOCK_LEN - c->block_len;
        size_t take = want < len ? want : len;
        for (size_t i = 0; i < take; i++) c->block[c->block_len + i] = in[i];
        c->block_len += (uint8_t)take;
        in += take; len -= take;
    }
}

static void chunk_state_output(const struct blake3_chunk_state *c, uint32_t flags,
                               struct output *o)
{
    for (int i = 0; i < 8; i++) o->cv[i] = c->cv[i];
    words_from_le_bytes(c->block, o->block_words, 16);
    /* Only the first `block_len` bytes of c->block are meaningful; the rest
     * must read as the spec's required zero pad. That is true here only
     * because BOTH chunk_state_init (the chunk's very first block) AND the
     * full-block branch of chunk_state_update (every block after) clear the
     * whole 64-byte buffer before anything is written into it again -- an
     * earlier version of this file cleared neither and passed every vector
     * whose final block happened to be full (len a multiple of 64) while
     * silently feeding a previous block's stale bytes into every other
     * length's padding. */
    o->counter = c->chunk_counter;
    o->block_len = c->block_len;
    o->flags = flags | chunk_start_flag(c) | CHUNK_END
#ifdef BLAKE3_BUG_ROOT_ALWAYS
    /* NEGATIVE CONTROL -- trap (1) made real. Every chunk output claims to
     * be the root, not just the one that actually is. For any input that
     * fits in a single chunk (<=1024 bytes) there is only ever one chunk
     * output and it IS the root, so this branch is invisible there; the
     * official vectors include ten input lengths above 1024 bytes and this
     * must diverge on every one of them, because now blake3_finalize_seek's
     * own `| ROOT` on the true root lands on a value that already had ROOT
     * baked in one layer down, and every non-final chunk that happens to
     * fill exactly one whole block also gets a bogus ROOT it must not have.
     * See tests/unit/blake3_test.c's control run. */
    | ROOT
#endif
    ;
}

/* ------------------------------------------------------------- parent ---- */

static void parent_output(const uint32_t left_cv[8], const uint32_t right_cv[8],
                          const uint32_t key_words[8], uint32_t flags,
                          struct output *o)
{
    for (int i = 0; i < 8; i++) { o->block_words[i] = left_cv[i]; o->block_words[8+i] = right_cv[i]; }
    for (int i = 0; i < 8; i++) o->cv[i] = key_words[i];
    o->counter = 0;
    o->block_len = BLAKE3_BLOCK_LEN;
    o->flags = PARENT | flags;
}

static void parent_cv(const uint32_t left_cv[8], const uint32_t right_cv[8],
                      const uint32_t key_words[8], uint32_t flags, uint32_t cv[8])
{
    struct output o;
    parent_output(left_cv, right_cv, key_words, flags, &o);
    output_chaining_value(&o, cv);
}

/* --------------------------------------------------------------- API ----- */

static void hasher_init_internal(struct blake3_hasher *h, const uint32_t key_words[8],
                                 uint32_t flags)
{
    chunk_state_init(&h->chunk, key_words, 0);
    for (int i = 0; i < 8; i++) h->key_words[i] = key_words[i];
    h->flags = flags;
    h->cv_stack_len = 0;
}

void blake3_init(struct blake3_hasher *h) { hasher_init_internal(h, IV, 0); }

void blake3_init_keyed(struct blake3_hasher *h, const uint8_t key[BLAKE3_KEY_LEN])
{
    uint32_t kw[8];
    words_from_le_bytes(key, kw, 8);
    hasher_init_internal(h, kw, KEYED_HASH);
}

void blake3_init_derive_key(struct blake3_hasher *h, const void *context, size_t context_len)
{
    /* Two-pass, per spec 6.2: first hash the (public, but hardcoded and
     * application-specific) context string under DERIVE_KEY_CONTEXT to get a
     * 32-byte context key, THEN start a fresh hasher keyed with THAT under
     * DERIVE_KEY_MATERIAL for the actual key material. One-pass ("just key
     * the material hash with a fixed transform of the context bytes") would
     * make derive_key indistinguishable from keyed_hash to an attacker who
     * controls the "context", collapsing the domain separation the two
     * flags exist to buy -- see spec 7 on the flag design. */
    struct blake3_hasher ctx_hasher;
    hasher_init_internal(&ctx_hasher, IV, DERIVE_KEY_CONTEXT);
    blake3_update(&ctx_hasher, context, context_len);
    uint8_t context_key[BLAKE3_KEY_LEN];
    blake3_finalize(&ctx_hasher, context_key);

    uint32_t kw[8];
    words_from_le_bytes(context_key, kw, 8);
    hasher_init_internal(h, kw, DERIVE_KEY_MATERIAL);
}

/* spec 5.1.2's merge recurrence, expressed exactly as a binary counter's
 * carry chain: `total_chunks` is the count AFTER this chunk, and its number
 * of trailing zero bits is exactly the number of already-completed subtrees
 * this chunk's arrival completes (a chunk index ending in k zero bits
 * completes a subtree of 2^k chunks whose left half was already on the
 * stack). This is trap (3) resolved without ever computing a split point. */
static void add_chunk_cv(struct blake3_hasher *h, uint32_t new_cv[8], uint64_t total_chunks)
{
    while ((total_chunks & 1) == 0) {
        uint32_t merged[8];
        h->cv_stack_len--;
        parent_cv(h->cv_stack[h->cv_stack_len], new_cv, h->key_words, h->flags, merged);
        for (int i = 0; i < 8; i++) new_cv[i] = merged[i];
        total_chunks >>= 1;
    }
    for (int i = 0; i < 8; i++) h->cv_stack[h->cv_stack_len][i] = new_cv[i];
    h->cv_stack_len++;
}

void blake3_update(struct blake3_hasher *h, const void *input, size_t len)
{
    const uint8_t *in = (const uint8_t *)input;
    while (len > 0) {
        if (chunk_state_len(&h->chunk) == BLAKE3_CHUNK_LEN) {
            struct output o;
            chunk_state_output(&h->chunk, h->flags, &o);
            uint32_t chunk_cv[8];
            output_chaining_value(&o, chunk_cv);
            uint64_t total_chunks = h->chunk.chunk_counter + 1;
            add_chunk_cv(h, chunk_cv, total_chunks);
            chunk_state_init(&h->chunk, h->key_words, total_chunks);
        }
        size_t want = BLAKE3_CHUNK_LEN - chunk_state_len(&h->chunk);
        size_t take = want < len ? want : len;
        chunk_state_update(&h->chunk, h->flags, in, take);
        in += take; len -= take;
    }
}

void blake3_finalize_seek(const struct blake3_hasher *h, uint64_t seek,
                          uint8_t *out, size_t out_len)
{
    struct output o;
    chunk_state_output(&h->chunk, h->flags, &o);

    /* Walk the CV stack from its top (most recently pushed = nearest the
     * root's right edge) down to its base, folding each entry in as the
     * LEFT sibling of whatever `o` currently represents -- this reproduces,
     * without recursion, exactly the right-spine merge spec 5.1.2 describes
     * as building the tree bottom-up as chunks arrive. */
    int i = h->cv_stack_len;
    while (i > 0) {
        i--;
        uint32_t right_cv[8];
        output_chaining_value(&o, right_cv);
        parent_output(h->cv_stack[i], right_cv, h->key_words, h->flags, &o);
    }

    /* `o` now describes the ROOT node -- the chunk output itself if this
     * message was a single chunk, or the final parent otherwise. ROOT is
     * added exactly HERE, exactly once, regardless of which case it was;
     * see trap (1) at the top of this file for why that placement is the
     * whole argument. */
    root_output_bytes(&o, seek, out, out_len);
}

void blake3_finalize(const struct blake3_hasher *h, uint8_t out[BLAKE3_OUT_LEN])
{
    blake3_finalize_seek(h, 0, out, BLAKE3_OUT_LEN);
}
