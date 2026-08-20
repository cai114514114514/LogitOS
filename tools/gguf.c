/* gguf.c -- the reader. See tools/gguf.h for why it is C and what it refuses.
 *
 * cc -O2 -w -Itools -c tools/gguf.c        (host only; not in C_SRC)
 */
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "gguf.h"

/* GGUF value types. The widths are spelled out rather than derived, because a
 * wrong width does not fail here -- it silently shifts every following
 * key-value pair AND the tensor table after them, so the first symptom is a
 * tensor at a nonsense offset a hundred keys later. */
enum {
    GV_U8 = 0, GV_I8, GV_U16, GV_I16, GV_U32, GV_I32, GV_F32,
    GV_BOOL, GV_STR, GV_ARR, GV_U64, GV_I64, GV_F64
};

static const int gv_width[] = { 1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8 };

const char *gguf_type_name(uint32_t t)
{
    switch (t) {
    case GGML_F32:  return "F32";
    case GGML_F16:  return "F16";
    case GGML_Q8_0: return "Q8_0";
    default:        return "OTHER";
    }
}

/* ------------------------------------------------------------------ f16 --
 *
 * IEEE 754 binary16 -> binary32, including subnormals and inf/nan. Written in
 * full rather than "s * 2^(e-15) * m", which is right for normals and wrong
 * for the subnormal scales a heavily-quantised block produces -- and a wrong
 * scale is a per-block multiplicative error, i.e. the exact failure that
 * survives every structural check in the tree. */
