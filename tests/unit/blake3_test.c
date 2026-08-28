/* BLAKE3 host known-answer test, against the BLAKE3 team's OWN test vector
 * file (tests/unit/blake3_vectors.inc, generated from
 * https://raw.githubusercontent.com/BLAKE3-team/BLAKE3/master/test_vectors/test_vectors.json
 * -- see that file's header for the fetch date). This is the reference
 * implementation's own KAT/XOF corpus, not anything derived from the code
 * under test.
 *
 * openssl has no BLAKE3 support (checked: `openssl list -digest-commands`
 * on the openssl@3.6.3 this tree uses names sha-family, shake-family and
 * blake2-family digests and no blake3), so there is no differential gate
 * to write alongside this one --
 * the official vector file is the strongest reference actually available,
 * and it is a strong one: 35 input lengths from 0 to 102,400 bytes, chosen by
 * the BLAKE3 authors themselves to straddle every 1024-byte chunk boundary
 * and every subtree-merge boundary, times three modes (hash / keyed_hash /
 * derive_key), each checked against 131 bytes of EXTENDED output -- enough to
 * force a third 64-byte XOF block and catch an output-block counter that
 * starts at the wrong value. All 35*3 = 105 cases run; the vectors were
 * deliberately chosen not to be safely sub-sampled (see blake3.c's header).
 *
 * The negative control is BLAKE3_BUG_ROOT_ALWAYS (see blake3.c) -- a `-D`
 * that makes every chunk's own compression claim the ROOT flag, not just the
 * one that is actually the root. It is invisible on the vectors' four
 * single-chunk cases (0, 1, 63, 64, 65, 1023, 1024 bytes -- <=1024) and must
 * redden every multi-chunk case (1025 bytes and up: 10 of the 35 lengths).
 * `make test-blake3-negctl` builds with the flag and asserts it fails with
 * exactly that shape -- single-chunk cases still passing, multi-chunk cases
 * not.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "blake3.h"

#include "blake3_vectors.inc"

static int pass, fail;
static void ck(int cond, const char *what)
{
    if (cond) pass++;
    else { fail++; printf("FAIL: %s\n", what); }
}

static void unhex(uint8_t *o, const char *h, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned v; sscanf(h + 2 * i, "%2x", &v); o[i] = (uint8_t)v;
    }
}

/* Fills buf[0..len) with the vector file's own input rule: the repeating
 * 251-byte cycle 0,1,...,250,0,1,... -- NOT random and NOT all-zero, on
 * purpose (an all-zero input cannot catch a message-word permutation bug
 * that only ever swaps two zero words). */
static void fill_input(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(i % 251);
}

/* Split single-chunk (<=1024 bytes -> root IS the chunk) from multi-chunk,
 * because that split is exactly what the negative control must separate --
 * BLAKE3_BUG_ROOT_ALWAYS is invisible on the former and wrong on the latter.
 * Tracked so main() can report the shape, not just a pass/fail count. */
static int single_chunk_pass, single_chunk_fail;
static int multi_chunk_pass, multi_chunk_fail;

