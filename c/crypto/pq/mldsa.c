#include "mldsa.h"
#include "keccak.h"

/* ML-DSA (FIPS 204), integer only, no libc, no allocation -- every buffer is a
 * caller's or an automatic, matching mlkem.c's discipline in the same
 * directory (this file must also link into the host KAT/differential gates,
 * which have no kernel heap).
 *
 * ARITHMETIC. q = 8380417 = 2^23 - 2^13 + 1, n = 256. Unlike ML-KEM's ring
 * (X^256+1 over q=3329, which splits into 128 quadratics because q-1 has only
 * 2-adic valuation 8), ML-DSA's q-1 = 8380416 = 2^13 * 3 * 11 * 31 has 2-adic
 * valuation 13, comfortably more than log2(256)=8 -- so X^256+1 splits
 * COMPLETELY into 256 linear factors here and the NTT is a full 8-layer
 * Cooley-Tukey transform with a plain per-coefficient product in the NTT
 * domain, not a degree-2 basemul. zeta = 1753 is the primitive 512th root of
 * unity FIPS 204 names; zetas[i] = zeta^brv8(i) mod q, generated and PROVEN
 * against schoolbook multiplication in Z_q[X]/(X^256+1) (5/5 exact, via the
 * same derive-then-verify discipline keccak.c and mlkem.c use for their own
 * tables -- a wrong entry here desynchronises every product silently, the VP8
 * table risk CLAUDE.md names). Forward NTT: zetas[1..255] consumed in
 * increasing order across 8 halving layers (len 128 down to 1). Inverse: the
 * same table consumed in DECREASING order (255 down to 1), negated, plus a
 * final scale by 256^-1 mod q = 8347681.
 *
 * MODULAR REDUCTION IS NOT CONSTANT TIME, AND IS NOT CHEAPLY MADE SO HERE.
 * modq() below is `a % Q; if (r<0) r+=Q`. On this target Q is a compile-time
 * constant, so clang lowers the `%` to a branch-free reciprocal-multiply (no
 * idiv) -- but that is a codegen property of THIS compiler on THIS constant,
 * not a specification this file enforces, and the correction step is a real
 * branch. This matches the portable AES backend's disclosure in
 * c/crypto/aead/aes_modes.c: a formally-verified branch-free Barrett reduction
 * for a 23-bit modulus (the mlkem.c model: an exhaustively-checked 16-bit
 * Barrett constant) is not "cheap" for a first ML-DSA implementation of this
 * size, and a subtly wrong hand-rolled correction step is exactly the kind of
 * defect that produces signatures which verify against THIS FILE's own
 * verifier and nothing else -- the trap CLAUDE.md's task brief names by name.
 * So: NOT proven constant time. Treat modq/NTT/basemul as a variable-time
 * arithmetic core, same status as aes_modes.c's portable backend.
 *
 * THE REJECTION LOOP (Sign_internal, FIPS 204 Algorithm 7) branches on
 * ||z||_inf, ||r0||_inf and the hint weight -- three DATA-DEPENDENT
 * `continue`s that are the whole mechanism of Fiat-Shamir with aborts. This is
 * intentionally not disguised as branch-free: the security argument (and the
 * pq-crystals reference implementation) is that the abort PROBABILITY is
 * independent of the secret by construction, so the number of iterations
 * leaks only what the scheme already intends to publish about itself. What
 * this file does keep exact against the spec, because getting them loose is
 * the silent-wrong-signature failure mode named in the task brief: every
 * bound is compared with >= (never >, which would accept a boundary value the
 * spec rejects and desynchronise from every other implementation only at the
 * boundary -- the one case a small KAT corpus is least likely to hit), and
 * Decompose's r1==(q-1)/alpha special case (make_hint/use_hint's inverse
 * partner) is implemented as its own branch, not folded away.
 *
 * EXPANDA'S BYTE ORDER: A[i][j] = RejNTTPoly(rho || j || i) -- COLUMN THEN
 * ROW. Swapping it produces a matrix that is still well-formed and still
 * self-consistent (our own keygen/sign/verify round-trip against each other
 * either way), interoperable with nothing -- see MLDSA_CTL_NO_TRANSPOSE below,
 * the same class of defect as ML-KEM's NO_TRANSPOSE control in mlkem.c.
 *
 * SCOPE CUT, NAMED RATHER THAN LEFT SILENT: HashML-DSA (the SHA-512 pre-hash
 * variant) and the IETF-draft external-mu entry point are NOT implemented.
 * Pure ML-DSA.Sign/Verify (FIPS 204 Algorithms 2-3) is. ACVP's sigGen/sigVer
 * groups that carry a `hashAlg` field exercise the pre-hash variant and this
 * file does not claim to pass them -- only the `hashAlg`-absent
 * "signatureInterface: external" groups are gated here.
 *
 * STACK. Not profiled or budgeted against a kernel thread's 32 KiB, unlike
 * mlkem.c's pke_decrypt (which measured -fstack-usage against a real caller).
 * This is deliberate for a first cut: nothing calls this code from ring 0 --
 * it is a library primitive with no trust-path consumer by the task this was
 * written under -- so there is no real call site to measure against yet. Do
 * NOT assume this fits a kernel thread stack without re-measuring; sign_internal
 * alone holds on the order of a dozen 256-entry int32 polynomials live at once
 * (~1 KiB each) across the k=8, l=7 (ML-DSA-87) parameter set.
 */

#define Q       8380417
#define N       256
#define D_BITS  13
#define NTT_F   8347681      /* 256^-1 mod Q, gen_derive above */

typedef struct { int32_t c[N]; } poly;

/* zetas[i] = 1753^brv8(i) mod Q, i = 0..255. Generated and verified (5/5
 * against schoolbook multiplication in Z_q[X]/(X^256+1)) -- see the file
 * header. zetas[0] = 1 is never referenced by either transform (both start
 * their table walk at index 1); kept for a clean 256-entry table rather than
 * a 255-entry one with an off-by-one index everywhere it is used. */
