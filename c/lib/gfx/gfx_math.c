/* Open Logit -- integer arithmetic the rest of the engine stands on.
 *
 * Everything here is exact integer work. There is no float in the drawing path
 * at all, for the same reason the kernel's glyph rasterizer has none: the two
 * have to agree pixel for pixel where a shape meets the type sitting on it, and
 * "close enough in double" is not the same rule as "exact in 24.8". */
#include "gfx.h"

/* memset with no libc. The volatile pointer is not decoration: at -O2 clang's
 * loop-idiom pass rewrites a plain byte-store loop into a call to memset, and
 * six GUI apps link crt0 + logit.h and nothing else, so that call does not
 * resolve. Writing through volatile is what forbids the rewrite. */
void gfx_zero(void *p, int n)
{
    volatile unsigned char *q = (volatile unsigned char *)p;
    while (n-- > 0) *q++ = 0;
}

/* Restoring (digit-by-digit) integer square root over 64 bits. `bit` must start
 * at a power of FOUR or the restoring step is off by a shift -- the same note is
 * on the two copies this replaces. */
unsigned long gfx_isqrt(unsigned long long v)
{
    unsigned long long rem = v, root = 0, bit = 1ULL << 62;
    while (bit > rem) bit >>= 2;
    while (bit) {
        if (rem >= root + bit) { rem -= root + bit; root = (root >> 1) + bit; }
        else root >>= 1;
        bit >>= 2;
    }
    return (unsigned long)root;
}

/* Quarter-wave sine, 16.16, one entry per degree 0..90 inclusive. Rotation is
 * the only place the engine needs a transcendental, and a table plus linear
 * interpolation is accurate to about 1.5e-4 -- a twentieth of a 24.8 coordinate
 * step at a radius of 1000 px, i.e. below what the rasterizer can express. */
static const int sin_q[91] = {   /* int, not unsigned short: the last entry IS 65536 */
        0,  1144,  2287,  3430,  4572,  5712,  6850,  7987,  9121, 10252,
    11380, 12505, 13626, 14742, 15855, 16962, 18064, 19161, 20252, 21336,
    22415, 23486, 24550, 25607, 26656, 27697, 28729, 29753, 30767, 31772,
    32768, 33754, 34729, 35693, 36647, 37590, 38521, 39441, 40348, 41243,
    42126, 42995, 43852, 44695, 45525, 46341, 47143, 47930, 48703, 49461,
    50203, 50931, 51643, 52339, 53020, 53684, 54332, 54963, 55578, 56175,
    56756, 57319, 57865, 58393, 58903, 59396, 59870, 60326, 60764, 61183,
    61584, 61966, 62328, 62672, 62997, 63303, 63589, 63856, 64104, 64332,
    64540, 64729, 64898, 65048, 65177, 65287, 65376, 65446, 65496, 65526,
    65536
};

/* sin of an angle given in 24.8 DEGREES, returned 16.16. */
int gfx_sin(int deg256)
{
    int neg = 0;
    long long a = deg256;
    long long full = 360 * (long long)GFX_ONE;
    a %= full;
    if (a < 0) a += full;
    if (a >= full / 2) { a -= full / 2; neg = 1; }      /* sin(x+180) = -sin(x) */
    if (a > full / 4) a = full / 2 - a;                 /* sin(180-x) =  sin(x) */
    int whole = (int)(a >> 8), frac = (int)(a & 255);
    if (whole > 89) { whole = 89; frac = 256; }
    int lo = sin_q[whole], hi = sin_q[whole + 1];
    int v = lo + (hi - lo) * frac / 256;
    return neg ? -v : v;
}

int gfx_cos(int deg256) { return gfx_sin(deg256 + 90 * GFX_ONE); }

/* ------------------------------------------------------------- matrices -- */

void gfx_m_identity(struct gfx_matrix *m)
{
    m->a = GFX_MONE; m->b = 0; m->c = 0; m->d = GFX_MONE; m->e = 0; m->f = 0;
}

void gfx_m_set(struct gfx_matrix *m, int a, int b, int c, int d, int e, int f)
{
    m->a = a; m->b = b; m->c = c; m->d = d; m->e = e; m->f = f;
}

/* o = l * r: apply r first, then l. Written into a temporary because callers
 * legitimately pass o == l or o == r while composing a CTM. */