static void check_one(size_t input_len, const uint8_t *in,
                      struct blake3_hasher *h, const char *want_hex,
                      const char *label)
{
    uint8_t want[BLAKE3_VEC_OUTLEN];
    unhex(want, want_hex, BLAKE3_VEC_OUTLEN);

    /* Two things checked at once, per the vector file's own comment: the
     * extended output, AND that its first 32 bytes equal the plain
     * (non-extended) finalize -- an implementation whose seek=0 XOF path and
     * whose default-length path disagree would pass neither on its own. */
    uint8_t got32[BLAKE3_OUT_LEN];
    blake3_finalize(h, got32);
    int ok32 = memcmp(got32, want, BLAKE3_OUT_LEN) == 0;

    uint8_t got[BLAKE3_VEC_OUTLEN];
    blake3_finalize_seek(h, 0, got, BLAKE3_VEC_OUTLEN);
    int okx = memcmp(got, want, BLAKE3_VEC_OUTLEN) == 0;

    /* Also check a genuinely offset seek (not from 0), a property no
     * single-shot finalize() call can exercise: bytes [64,131) read via a
     * seek=64 call must equal the same slice of the seek=0 call. */
    uint8_t got_tail[BLAKE3_VEC_OUTLEN - 64];
    blake3_finalize_seek(h, 64, got_tail, sizeof got_tail);
    int okseek = memcmp(got_tail, got + 64, sizeof got_tail) == 0;

    int ok = ok32 && okx && okseek;
    char msg[128];
    snprintf(msg, sizeof msg, "%s len=%zu (32b:%d xof131:%d seek64:%d)",
             label, input_len, ok32, okx, okseek);
    ck(ok, msg);

    if (input_len <= BLAKE3_CHUNK_LEN) { if (ok) single_chunk_pass++; else single_chunk_fail++; }
    else                               { if (ok) multi_chunk_pass++;  else multi_chunk_fail++; }

    (void)in;
}

int main(void)
{
    uint8_t key[BLAKE3_KEY_LEN];
    memcpy(key, BLAKE3_VEC_KEY, BLAKE3_KEY_LEN); /* the ASCII key is exactly 32 bytes */

    static uint8_t input[102400];

    for (int i = 0; i < BLAKE3_VEC_COUNT; i++) {
        const struct blake3_vec *v = &BLAKE3_VECS[i];
        fill_input(input, v->input_len);

        struct blake3_hasher h;

        blake3_init(&h);
        blake3_update(&h, input, v->input_len);
        check_one(v->input_len, input, &h, v->hash_hex, "hash");

        blake3_init_keyed(&h, key);
        blake3_update(&h, input, v->input_len);
        check_one(v->input_len, input, &h, v->keyed_hash_hex, "keyed_hash");

        blake3_init_derive_key(&h, BLAKE3_VEC_CONTEXT, sizeof(BLAKE3_VEC_CONTEXT) - 1);
        blake3_update(&h, input, v->input_len);
        check_one(v->input_len, input, &h, v->derive_key_hex, "derive_key");
    }

    /* Two extra structural properties beyond the vector file, cheap and
     * worth pinning: update() called in many small pieces must equal one
     * big update() (this is what the chunk_state block-buffering code
     * exists for), and update() called with a zero-length slice must be a
     * no-op. Length 5000 spans multiple chunks with a partial final block. */
    {
        uint8_t buf[5000];
        fill_input(buf, sizeof buf);
        struct blake3_hasher h1, h2;
        blake3_init(&h1);
        blake3_update(&h1, buf, sizeof buf);
        uint8_t out1[64]; blake3_finalize_seek(&h1, 0, out1, sizeof out1);

        blake3_init(&h2);
        blake3_update(&h2, buf, 0);          /* no-op */
        size_t off = 0;
        size_t steps[] = { 1, 63, 64, 65, 900, 1, 1023, 1024, 1024, 1024 };
        for (size_t s = 0; s < sizeof(steps)/sizeof(steps[0]) && off < sizeof buf; s++) {
            size_t n = steps[s];
            if (off + n > sizeof buf) n = sizeof buf - off;
            blake3_update(&h2, buf + off, n);
            off += n;
        }
        if (off < sizeof buf) blake3_update(&h2, buf + off, sizeof buf - off);
        uint8_t out2[64]; blake3_finalize_seek(&h2, 0, out2, sizeof out2);
        ck(memcmp(out1, out2, sizeof out1) == 0, "incremental update() in ragged pieces == one-shot");
    }

    printf("blake3: %d passed, %d failed (single-chunk %d/%d, multi-chunk %d/%d)\n",
           pass, fail, single_chunk_pass, single_chunk_pass + single_chunk_fail,
           multi_chunk_pass, multi_chunk_pass + multi_chunk_fail);
    return fail ? 1 : 0;
}