static const int32_t zetas[256] = {
          1, 4808194, 3765607, 3761513, 5178923, 5496691, 5234739, 5178987,
    7778734, 3542485, 2682288, 2129892, 3764867, 7375178,  557458, 7159240,
    5010068, 4317364, 2663378, 6705802, 4855975, 7946292,  676590, 7044481,
    5152541, 1714295, 2453983, 1460718, 7737789, 4795319, 2815639, 2283733,
    3602218, 3182878, 2740543, 4793971, 5269599, 2101410, 3704823, 1159875,
     394148,  928749, 1095468, 4874037, 2071829, 4361428, 3241972, 2156050,
    3415069, 1759347, 7562881, 4805951, 3756790, 6444618, 6663429, 4430364,
    5483103, 3192354,  556856, 3870317, 2917338, 1853806, 3345963, 1858416,
    3073009, 1277625, 5744944, 3852015, 4183372, 5157610, 5258977, 8106357,
    2508980, 2028118, 1937570, 4564692, 2811291, 5396636, 7270901, 4158088,
    1528066,  482649, 1148858, 5418153, 7814814,  169688, 2462444, 5046034,
    4213992, 4892034, 1987814, 5183169, 1736313,  235407, 5130263, 3258457,
    5801164, 1787943, 5989328, 6125690, 3482206, 4197502, 7080401, 6018354,
    7062739, 2461387, 3035980,  621164, 3901472, 7153756, 2925816, 3374250,
    1356448, 5604662, 2683270, 5601629, 4912752, 2312838, 7727142, 7921254,
     348812, 8052569, 1011223, 6026202, 4561790, 6458164, 6143691, 1744507,
       1753, 6444997, 5720892, 6924527, 2660408, 6600190, 8321269, 2772600,
    1182243,   87208,  636927, 4415111, 4423672, 6084020, 5095502, 4663471,
    8352605,  822541, 1009365, 5926272, 6400920, 1596822, 4423473, 4620952,
    6695264, 4969849, 2678278, 4611469, 4829411,  635956, 8129971, 5925040,
    4234153, 6607829, 2192938, 6653329, 2387513, 4768667, 8111961, 5199961,
    3747250, 2296099, 1239911, 4541938, 3195676, 2642980, 1254190, 8368000,
    2998219,  141835, 8291116, 2513018, 7025525,  613238, 7070156, 6161950,
    7921677, 6458423, 4040196, 4908348, 2039144, 6500539, 7561656, 6201452,
    6757063, 2105286, 6006015, 6346610,  586241, 7200804,  527981, 5637006,
    6903432, 1994046, 2491325, 6987258,  507927, 7192532, 7655613, 6545891,
    5346675, 8041997, 2647994, 3009748, 5767564, 4148469,  749577, 4357667,
    3980599, 2569011, 6764887, 1723229, 1665318, 2028038, 1163598, 5011144,
    3994671, 8368538, 7009900, 3020393, 3363542,  214880,  545376, 7609976,
    3105558, 7277073,  508145, 7826699,  860144, 3430436,  140244, 6866265,
    6195333, 3123762, 2358373, 6187330, 5365997, 6663603, 2926054, 7987710,
    8077412, 3531229, 4405932, 4606686, 1900052, 7598542, 1054478, 7648983,
};

/* -------------------------------------------------------- parameter sets */

const mldsa_params mldsa44_params = {
    "ML-DSA-44", 4, 4, 2, 39, 78, 1 << 17, (Q - 1) / 88, 80, 32,
    MLDSA44_PK, MLDSA44_SK, MLDSA44_SIG,
};
const mldsa_params mldsa65_params = {
    "ML-DSA-65", 6, 5, 4, 49, 196, 1 << 19, (Q - 1) / 32, 55, 48,
    MLDSA65_PK, MLDSA65_SK, MLDSA65_SIG,
};
const mldsa_params mldsa87_params = {
    "ML-DSA-87", 8, 7, 2, 60, 120, 1 << 19, (Q - 1) / 32, 75, 64,
    MLDSA87_PK, MLDSA87_SK, MLDSA87_SIG,
};

/* ------------------------------------------------------- small helpers */

/* No libc, same reasoning as mlkem.c: this file links into the kernel and
 * into a host test, and a libc memcpy/memset call would resolve to a
 * different implementation in each. */
