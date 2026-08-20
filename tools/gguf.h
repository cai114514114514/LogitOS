/* gguf.h -- read a GGUF file well enough to convert one. HOST ONLY.
 *
 * It is deliberately NOT a general GGUF library, and the refusals are the
 * design: every quantisation type but F32/F16/Q8_0 is refused BY NAME, every
 * key is fetched BY NAME with a type check, and a tensor whose shape does not
 * match what the caller expected is refused with both shapes printed. A GGUF
 * reader that silently skips what it does not understand produces a model file
 * with holes in it that lm_open, lm_expected_size and a forward pass all pass
 * over without complaint.
 *
 * WHY C AND NOT PYTHON -- the question the task asks to argue, and three of
 * the four reasons are already in tools/lmshape.c's header and hold unchanged.
 * The fourth is specific to this machine and is the one that decided it:
 *
 *   1. (lmshape.c's) numpy is not vendored and a build step that begins with
 *      `pip install` fails on somebody else's machine six months from now.
 *   2. (lmshape.c's) a per-element Python loop over 596 million weights is
 *      minutes, not seconds.
 *   3. (lmshape.c's) the quantiser must be the real one. A Python converter
 *      would have to either call back into q4_quantize through a binding that
 *      does not exist, or reimplement it -- a second rounding rule that agrees
 *      almost always, and the cases where it does not are the ones worth
 *      finding.
 *   4. MEASURED ON THIS MACHINE: the build runs in WSL and the only python
 *      with torch is the WINDOWS one (torch 2.13.0+cu132; WSL's python3 has
 *      numpy 2.3.5 and no torch). gguf-py and llama-cpp-python are installed
 *      on NEITHER. So a Python converter in the build graph would have to
 *      cross a shell boundary that `make` cannot, to reach an interpreter
 *      whose GGUF library does not exist either. tools/gguf_check.py is the
 *      numpy ORACLE, which is a different job and runs outside the build.
 *
 * THE ACCESS PATTERN THIS EXISTS TO SERVE is tools/lmshape.c's writer, which
 * streams one row at a time and holds nothing larger. So the interface is
 * "give me element i of tensor T" in O(1), not "give me the tensor". That is
 * also what keeps --verify able to re-derive the first and last element of
 * every tensor from a 610 MB file without materialising any of it, exactly as
 * it does from the PRNG seed today.
 *
 * O(1) IS FREE HERE AND IT IS WORTH SAYING WHY, because it is a property of
 * this model rather than of the format. Q8_0's blocks run along the
 * CONTIGUOUS dimension (ne0), and every ne0 in this file is a multiple of 32
 * (1024, 2048, 3072) -- so a block never straddles a row, and the flat
 * row-major index IS the block index times 32 plus the lane. No per-row
 * arithmetic, no row cache, no partial block. gguf_get asserts the multiple
 * rather than assuming it: a model whose ne0 is not a multiple of 32 would
 * still be a legal GGUF and this reader must refuse it rather than compute a
 * plausible wrong offset.
 */
#ifndef LOGIT_GGUF_H
#define LOGIT_GGUF_H

#include <stddef.h>
#include <stdint.h>

/* ggml type ids. Only these three appear in a Q8_0 Qwen3 export; the reader
 * refuses everything else by number, and the number is printed so the refusal
 * names what it saw rather than "unsupported". */
enum { GGML_F32 = 0, GGML_F16 = 1, GGML_Q8_0 = 8 };

#define GGUF_MAXDIM 4

struct gguf_tensor {
    char           name[64];
    int            ndim;
    uint64_t       dim[GGUF_MAXDIM];   /* AS GGUF STATES THEM: dim[0] is the
                                        * CONTIGUOUS one. See gguf_get. */
    uint32_t       type;               /* GGML_* */
    uint64_t       off;                /* from the aligned data base */
    const uint8_t *p;                  /* resolved: blob + data0 + off */
    uint64_t       numel;
};

struct gguf_kv {
    char     key[96];
    uint32_t type;                     /* GGUF value type, see gguf.c */
    /* Scalars are kept; strings and arrays are NOT materialised (the token
     * list alone is 151,936 of them and nothing here reads one). `str` points
     * into the mapping for a string value and is NUL-terminated only by
     * accident, so `slen` is the length. */
    union { uint64_t u; int64_t i; double f; } num;
    const char *str;
    uint64_t    slen;
    uint64_t    acount;                /* elements, when type is an array */
};

struct gguf {
    int             fd;
    const uint8_t  *blob;
    size_t          len;
    uint32_t        version;
    uint64_t        data0;             /* aligned base of the tensor payload */
    uint64_t        align;
    struct gguf_kv *kv;
    uint64_t        nkv;
    struct gguf_tensor *t;
    uint64_t        nt;
};

/* Open and parse. Returns 0, or non-zero with a reason already on stderr.
 * Never leaves a half-built struct: on failure everything is released. */
int  gguf_open(struct gguf *g, const char *path);
void gguf_close(struct gguf *g);

/* Look a tensor up by exact name. NULL if absent -- the caller decides whether
 * absent is an error, because for `output.weight` it means "tied", which is
 * information, and for `blk.7.attn_q.weight` it means the file is broken. */
const struct gguf_tensor *gguf_find(const struct gguf *g, const char *name);

/* Metadata by exact key, with the type checked. Return 0 on success. A missing
 * key is an ERROR the caller must handle rather than a default the reader
 * invents: a defaulted rope base is precisely the failure that produces
 * fluent-looking wrong text. */
int gguf_u32(const struct gguf *g, const char *key, uint32_t *out);
int gguf_f32(const struct gguf *g, const char *key, float *out);
/* Element count of an array-valued key (the vocabulary size arrives this way). */
int gguf_alen(const struct gguf *g, const char *key, uint64_t *out);
/* A string value, as a pointer into the mapping plus its length. */
int gguf_str(const struct gguf *g, const char *key, const char **s, uint64_t *n);

/* ONE ELEMENT, by flat row-major index in [ne1, ne0] order -- which is the
 * SAME flat order the bytes are already in, because ne0 is the contiguous
 * dimension. So this is not a transposition and does not perform one; see the
 * transposition note in tools/lmshape.c, which is where the orientation
 * decision is made and proved.
 *
 * Out of range returns 0.0f, which is a value and not an error, so callers
 * must range-check. There is exactly one caller (lmshape.c's elem()) and it is
 * driven by the descriptor list, which is checked against these dims at build
 * time -- so a range failure here would mean that check did not run. */
float gguf_get(const struct gguf_tensor *t, uint64_t idx);

/* Dequantise `nrow` whole rows into `out` (nrow * ne0 floats). Only used by
 * the orientation proof and the element dump, never by the writer -- the
 * writer streams through gguf_get. Returns 0 on success. */
int gguf_rows(const struct gguf_tensor *t, uint64_t r0, uint64_t nrow, float *out);

/* f16 -> f32, written out rather than taken from a compiler builtin: -w
 * -ffreestanding hosts differ on _Float16 and __fp16, and a wrong conversion
 * of the SCALE is a per-block multiplicative error, which is exactly the
 * failure mode gguf_check.py exists to catch. Exposed because the test wants
 * to hit it directly. */
float gguf_f16_to_f32(uint16_t h);

const char *gguf_type_name(uint32_t t);

#endif /* LOGIT_GGUF_H */