void gfx_m_mul(struct gfx_matrix *o, const struct gfx_matrix *l, const struct gfx_matrix *r)
{
    struct gfx_matrix t;
    t.a = (int)(((long long)l->a * r->a + (long long)l->c * r->b) >> 16);
    t.b = (int)(((long long)l->b * r->a + (long long)l->d * r->b) >> 16);
    t.c = (int)(((long long)l->a * r->c + (long long)l->c * r->d) >> 16);
    t.d = (int)(((long long)l->b * r->c + (long long)l->d * r->d) >> 16);
    /* r's translation is in 24.8 and passes through l's linear part; l's own
     * translation is already in device 24.8 and is simply added. */
    t.e = (int)(((long long)l->a * r->e + (long long)l->c * r->f) >> 16) + l->e;
    t.f = (int)(((long long)l->b * r->e + (long long)l->d * r->f) >> 16) + l->f;
    *o = t;
}

void gfx_m_translate(struct gfx_matrix *m, int dx, int dy)
{
    struct gfx_matrix t;
    gfx_m_identity(&t); t.e = dx; t.f = dy;
    gfx_m_mul(m, m, &t);
}

void gfx_m_scale(struct gfx_matrix *m, int sx, int sy)
{
    struct gfx_matrix t;
    gfx_m_identity(&t); t.a = sx; t.d = sy;
    gfx_m_mul(m, m, &t);
}

void gfx_m_rotate(struct gfx_matrix *m, int deg256)
{
    struct gfx_matrix t;
    int s = gfx_sin(deg256), c = gfx_cos(deg256);
    gfx_m_set(&t, c, s, -s, c, 0, 0);
    gfx_m_mul(m, m, &t);
}

void gfx_m_apply(const struct gfx_matrix *m, int x, int y, int *ox, int *oy)
{
    if (ox) *ox = (int)(((long long)m->a * x + (long long)m->c * y) >> 16) + m->e;
    if (oy) *oy = (int)(((long long)m->b * x + (long long)m->d * y) >> 16) + m->f;
}

/* Inverse, or 0 for a singular matrix. The determinant is computed in 64 bits
 * and kept at 16.16; a matrix whose determinant rounds to zero at that scale is
 * refused rather than producing coordinates that wrap. */
int gfx_m_invert(struct gfx_matrix *o, const struct gfx_matrix *m)
{
    long long det = ((long long)m->a * m->d - (long long)m->b * m->c) >> 16;
    if (det == 0) return 0;
    struct gfx_matrix t;
    /* `* 65536` rather than `<< 16`: a matrix coefficient is signed and any
     * rotation or flip makes one of these negative, and left-shifting a
     * negative signed value is undefined behaviour. Same instruction, same
     * fix as gfx_raster.c's add_edge -- see the comment there for how the
     * class was found. */
    t.a = (int)(((long long)m->d * 65536) / det);
    t.b = (int)((-(long long)m->b * 65536) / det);
    t.c = (int)((-(long long)m->c * 65536) / det);
    t.d = (int)(((long long)m->a * 65536) / det);
    /* Translation of the inverse: -(A^-1) * t. */
    t.e = (int)(-(((long long)t.a * m->e + (long long)t.c * m->f) >> 16));
    t.f = (int)(-(((long long)t.b * m->e + (long long)t.d * m->f) >> 16));
    *o = t;
    return 1;
}

/* An approximate uniform scale: the geometric mean of the two axis lengths,
 * i.e. sqrt(|det|). Used only to pick a flattening tolerance, where being a few
 * percent out costs a segment, not a pixel. */
int gfx_m_scale_of(const struct gfx_matrix *m)
{
    long long det = ((long long)m->a * m->d - (long long)m->b * m->c) >> 16;
    if (det < 0) det = -det;
    /* det is 16.16; sqrt of (det << 16) is the 16.16 root. */
    return (int)gfx_isqrt((unsigned long long)det << 16);
}

/* ---------------------------------------------------------------- colour -- */
unsigned gfx_mix(unsigned a, unsigned b, int t)
{
    int ar = GFX_R(a), ag = GFX_G(a), ab = GFX_B(a);
    int br = GFX_R(b), bg = GFX_G(b), bb = GFX_B(b);
    return GFX_RGB(ar + (br - ar) * t / 255,
                   ag + (bg - ag) * t / 255,
                   ab + (bb - ab) * t / 255);
}