static void cp(uint8_t *d, const uint8_t *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }
static void wipe(void *p, int n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

/* a mod Q into [0, Q). See the file header: NOT proven constant time. */
static int32_t modq(int64_t a)
{
    int64_t r = a % Q;
    if (r < 0) r += Q;
    return (int32_t)r;
}

/* r mod^+- n: the unique representative of r (mod n) in (-(n>>1), n>>1] for
 * even n and the symmetric integer range for odd n -- FIPS 204's own
 * definition (Section 2.4), used for Power2Round, Decompose's r0, and to
 * recover a small signed coefficient before bit-packing z/t0/s. Matches
 * utils.py's reduce_mod_pm exactly, including the `n >> 1` (not `n / 2`) form
 * so odd and even n are handled by one expression. */
static int32_t crd(int32_t r, int32_t n)
{
    int32_t rr = r % n;
    if (rr < 0) rr += n;
    if (rr > (n >> 1)) rr -= n;
    return rr;
}

static void poly_add(poly *r, const poly *a, const poly *b)
{ for (int i = 0; i < N; i++) r->c[i] = modq((int64_t)a->c[i] + b->c[i]); }
static void poly_sub(poly *r, const poly *a, const poly *b)
{ for (int i = 0; i < N; i++) r->c[i] = modq((int64_t)a->c[i] - b->c[i]); }
/* Pointwise product of two NTT-domain polynomials -- valid because X^256+1
 * splits completely here (see file header); no degree-2 basemul needed. */
static void poly_pmul(poly *r, const poly *a, const poly *b)
{ for (int i = 0; i < N; i++) r->c[i] = modq((int64_t)a->c[i] * b->c[i]); }
static void poly_scale(poly *r, const poly *a, int32_t s)
{ for (int i = 0; i < N; i++) r->c[i] = modq((int64_t)a->c[i] * s); }

/* ------------------------------------------------------------- the NTT */

static void ntt(poly *p)
{
    int k = 0;
    for (int len = 128; len >= 1; len >>= 1) {
        for (int start = 0; start < N; start += 2 * len) {
            int32_t zeta = zetas[++k];
            for (int j = start; j < start + len; j++) {
                int32_t t = modq((int64_t)zeta * p->c[j + len]);
                p->c[j + len] = modq((int64_t)p->c[j] - t);
                p->c[j]       = modq((int64_t)p->c[j] + t);
            }
        }
    }
}

static void invntt(poly *p)
{
    int k = 256;
    for (int len = 1; len < N; len <<= 1) {
        for (int start = 0; start < N; start += 2 * len) {
            int32_t zeta = modq(-(int64_t)zetas[--k]);
            for (int j = start; j < start + len; j++) {
                int32_t t = p->c[j];
                p->c[j]       = modq((int64_t)t + p->c[j + len]);
                int32_t d     = modq((int64_t)t - p->c[j + len]);  /* uses the OLD p->c[j+len] */
                p->c[j + len] = modq((int64_t)zeta * d);
            }
        }
    }
    for (int j = 0; j < N; j++) p->c[j] = modq((int64_t)p->c[j] * NTT_F);
}

/* --------------------------------------------------- generic bit packing */

/* Pack/unpack `n` coefficients of `nbits` bits each, little-endian bit order,
 * exactly FIPS 204's BitPack/BitUnpack -- same accumulator style as
 * mlkem.c's pack()/unpack(), generalised from d<=12 to nbits<=20 (z's width).
 * Safe from overflow: the accumulator's live bit count is always < 8 before
 * an insertion, so `nb + nbits` never exceeds 8+20=28 bits inside a uint32. */
static void bitpack(uint8_t *out, const uint32_t *v, int nbits)
{
    uint32_t acc = 0; int nb = 0, o = 0;
    for (int i = 0; i < N; i++) {
        acc |= (v[i] & ((1u << nbits) - 1u)) << nb;
        nb += nbits;
        while (nb >= 8) { out[o++] = (uint8_t)acc; acc >>= 8; nb -= 8; }
    }
}
static void bitunpack(uint32_t *v, const uint8_t *in, int nbits)
{
    uint32_t acc = 0; int nb = 0, o = 0;
    uint32_t mask = (1u << nbits) - 1u;
    for (int i = 0; i < N; i++) {
        while (nb < nbits) { acc |= (uint32_t)in[o++] << nb; nb += 8; }
        v[i] = acc & mask;
        acc >>= nbits; nb -= nbits;
    }
}

/* ----------------------------------------- Decompose / hints / norm bound */

/* Decompose, FIPS 204 Algorithm 37: r = r1*a + r0 with r0 in (-a/2, a/2] (or
 * its q-1 special case). `a` is always even here (a = 2*gamma2 in HighBits/
 * LowBits/MakeHint/UseHint, a = 2^d in Power2Round). THE SPECIAL CASE: when
 * the naive r1 would land exactly on (q-1)/a -- one past its valid range --
 * force r1 = 0 and step r0 down by one. Both ML-DSA gamma2 choices here divide
 * q-1 evenly ((q-1)/88 and (q-1)/32), so this triggers whenever r's canonical
 * representative is exactly q-1; a Decompose that drops this branch still
 * terminates and still looks plausible, and is exactly the kind of thing only
 * a KAT or a differential against a second implementation catches (a
 * self-consistent round trip does not exercise the boundary reliably). */
static void decompose(int32_t r_in, int32_t a, int32_t *r1, int32_t *r0)
{
    int32_t rp = modq(r_in);
    int32_t rz = crd(rp, a);
    if (rp - rz == Q - 1) { *r1 = 0; *r0 = rz - 1; }
    else                  { *r1 = (rp - rz) / a; *r0 = rz; }
}
static int32_t high_bits(int32_t r, int32_t a) { int32_t r1, r0; decompose(r, a, &r1, &r0); return r1; }
static int32_t low_bits(int32_t r, int32_t a)  { int32_t r1, r0; decompose(r, a, &r1, &r0); return r0; }

static void poly_power2round(const poly *t, poly *t1, poly *t0)
{
    for (int i = 0; i < N; i++) {
        int32_t r = modq(t->c[i]);
        int32_t r0 = crd(r, 1 << D_BITS);
        t0->c[i] = r0;
        t1->c[i] = (r - r0) >> D_BITS;
    }
}
static void poly_high_bits(poly *r, const poly *a, int32_t alpha)
{ for (int i = 0; i < N; i++) r->c[i] = high_bits(a->c[i], alpha); }
static void poly_low_bits(poly *r, const poly *a, int32_t alpha)
{ for (int i = 0; i < N; i++) r->c[i] = low_bits(a->c[i], alpha); }

/* MakeHint / UseHint, FIPS 204 Algorithms 39-40, applied directly to the two
 * polynomials whose HighBits differ (w-c.s2 and w-c.s2+c.t0) rather than
 * through the z/r indirection utils.py's make_hint(z,r,a,q) uses -- same
 * predicate, fewer sign-confusable arguments. See the sign loop below for the
 * derivation that this equals FIPS 204's MakeHint(-c.t0, w-c.s2+c.t0). */
static void poly_make_hint(poly *h, const poly *low, const poly *low_plus_ct0, int32_t alpha)
{
    for (int i = 0; i < N; i++)
        h->c[i] = (high_bits(low->c[i], alpha) != high_bits(low_plus_ct0->c[i], alpha)) ? 1 : 0;
}
static void poly_use_hint(poly *r, const poly *h, const poly *w, int32_t alpha)
{
    int32_t m = (Q - 1) / alpha;
    for (int i = 0; i < N; i++) {
        int32_t r1, r0;
        decompose(w->c[i], alpha, &r1, &r0);
        if (h->c[i] == 0) { r->c[i] = r1; continue; }
        if (r0 > 0) { int32_t v = r1 + 1; r->c[i] = (v == m) ? 0 : v; }
        else        { int32_t v = r1 - 1; r->c[i] = (v < 0) ? m - 1 : v; }
    }
}

static int sum_hint(const poly *h) { int s = 0; for (int i = 0; i < N; i++) s += (int)h->c[i]; return s; }

/* |centered representative of x mod Q| >= b ? FIPS 204's InRange check used by
 * the signer's rejection loop and by Verify's z-norm check (Algorithm 7 lines
 * 15/19/23, Algorithm 8 line 6). Equivalent to utils.py's check_norm_bound,
 * whose XOR/shift form computes the same |centered| via a branch-free trick;
 * written directly here because the branch this feeds is inherent to the
 * algorithm's control flow regardless (see the file header's rejection-loop
 * note), so disguising this one comparison buys nothing. */
static int norm_ge(int32_t x, int32_t b)
{
    int32_t c = crd(modq(x), Q);
    if (c < 0) c = -c;
    return c >= b;
}
static int poly_norm_ge(const poly *p, int32_t b)
{ for (int i = 0; i < N; i++) if (norm_ge(p->c[i], b)) return 1; return 0; }

/* -------------------------------------------------------------- sampling */

/* ExpandA element, FIPS 204 Algorithm 32 (RejNTTPoly). Three bytes -> one
 * 23-bit candidate (masked with 0x7FFFFF), reject if >= Q -- unlike ML-KEM's
 * SampleNTT, which packs TWO candidates into three bytes, this is one
 * candidate per three bytes. Variable-time by design: it runs on rho, which
 * is public (it is the first 32 bytes of both pk and sk). THE BYTE ORDER,
 * see the file header: seed = rho || j || i, column then row. */
static void gen_a_row(poly row[MLDSA_L_MAX], const uint8_t rho[32], int i, int l)
{
    for (int j = 0; j < l; j++) {
        struct shake xof;
        uint8_t buf[168];
        int pos = 168, ctr = 0;

        shake128_init(&xof);
        shake_absorb(&xof, rho, 32);
#ifdef MLDSA_CTL_NO_TRANSPOSE
        /* NEGATIVE CONTROL: absorb (i, j) instead of (j, i). Our own keygen,
         * sign and verify all call this same function, so they stay
         * self-consistent with each other -- and interoperate with nothing,
         * exactly the ML-KEM NO_TRANSPOSE control's shape. */
        { uint8_t ij[2] = { (uint8_t)i, (uint8_t)j }; shake_absorb(&xof, ij, 2); }
#else
        { uint8_t ij[2] = { (uint8_t)j, (uint8_t)i }; shake_absorb(&xof, ij, 2); }
#endif
        shake_finalize(&xof);

        while (ctr < N) {
            if (pos > 168 - 3) { shake_squeeze(&xof, buf, 168); pos = 0; }
            uint32_t t = (uint32_t)buf[pos] | ((uint32_t)buf[pos + 1] << 8) | ((uint32_t)buf[pos + 2] << 16);
            t &= 0x7FFFFFu;
            pos += 3;
            if (t < (uint32_t)Q) row[j].c[ctr++] = (int32_t)t;
        }
    }
}

/* ExpandS element, FIPS 204 Algorithm 33 (RejBoundedPoly), for eta in {2,4}.
 * One byte yields two nibble candidates (low nibble first, matching j % 16
 * then j // 16 in the reference); eta=2 accepts nibbles < 15 as 2-(j%5),
 * eta=4 accepts nibbles < 9 as 4-j. rho_prime is not published (unlike ML-KEM
 * rho), but the rejection decision here depends only on the XOF byte's value
 * against a fixed threshold, not on the resulting COEFFICIENT -- the standard
 * argument (and the pq-crystals reference) treat this as safe to leave
 * variable-time; see the file header. */
static void sample_eta_poly(poly *r, const uint8_t rho_prime[64], int idx, int eta)
{
    struct shake xof;
    uint8_t ib[2] = { (uint8_t)(idx & 0xFF), (uint8_t)((idx >> 8) & 0xFF) };
    uint8_t buf[136];
    int pos = 136, ctr = 0;

    shake256_init(&xof);
    shake_absorb(&xof, rho_prime, 64);
    shake_absorb(&xof, ib, 2);
    shake_finalize(&xof);

    while (ctr < N) {
        if (pos >= 136) { shake_squeeze(&xof, buf, 136); pos = 0; }
        uint8_t b = buf[pos++];
        int lo = b & 0x0F, hi = b >> 4;
        if (eta == 2) { if (lo < 15) r->c[ctr++] = 2 - (lo % 5); }
        else          { if (lo < 9)  r->c[ctr++] = 4 - lo; }
        if (ctr >= N) break;
        if (eta == 2) { if (hi < 15) r->c[ctr++] = 2 - (hi % 5); }
        else          { if (hi < 9)  r->c[ctr++] = 4 - hi; }
    }
}

/* ExpandMask element, FIPS 204 Algorithm 34. NO rejection sampling here --
 * bit_count (18 for gamma1=2^17, 20 for gamma1=2^19) is chosen so that
 * 2*gamma1 is exactly 2^bit_count, so every bit_count-bit chunk maps to a
 * valid coefficient with no waste and no retry. */
static void expand_mask_poly(poly *r, const uint8_t rho_prime[64], int idx, int32_t gamma1)
{
    int bits = (gamma1 == (1 << 17)) ? 18 : 20;
    int nbytes = N * bits / 8;   /* 576 or 640 */
    uint8_t buf[640];
    struct shake xof;
    uint8_t ib[2] = { (uint8_t)(idx & 0xFF), (uint8_t)((idx >> 8) & 0xFF) };
    uint32_t v[N];

    shake256_init(&xof);
    shake_absorb(&xof, rho_prime, 64);
    shake_absorb(&xof, ib, 2);
    shake_finalize(&xof);
    shake_squeeze(&xof, buf, (size_t)nbytes);

    bitunpack(v, buf, bits);
    for (int i = 0; i < N; i++) r->c[i] = gamma1 - (int32_t)v[i];
}

/* SampleInBall, FIPS 204 Algorithm 29. A Fisher-Yates-style shuffle producing
 * tau coefficients of +-1 and 256-tau of 0. Reads 8 bytes of sign bits up
 * front, then one rejection byte per placed coefficient. */
static void sample_in_ball(poly *c, const uint8_t *seed, int seedlen, int tau)
{
    struct shake xof;
    uint8_t signbytes[8];
    uint64_t signs = 0;

    for (int i = 0; i < N; i++) c->c[i] = 0;

    shake256_init(&xof);
    shake_absorb(&xof, seed, (size_t)seedlen);
    shake_finalize(&xof);
    shake_squeeze(&xof, signbytes, 8);
    for (int b = 0; b < 8; b++) signs |= (uint64_t)signbytes[b] << (8 * b);

    for (int i = N - tau; i < N; i++) {
        uint8_t j;
        for (;;) { shake_squeeze(&xof, &j, 1); if (j <= i) break; }
        c->c[i] = c->c[j];
        c->c[j] = (int32_t)(1 - 2 * (int32_t)(signs & 1));
        signs >>= 1;
    }
}

/* --------------------------------------------------------- pk/sk/sig I/O */

static int s_bits(int eta) { return eta == 2 ? 3 : 4; }
static int s_bytes(int eta) { return eta == 2 ? 96 : 128; }
static int z_bits(int32_t gamma1) { return gamma1 == (1 << 17) ? 18 : 20; }
static int z_bytes(int32_t gamma1) { return gamma1 == (1 << 17) ? 576 : 640; }
static int w_bits(int32_t gamma2) { return gamma2 == (Q - 1) / 88 ? 6 : 4; }
static int w_bytes(int32_t gamma2) { return gamma2 == (Q - 1) / 88 ? 192 : 128; }

static void pack_t1(uint8_t out[320], const poly *t1)
{ uint32_t v[N]; for (int i = 0; i < N; i++) v[i] = (uint32_t)t1->c[i]; bitpack(out, v, 10); }
static void unpack_t1(poly *t1, const uint8_t in[320])
{ uint32_t v[N]; bitunpack(v, in, 10); for (int i = 0; i < N; i++) t1->c[i] = (int32_t)v[i]; }

static void pack_t0(uint8_t out[416], const poly *t0)
{ uint32_t v[N]; for (int i = 0; i < N; i++) v[i] = (uint32_t)((1 << 12) - t0->c[i]); bitpack(out, v, 13); }
static void unpack_t0(poly *t0, const uint8_t in[416])
{ uint32_t v[N]; bitunpack(v, in, 13); for (int i = 0; i < N; i++) t0->c[i] = (1 << 12) - (int32_t)v[i]; }

static void pack_s(uint8_t *out, const poly *s, int eta)
{ uint32_t v[N]; for (int i = 0; i < N; i++) v[i] = (uint32_t)(eta - s->c[i]); bitpack(out, v, s_bits(eta)); }
static void unpack_s(poly *s, const uint8_t *in, int eta)
{ uint32_t v[N]; bitunpack(v, in, s_bits(eta)); for (int i = 0; i < N; i++) s->c[i] = eta - (int32_t)v[i]; }

static void pack_w(uint8_t *out, const poly *w1, int32_t gamma2)
{ uint32_t v[N]; for (int i = 0; i < N; i++) v[i] = (uint32_t)w1->c[i]; bitpack(out, v, w_bits(gamma2)); }

/* z is packed as (gamma1 - centered(z)) -- centre it first (crd against Q)
 * so the subtraction matches utils.py's mod-q formula for every VALID
 * (rejection-loop-accepted) z without needing an explicit modular
 * wraparound: a valid z always has gamma1 - centered(z) in [0, 2*gamma1). */
static void pack_z(uint8_t *out, const poly *z, int32_t gamma1)
{
    uint32_t v[N];
    for (int i = 0; i < N; i++) v[i] = (uint32_t)(gamma1 - crd(z->c[i], Q));
    bitpack(out, v, z_bits(gamma1));
}
static void unpack_z(poly *z, const uint8_t *in, int32_t gamma1)
{
    uint32_t v[N];
    bitunpack(v, in, z_bits(gamma1));
    for (int i = 0; i < N; i++) z->c[i] = gamma1 - (int32_t)v[i];
}

/* HintBitPack / HintBitUnpack, FIPS 204 Algorithms 20-21: a sparse encoding
 * of up to omega coefficient positions (each < 256, one byte) across k
 * polynomials, followed by k cumulative-count bytes. Unpack validates
 * EVERY structural invariant the spec requires (monotone offsets, strictly
 * increasing positions within a polynomial, the trailing pad all-zero) and
 * fails closed -- a hint that decodes to "accept anyway" is a second,
 * unaudited, verifier. */
static void pack_h(uint8_t *out, const poly h[MLDSA_K_MAX], int k, int omega)
{
    int pos = 0;
    uint8_t offsets[MLDSA_K_MAX];
    for (int i = 0; i < k; i++) {
        for (int j = 0; j < N; j++) if (h[i].c[j]) out[pos++] = (uint8_t)j;
        offsets[i] = (uint8_t)pos;
    }
    for (; pos < omega; pos++) out[pos] = 0;
    for (int i = 0; i < k; i++) out[omega + i] = offsets[i];
}
static int unpack_h(poly h[MLDSA_K_MAX], const uint8_t *in, int k, int omega)
{
    int prev = 0;
    for (int i = 0; i < k; i++) {
        int cur = in[omega + i];
        if (cur < prev || cur > omega) return -1;
        for (int j = 0; j < N; j++) h[i].c[j] = 0;
        int last = -1;
        for (int t = prev; t < cur; t++) {
            int posn = in[t];
            if (posn <= last) return -1;
            h[i].c[posn] = 1;
            last = posn;
        }
        prev = cur;
    }
    for (int t = prev; t < omega; t++) if (in[t] != 0) return -1;
    return 0;
}

/* ------------------------------------------------------------- H helper */

/* H, FIPS 204's own name for SHAKE256 used as an arbitrary-length hash
 * (Section 3.4). Three-piece incremental form because every call site here
 * hashes a concatenation (tr||mprime, K||rnd||mu, mu||w1_bytes) and none of
 * those pairs live in one contiguous buffer. */
static void h3(uint8_t *out, size_t outlen,
               const uint8_t *a, size_t alen,
               const uint8_t *b, size_t blen,
               const uint8_t *c, size_t clen)
{
    struct shake xof;
    shake256_init(&xof);
    if (alen) shake_absorb(&xof, a, alen);
    if (blen) shake_absorb(&xof, b, blen);
    if (clen) shake_absorb(&xof, c, clen);
    shake_finalize(&xof);
    shake_squeeze(&xof, out, outlen);
}

/* ------------------------------------------------------------- KeyGen */

void mldsa_keygen_internal(const mldsa_params *p, const uint8_t xi[32],
                            uint8_t *pk, uint8_t *sk)
{
    uint8_t seedbuf[128], rho[32], rho_prime[64], Kseed[32];
    poly s1[MLDSA_L_MAX], s2[MLDSA_K_MAX], t0[MLDSA_K_MAX], t1[MLDSA_K_MAX];
    poly s1_hat[MLDSA_L_MAX];
    poly row[MLDSA_L_MAX];

    /* H(xi || k || l, 128) -- FIPS 204 Algorithm 6 lines 1-2. The trailing
     * domain-separation bytes bind the seed expansion to the parameter set,
     * same role as ML-KEM's G(d||k): see MLDSA_CTL_NO_KL_DOMAIN. */
    {
        uint8_t in[34];
        cp(in, xi, 32);
        in[32] = (uint8_t)p->k;
        in[33] = (uint8_t)p->l;
#ifdef MLDSA_CTL_NO_KL_DOMAIN
        /* NEGATIVE CONTROL: H(xi) instead of H(xi || k || l). Self-consistent
         * (our own keygen and, transitively, sign/verify all derive from the
         * same wrong rho/rho'/K), and produces different keys than any
         * official implementation for the same seed. */
        shake256(seedbuf, sizeof seedbuf, in, 32);
#else
        shake256(seedbuf, sizeof seedbuf, in, 34);
#endif
        wipe(in, sizeof in);
    }
    cp(rho, seedbuf, 32);
    cp(rho_prime, seedbuf + 32, 64);
    cp(Kseed, seedbuf + 96, 32);
    wipe(seedbuf, sizeof seedbuf);

    for (int i = 0; i < p->l; i++) sample_eta_poly(&s1[i], rho_prime, i, p->eta);
    for (int i = 0; i < p->k; i++) sample_eta_poly(&s2[i], rho_prime, p->l + i, p->eta);

    for (int i = 0; i < p->l; i++) { s1_hat[i] = s1[i]; ntt(&s1_hat[i]); }

    /* t = A.s1 + s2, one row of A at a time (never held whole -- same
     * discipline as mlkem.c's pke_keygen/pke_encrypt, for the same reason:
     * k*l can be up to 8*7=56 polynomials, 56 KiB, not worth holding live
     * when each row is used once). */
    for (int i = 0; i < p->k; i++) {
        poly acc, tmp;
        gen_a_row(row, rho, i, p->l);
        poly_pmul(&acc, &row[0], &s1_hat[0]);
        for (int j = 1; j < p->l; j++) { poly_pmul(&tmp, &row[j], &s1_hat[j]); poly_add(&acc, &acc, &tmp); }
        invntt(&acc);
        poly_add(&t1[i], &acc, &s2[i]);       /* t1[i] temporarily holds t, split below */
    }
    for (int i = 0; i < p->k; i++) { poly t = t1[i]; poly_power2round(&t, &t1[i], &t0[i]); }

    cp(pk, rho, 32);
    for (int i = 0; i < p->k; i++) pack_t1(pk + 32 + i * 320, &t1[i]);

    uint8_t tr[64];
    shake256(tr, 64, pk, (size_t)p->pk_bytes);

    cp(sk, rho, 32);
    cp(sk + 32, Kseed, 32);
    cp(sk + 64, tr, 64);
    {
        int sb = s_bytes(p->eta);
        uint8_t *w = sk + 128;
        for (int i = 0; i < p->l; i++) { pack_s(w, &s1[i], p->eta); w += sb; }
        for (int i = 0; i < p->k; i++) { pack_s(w, &s2[i], p->eta); w += sb; }
        for (int i = 0; i < p->k; i++) { pack_t0(w, &t0[i]); w += 416; }
    }

    wipe(rho_prime, sizeof rho_prime);
    wipe(s1, sizeof s1); wipe(s2, sizeof s2); wipe(s1_hat, sizeof s1_hat);
    wipe(t0, sizeof t0);   /* t0 is part of the secret key (2026-09-16 audit) */
}

/* --------------------------------------------------------------- Sign */

int mldsa_sign_internal(const mldsa_params *p, const uint8_t *sk,
                         const uint8_t *mprime, size_t mplen,
                         const uint8_t rnd[32], uint8_t *sig)
{
    const uint8_t *rho = sk, *Kseed = sk + 32, *tr = sk + 64;
    int sb = s_bytes(p->eta);
    const uint8_t *s1_bytes = sk + 128;
    const uint8_t *s2_bytes = s1_bytes + p->l * sb;
    const uint8_t *t0_bytes = s2_bytes + p->k * sb;

    poly s1_hat[MLDSA_L_MAX], s2_hat[MLDSA_K_MAX], t0_hat[MLDSA_K_MAX];
    for (int i = 0; i < p->l; i++) { unpack_s(&s1_hat[i], s1_bytes + i * sb, p->eta); ntt(&s1_hat[i]); }
    for (int i = 0; i < p->k; i++) { unpack_s(&s2_hat[i], s2_bytes + i * sb, p->eta); ntt(&s2_hat[i]); }
    for (int i = 0; i < p->k; i++) { unpack_t0(&t0_hat[i], t0_bytes + i * 416); ntt(&t0_hat[i]); }

    uint8_t mu[64];
    h3(mu, 64, tr, 64, mprime, mplen, 0, 0);
    uint8_t rho_prime[64];
    h3(rho_prime, 64, Kseed, 32, rnd, 32, mu, 64);

    int32_t alpha = 2 * p->gamma2;
    int kappa = 0;
    int zb = z_bytes(p->gamma1);
    int wb = w_bytes(p->gamma2);

    for (int attempt = 0; attempt < MLDSA_MAX_SIGN_ATTEMPTS; attempt++) {
        poly y[MLDSA_L_MAX], y_hat[MLDSA_L_MAX], w[MLDSA_K_MAX], w1[MLDSA_K_MAX], row[MLDSA_L_MAX];
        uint8_t w1bytes[MLDSA_K_MAX * 192], c_tilde[64];

        for (int i = 0; i < p->l; i++) {
            expand_mask_poly(&y[i], rho_prime, kappa + i, p->gamma1);
            y_hat[i] = y[i];
            ntt(&y_hat[i]);
        }
        kappa += p->l;

        /* w = NTT^-1(A.y_hat). A is regenerated per attempt rather than
         * cached: correctness-first for a first cut, see the file header's
         * stack note -- caching all k*l rows (up to 56 KiB) trades a stack
         * budget nothing currently constrains for a speed nothing currently
         * measures, since this code has no caller yet. */
        for (int i = 0; i < p->k; i++) {
            poly acc, tmp;
            gen_a_row(row, rho, i, p->l);
            poly_pmul(&acc, &row[0], &y_hat[0]);
            for (int j = 1; j < p->l; j++) { poly_pmul(&tmp, &row[j], &y_hat[j]); poly_add(&acc, &acc, &tmp); }
            invntt(&acc);
            w[i] = acc;
            poly_high_bits(&w1[i], &w[i], alpha);
            pack_w(w1bytes + i * wb, &w1[i], p->gamma2);
        }

        h3(c_tilde, (size_t)p->c_tilde_bytes, mu, 64, w1bytes, (size_t)(p->k * wb), 0, 0);

        poly c, c_hat;
        sample_in_ball(&c, c_tilde, p->c_tilde_bytes, p->tau);
        c_hat = c; ntt(&c_hat);

        poly z[MLDSA_L_MAX];
        int reject = 0;
        for (int i = 0; i < p->l; i++) {
            poly cs1;
            poly_pmul(&cs1, &c_hat, &s1_hat[i]); invntt(&cs1);
            poly_add(&z[i], &y[i], &cs1);
            /* THE >= IS THE SPEC, NOT A ROUNDING CHOICE (FIPS 204 Alg.7
             * line 15): a coefficient sitting exactly at the bound must
             * reject, and comparing with > instead would accept it -- a
             * signature this file's own verifier would still pass (it uses
             * the same wrong bound) and every other implementation would
             * refuse. See MLDSA_CTL_WEAK_ZBOUND below and the file header. */
            if (poly_norm_ge(&z[i], p->gamma1 - p->beta)) reject = 1;
        }
        if (reject) { wipe(y, sizeof y); wipe(y_hat, sizeof y_hat); continue; }
        if (reject) { wipe(y, sizeof y); wipe(y_hat, sizeof y_hat); continue; }

        poly h[MLDSA_K_MAX];
        int total_hint = 0;
        for (int i = 0; i < p->k; i++) {
            poly cs2, low, ct0, low_plus_ct0, r0;
            poly_pmul(&cs2, &c_hat, &s2_hat[i]); invntt(&cs2);
            poly_sub(&low, &w[i], &cs2);                     /* w - c.s2 */
            poly_low_bits(&r0, &low, alpha);
#ifdef MLDSA_CTL_WEAK_ZBOUND
            /* NEGATIVE CONTROL: drop the r0 rejection entirely (as if it
             * always passed). Rare to trigger per attempt (the r0 rejection
             * probability is small relative to the z one), but over the many
             * attempts a KAT/differential run makes it fires and produces a
             * signature that fails elsewhere -- exactly the "signs but does
             * not decompose the same way" trap the task brief names. */
#else
            if (poly_norm_ge(&r0, p->gamma2 - p->beta)) { reject = 1; break; }
#endif
            poly_pmul(&ct0, &c_hat, &t0_hat[i]); invntt(&ct0);
            if (poly_norm_ge(&ct0, p->gamma2)) { reject = 1; break; }

            poly_add(&low_plus_ct0, &low, &ct0);              /* w - c.s2 + c.t0 */
            poly_make_hint(&h[i], &low, &low_plus_ct0, alpha);
            total_hint += sum_hint(&h[i]);
        }
        if (reject) { wipe(y, sizeof y); wipe(y_hat, sizeof y_hat); continue; }
        if (total_hint > p->omega) { wipe(y, sizeof y); wipe(y_hat, sizeof y_hat); continue; }

        cp(sig, c_tilde, (size_t)p->c_tilde_bytes);
        for (int i = 0; i < p->l; i++) pack_z(sig + p->c_tilde_bytes + i * zb, &z[i], p->gamma1);
        pack_h(sig + p->c_tilde_bytes + p->l * zb, h, p->k, p->omega);

        wipe(y, sizeof y); wipe(y_hat, sizeof y_hat);   /* mask dies with the sig */
        wipe(s1_hat, sizeof s1_hat); wipe(s2_hat, sizeof s2_hat); wipe(t0_hat, sizeof t0_hat); wipe(rho_prime, sizeof rho_prime);
        return 0;
    }
    wipe(s1_hat, sizeof s1_hat); wipe(s2_hat, sizeof s2_hat); wipe(t0_hat, sizeof t0_hat); wipe(rho_prime, sizeof rho_prime);
    return -1;
}

/* ------------------------------------------------------------- Verify */

int mldsa_verify_internal(const mldsa_params *p, const uint8_t *pk,
                           const uint8_t *mprime, size_t mplen,
                           const uint8_t *sig)
{
    const uint8_t *rho = pk;
    poly t1[MLDSA_K_MAX];
    for (int i = 0; i < p->k; i++) unpack_t1(&t1[i], pk + 32 + i * 320);

    int zb = z_bytes(p->gamma1);
    const uint8_t *c_tilde = sig;
    const uint8_t *zbuf = sig + p->c_tilde_bytes;
    const uint8_t *hbuf = zbuf + p->l * zb;

    poly z[MLDSA_L_MAX];
    for (int i = 0; i < p->l; i++) unpack_z(&z[i], zbuf + i * zb, p->gamma1);

    poly h[MLDSA_K_MAX];
    if (unpack_h(h, hbuf, p->k, p->omega) != 0) return 0;   /* malformed hint encoding */

    for (int i = 0; i < p->l; i++) if (poly_norm_ge(&z[i], p->gamma1 - p->beta)) return 0;

    uint8_t tr[64];
    shake256(tr, 64, pk, (size_t)p->pk_bytes);
    uint8_t mu[64];
    h3(mu, 64, tr, 64, mprime, mplen, 0, 0);

    poly c, c_hat;
    sample_in_ball(&c, c_tilde, p->c_tilde_bytes, p->tau);
    c_hat = c; ntt(&c_hat);

    poly z_hat[MLDSA_L_MAX];
    for (int i = 0; i < p->l; i++) { z_hat[i] = z[i]; ntt(&z_hat[i]); }

    poly t1_hat[MLDSA_K_MAX];
    for (int i = 0; i < p->k; i++) { poly_scale(&t1_hat[i], &t1[i], 1 << D_BITS); ntt(&t1_hat[i]); }

    int wb = w_bytes(p->gamma2);
    uint8_t w1bytes[MLDSA_K_MAX * 192];
    poly row[MLDSA_L_MAX];
    for (int i = 0; i < p->k; i++) {
        poly acc, tmp, ct1, diff, w1i;
        gen_a_row(row, rho, i, p->l);
        poly_pmul(&acc, &row[0], &z_hat[0]);
        for (int j = 1; j < p->l; j++) { poly_pmul(&tmp, &row[j], &z_hat[j]); poly_add(&acc, &acc, &tmp); }
        poly_pmul(&ct1, &c_hat, &t1_hat[i]);
        poly_sub(&diff, &acc, &ct1);
        invntt(&diff);
        poly_use_hint(&w1i, &h[i], &diff, 2 * p->gamma2);
        pack_w(w1bytes + i * wb, &w1i, p->gamma2);
    }

    uint8_t c_tilde2[64];
    h3(c_tilde2, (size_t)p->c_tilde_bytes, mu, 64, w1bytes, (size_t)(p->k * wb), 0, 0);

    for (int i = 0; i < p->c_tilde_bytes; i++) if (c_tilde[i] != c_tilde2[i]) return 0;
    return 1;
}

/* --------------------------------------------------- context wrapping */

#define MLDSA_MPRIME_MAX 16384

int mldsa_sign(const mldsa_params *p, const uint8_t *sk,
               const uint8_t *msg, size_t mlen,
               const uint8_t *ctx, size_t ctxlen,
               const uint8_t rnd[32], uint8_t *sig)
{
    if (ctxlen > 255) return -1;
    if (2 + ctxlen + mlen > MLDSA_MPRIME_MAX) return -1;
    uint8_t mprime[MLDSA_MPRIME_MAX];
    mprime[0] = 0x00;
    mprime[1] = (uint8_t)ctxlen;
    cp(mprime + 2, ctx, (int)ctxlen);
    cp(mprime + 2 + ctxlen, msg, (int)mlen);
    int rc = mldsa_sign_internal(p, sk, mprime, 2 + ctxlen + mlen, rnd, sig);
    wipe(mprime, (int)(2 + ctxlen + mlen));
    return rc;
}

int mldsa_verify(const mldsa_params *p, const uint8_t *pk,
                  const uint8_t *msg, size_t mlen,
                  const uint8_t *ctx, size_t ctxlen,
                  const uint8_t *sig)
{
    if (ctxlen > 255) return 0;
    if (2 + ctxlen + mlen > MLDSA_MPRIME_MAX) return 0;
    uint8_t mprime[MLDSA_MPRIME_MAX];
    mprime[0] = 0x00;
    mprime[1] = (uint8_t)ctxlen;
    cp(mprime + 2, ctx, (int)ctxlen);
    cp(mprime + 2 + ctxlen, msg, (int)mlen);
    return mldsa_verify_internal(p, pk, mprime, 2 + ctxlen + mlen, sig);
}