float gguf_f16_to_f32(uint16_t h)
{
    uint32_t s = (uint32_t)(h & 0x8000u) << 16;
    uint32_t e = (h >> 10) & 0x1Fu;
    uint32_t m = h & 0x3FFu;
    uint32_t bits;

    if (e == 0) {
        if (m == 0) { bits = s; }                     /* +-0 */
        else {
            /* Subnormal: renormalise. The exponent of the f32 result is
             * 127 - 15 + 1 - shift, where shift is how far the mantissa's
             * leading 1 has to travel. */
            int sh = 0;
            while (!(m & 0x400u)) { m <<= 1; sh++; }
            m &= 0x3FFu;
            bits = s | ((uint32_t)(127 - 15 + 1 - sh) << 23) | (m << 13);
        }
    } else if (e == 31) {
        bits = s | 0x7F800000u | (m << 13);           /* inf / nan */
    } else {
        bits = s | ((e + (127 - 15)) << 23) | (m << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* ------------------------------------------------------------ the parser --
 *
 * A cursor with a hard bound. Every read checks the remaining length BEFORE
 * consuming, so a truncated or hostile file makes the parser refuse rather
 * than walk off the mapping -- this reads a 610 MB file that arrived over the
 * network, and an out-of-range length prefix is the first thing a fuzzer
 * finds in any format with a `u64 len` in it. */
struct cur { const uint8_t *p; size_t n, i; int bad; };

static int need(struct cur *c, size_t k)
{
    if (c->bad) return 0;
    if (k > c->n - c->i) { c->bad = 1; return 0; }
    return 1;
}
static uint8_t  rd8 (struct cur *c) { if (!need(c,1)) return 0; return c->p[c->i++]; }
static uint16_t rd16(struct cur *c) { if (!need(c,2)) return 0; uint16_t v; memcpy(&v,c->p+c->i,2); c->i+=2; return v; }
static uint32_t rd32(struct cur *c) { if (!need(c,4)) return 0; uint32_t v; memcpy(&v,c->p+c->i,4); c->i+=4; return v; }
static uint64_t rd64(struct cur *c) { if (!need(c,8)) return 0; uint64_t v; memcpy(&v,c->p+c->i,8); c->i+=8; return v; }

/* A string is [u64 len][bytes]. Returned as a pointer INTO the mapping: no
 * copy, because the largest one in this file is a chat template of several
 * kilobytes and the array of them is 151,936 entries. */
static const char *rdstr(struct cur *c, uint64_t *len)
{
    uint64_t n = rd64(c);
    if (c->bad || n > c->n - c->i) { c->bad = 1; *len = 0; return ""; }
    const char *s = (const char *)(c->p + c->i);
    c->i += (size_t)n;
    *len = n;
    return s;
}

/* Skip a value of type `t`, recording what it was. Arrays of strings are
 * WALKED (each length read and skipped) rather than jumped over, because
 * there is no total length in the encoding to jump by. */
static void skipval(struct cur *c, uint32_t t, struct gguf_kv *kv)
{
    uint64_t l;
    if (t == GV_STR) {
        kv->str = rdstr(c, &l);
        kv->slen = l;
        return;
    }
    if (t == GV_ARR) {
        uint32_t et = rd32(c);
        uint64_t n  = rd64(c);
        kv->acount = n;
        if (et == GV_STR) {
            for (uint64_t k = 0; k < n && !c->bad; k++) { (void)rdstr(c, &l); }
        } else if (et == GV_ARR) {
            /* Nested arrays are legal in the spec and appear in no export this
             * converter targets. Refused loudly rather than skipped, because
             * "skip a length I cannot compute" is how a parser desynchronises
             * silently. */
            fprintf(stderr, "gguf: a nested array value -- this reader refuses "
                            "it rather than guessing its length\n");
            c->bad = 1;
        } else if (et < (uint32_t)(sizeof gv_width / sizeof gv_width[0])) {
            if (n > (c->n - c->i) / (uint64_t)gv_width[et]) c->bad = 1;
            else c->i += (size_t)(n * (uint64_t)gv_width[et]);
        } else c->bad = 1;
        return;
    }
    switch (t) {
    case GV_U8: case GV_I8: case GV_BOOL: kv->num.u = rd8(c);  break;
    case GV_U16: case GV_I16:             kv->num.u = rd16(c); break;
    case GV_U32:                          kv->num.u = rd32(c); break;
    case GV_I32:                          kv->num.i = (int32_t)rd32(c); break;
    case GV_F32: { uint32_t b = rd32(c); float f; memcpy(&f,&b,4); kv->num.f = f; break; }
    case GV_U64: case GV_I64:             kv->num.u = rd64(c); break;
    case GV_F64: { uint64_t b = rd64(c); double d; memcpy(&d,&b,8); kv->num.f = d; break; }
    default:
        fprintf(stderr, "gguf: value type %u is not one this reader knows\n", t);
        c->bad = 1;
    }
}

int gguf_open(struct gguf *g, const char *path)
{
    memset(g, 0, sizeof *g);
    g->fd = open(path, O_RDONLY);
    if (g->fd < 0) { perror(path); return 1; }
    struct stat st;
    if (fstat(g->fd, &st) != 0) { perror("fstat"); close(g->fd); g->fd = -1; return 1; }
    g->len = (size_t)st.st_size;
    void *b = mmap(NULL, g->len, PROT_READ, MAP_PRIVATE, g->fd, 0);
    if (b == MAP_FAILED) { perror("mmap"); close(g->fd); g->fd = -1; return 1; }
    g->blob = (const uint8_t *)b;

    struct cur c = { g->blob, g->len, 0, 0 };
    if (rd8(&c) != 'G' || rd8(&c) != 'G' || rd8(&c) != 'U' || rd8(&c) != 'F') {
        fprintf(stderr, "gguf: %s does not begin with the GGUF magic\n", path);
        gguf_close(g); return 1;
    }
    g->version = rd32(&c);
    if (g->version != 2 && g->version != 3) {
        /* REFUSED, not warned. v1 has a different tensor-info encoding, and a
         * future version may move the alignment rule; a reader that proceeds
         * produces a model built from bytes it read at the wrong offsets. */
        fprintf(stderr, "gguf: version %u -- this reader knows 2 and 3\n", g->version);
        gguf_close(g); return 1;
    }
    g->nt  = rd64(&c);
    g->nkv = rd64(&c);
    if (c.bad || g->nt > 1000000 || g->nkv > 100000) {
        fprintf(stderr, "gguf: header says %llu tensors and %llu keys, which is "
                "not a file this reader will try to parse\n",
                (unsigned long long)g->nt, (unsigned long long)g->nkv);
        gguf_close(g); return 1;
    }

    g->kv = (struct gguf_kv *)calloc((size_t)g->nkv, sizeof *g->kv);
    g->t  = (struct gguf_tensor *)calloc((size_t)g->nt, sizeof *g->t);
    if (!g->kv || !g->t) { fprintf(stderr, "gguf: out of memory\n"); gguf_close(g); return 1; }

    for (uint64_t k = 0; k < g->nkv && !c.bad; k++) {
        uint64_t n;
        const char *key = rdstr(&c, &n);
        if (n >= sizeof g->kv[k].key) n = sizeof g->kv[k].key - 1;
        memcpy(g->kv[k].key, key, (size_t)n);
        g->kv[k].key[n] = 0;
        g->kv[k].type = rd32(&c);
        skipval(&c, g->kv[k].type, &g->kv[k]);
    }
    for (uint64_t k = 0; k < g->nt && !c.bad; k++) {
        struct gguf_tensor *t = &g->t[k];
        uint64_t n;
        const char *nm = rdstr(&c, &n);
        if (n >= sizeof t->name) n = sizeof t->name - 1;
        memcpy(t->name, nm, (size_t)n);
        t->name[n] = 0;
        t->ndim = (int)rd32(&c);
        if (t->ndim < 1 || t->ndim > GGUF_MAXDIM) { c.bad = 1; break; }
        t->numel = 1;
        for (int d = 0; d < t->ndim; d++) { t->dim[d] = rd64(&c); t->numel *= t->dim[d]; }
        t->type = rd32(&c);
        t->off  = rd64(&c);
    }
    if (c.bad) {
        fprintf(stderr, "gguf: %s is truncated or malformed -- the parser ran "
                "out of bytes at offset %llu of %llu\n",
                path, (unsigned long long)c.i, (unsigned long long)g->len);
        gguf_close(g); return 1;
    }

    uint32_t al = 32;
    for (uint64_t k = 0; k < g->nkv; k++)
        if (!strcmp(g->kv[k].key, "general.alignment")) al = (uint32_t)g->kv[k].num.u;
    if (al == 0 || (al & (al - 1))) {
        fprintf(stderr, "gguf: general.alignment is %u, not a power of two\n", al);
        gguf_close(g); return 1;
    }
    g->align = al;
    g->data0 = ((uint64_t)c.i + al - 1) / al * al;

    /* Resolve every tensor's pointer NOW and bound-check it NOW, so gguf_get
     * on the hot path is arithmetic with no branch on the mapping's length.
     * The bound is computed from the type's own storage rule; a type this
     * reader does not know is refused here, by name, with the offending
     * tensor named -- which is the requirement in the task, and the reason it
     * is here rather than at first use: a tensor nothing happens to read would
     * otherwise pass unnoticed into a model with a hole in it. */
    for (uint64_t k = 0; k < g->nt; k++) {
        struct gguf_tensor *t = &g->t[k];
        uint64_t bytes;
        if (t->type == GGML_F32)       bytes = t->numel * 4;
        else if (t->type == GGML_F16)  bytes = t->numel * 2;
        else if (t->type == GGML_Q8_0) {
            if (t->dim[0] % 32) {
                fprintf(stderr, "gguf: %s is Q8_0 with ne0=%llu, not a multiple "
                        "of the 32-weight block -- a block would straddle a row "
                        "and this reader's O(1) index would be wrong. REFUSED.\n",
                        t->name, (unsigned long long)t->dim[0]);
                gguf_close(g); return 1;
            }
            bytes = t->numel / 32 * 34;
        } else {
            fprintf(stderr, "gguf: %s is ggml type %u, which this reader does "
                    "not implement. REFUSED rather than skipped -- a skipped "
                    "tensor is a hole in the model that every structural check "
                    "passes over.\n", t->name, t->type);
            gguf_close(g); return 1;
        }
        if (g->data0 + t->off + bytes > g->len) {
            fprintf(stderr, "gguf: %s claims %llu bytes at %llu, past the end of "
                    "a %llu-byte file\n", t->name, (unsigned long long)bytes,
                    (unsigned long long)(g->data0 + t->off),
                    (unsigned long long)g->len);
            gguf_close(g); return 1;
        }
        t->p = g->blob + g->data0 + t->off;
    }
    return 0;
}

void gguf_close(struct gguf *g)
{
    free(g->kv); free(g->t);
    if (g->blob) munmap((void *)g->blob, g->len);
    if (g->fd >= 0) close(g->fd);
    memset(g, 0, sizeof *g);
    g->fd = -1;
}

const struct gguf_tensor *gguf_find(const struct gguf *g, const char *name)
{
    for (uint64_t k = 0; k < g->nt; k++)
        if (!strcmp(g->t[k].name, name)) return &g->t[k];
    return NULL;
}

static const struct gguf_kv *kvfind(const struct gguf *g, const char *key)
{
    for (uint64_t k = 0; k < g->nkv; k++)
        if (!strcmp(g->kv[k].key, key)) return &g->kv[k];
    return NULL;
}

int gguf_u32(const struct gguf *g, const char *key, uint32_t *out)
{
    const struct gguf_kv *k = kvfind(g, key);
    if (!k) { fprintf(stderr, "gguf: no key `%s`\n", key); return 1; }
    switch (k->type) {
    case GV_U8: case GV_U16: case GV_U32: case GV_U64: case GV_BOOL:
        *out = (uint32_t)k->num.u; return 0;
    case GV_I32: case GV_I64:
        if (k->num.i < 0) { fprintf(stderr, "gguf: %s is negative\n", key); return 1; }
        *out = (uint32_t)k->num.i; return 0;
    default:
        fprintf(stderr, "gguf: %s has value type %u, not an integer\n", key, k->type);
        return 1;
    }
}

int gguf_f32(const struct gguf *g, const char *key, float *out)
{
    const struct gguf_kv *k = kvfind(g, key);
    if (!k) { fprintf(stderr, "gguf: no key `%s`\n", key); return 1; }
    if (k->type != GV_F32 && k->type != GV_F64) {
        fprintf(stderr, "gguf: %s has value type %u, not a float\n", key, k->type);
        return 1;
    }
    *out = (float)k->num.f;
    return 0;
}

int gguf_alen(const struct gguf *g, const char *key, uint64_t *out)
{
    const struct gguf_kv *k = kvfind(g, key);
    if (!k) { fprintf(stderr, "gguf: no key `%s`\n", key); return 1; }
    if (k->type != GV_ARR) { fprintf(stderr, "gguf: %s is not an array\n", key); return 1; }
    *out = k->acount;
    return 0;
}

int gguf_str(const struct gguf *g, const char *key, const char **s, uint64_t *n)
{
    const struct gguf_kv *k = kvfind(g, key);
    if (!k) { fprintf(stderr, "gguf: no key `%s`\n", key); return 1; }
    if (k->type != GV_STR) { fprintf(stderr, "gguf: %s is not a string\n", key); return 1; }
    *s = k->str; *n = k->slen;
    return 0;
}

/* ------------------------------------------------------- element access --
 *
 * Q8_0 IS SIMPLE AND IT IS WORTH SAYING SO IN FULL, because the whole
 * conversion rests on these two lines and the task ground is right that a
 * constant-factor error here produces a model that runs and talks nonsense:
 *
 *     block = 34 bytes = [ f16 scale ][ 32 x int8 ]
 *     value = scale * q
 *
 * No zero point, no minimum, no per-row term. (Q4_K and Q8_K do carry those;
 * this is not one of them, and adding one "for safety" would be a constant
 * offset on every weight -- the failure this file is defending against.)
 *
 * The f16 scale is converted through gguf_f16_to_f32 and the multiply is in
 * FLOAT, not double. That is a choice with a reason: tools/gguf_check.py
 * demands EXACT equality against its own dequantisation, so both sides must
 * make the same promotion. Doing it in double here would be "more accurate"
 * and would put the oracle permanently one ulp away, which converts an exact
 * check into a tolerance -- and a tolerance is what a constant factor of
 * 1.0001 hides in. */
float gguf_get(const struct gguf_tensor *t, uint64_t idx)
{
    if (idx >= t->numel) return 0.0f;
    if (t->type == GGML_F32) {
        float f; memcpy(&f, t->p + idx * 4, 4); return f;
    }
    if (t->type == GGML_F16) {
        uint16_t h; memcpy(&h, t->p + idx * 2, 2); return gguf_f16_to_f32(h);
    }
    /* Q8_0. gguf_open has already refused any tensor whose ne0 is not a
     * multiple of 32, so a block never straddles a row and the flat index is
     * the block index directly. */
    const uint8_t *b = t->p + (idx >> 5) * 34;
    uint16_t h; memcpy(&h, b, 2);
    return gguf_f16_to_f32(h) * (float)(int8_t)b[2 + (idx & 31)];
}

int gguf_rows(const struct gguf_tensor *t, uint64_t r0, uint64_t nrow, float *out)
{
    uint64_t ne0 = t->dim[0];
    if ((r0 + nrow) * ne0 > t->numel) return 1;
    for (uint64_t i = 0; i < nrow * ne0; i++)
        out[i] = gguf_get(t, r0 * ne0 + i);
    return 0;
}
