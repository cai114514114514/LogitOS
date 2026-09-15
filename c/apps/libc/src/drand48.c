/* POSIX/SVID 48-bit linear-congruential generator family.  This is the
 * historical deterministic API, intentionally separate from random.c's
 * cryptographic source: seed48/lcong48 promise reproducibility. */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define MASK48 ((1ULL << 48) - 1)
#define A48 0x5deece66dULL
#define C48 0xbU

static struct drand48_data global_state = {
    { 0x330e, 0xabcd, 0x1234 }, { 0, 0, 0 }, C48, 1, A48
};
static unsigned short seed48_old[3];

static uint64_t pack48(const unsigned short x[3])
{ return (uint64_t)x[0] | ((uint64_t)x[1] << 16) | ((uint64_t)x[2] << 32); }
static void unpack48(uint64_t x, unsigned short out[3])
{ out[0] = (unsigned short)x; out[1] = (unsigned short)(x >> 16); out[2] = (unsigned short)(x >> 32); }
static void init_params(struct drand48_data *s)
{ if (!s->__init) { s->__a = A48; s->__c = C48; s->__init = 1; } }
static uint64_t step(unsigned short x[3], struct drand48_data *s)
{
    init_params(s);
    uint64_t v = (pack48(x) * s->__a + s->__c) & MASK48;
    unpack48(v, x);
    return v;
}
static double as_double(uint64_t x) { return (double)x / 281474976710656.0; }

int drand48_r(struct drand48_data *s, double *result)
{ if (!s || !result) return -1; *result = as_double(step(s->__x, s)); return 0; }
int erand48_r(unsigned short x[3], struct drand48_data *s, double *result)
{ if (!x || !s || !result) return -1; *result = as_double(step(x, s)); return 0; }
int lrand48_r(struct drand48_data *s, long *result)
{ if (!s || !result) return -1; *result = (long)(step(s->__x, s) >> 17); return 0; }
int nrand48_r(unsigned short x[3], struct drand48_data *s, long *result)
{ if (!x || !s || !result) return -1; *result = (long)(step(x, s) >> 17); return 0; }
int mrand48_r(struct drand48_data *s, long *result)
{ if (!s || !result) return -1; *result = (long)(int32_t)(step(s->__x, s) >> 16); return 0; }
int jrand48_r(unsigned short x[3], struct drand48_data *s, long *result)
{ if (!x || !s || !result) return -1; *result = (long)(int32_t)(step(x, s) >> 16); return 0; }

double drand48(void) { double r; drand48_r(&global_state, &r); return r; }
double erand48(unsigned short x[3]) { double r; erand48_r(x, &global_state, &r); return r; }
long lrand48(void) { long r; lrand48_r(&global_state, &r); return r; }
long nrand48(unsigned short x[3]) { long r; nrand48_r(x, &global_state, &r); return r; }
long mrand48(void) { long r; mrand48_r(&global_state, &r); return r; }
long jrand48(unsigned short x[3]) { long r; jrand48_r(x, &global_state, &r); return r; }

int srand48_r(long seed, struct drand48_data *s)
{
    if (!s) return -1;
    memset(s, 0, sizeof *s);
    s->__x[0] = 0x330e; s->__x[1] = (unsigned short)seed;
    s->__x[2] = (unsigned short)((unsigned long)seed >> 16);
    s->__a = A48; s->__c = C48; s->__init = 1;
    return 0;
}
void srand48(long seed) { (void)srand48_r(seed, &global_state); }

int seed48_r(unsigned short seed[3], struct drand48_data *s)
{
    if (!seed || !s) return -1;
    init_params(s);
    memcpy(s->__old_x, s->__x, sizeof s->__x);
    memcpy(s->__x, seed, sizeof s->__x);
    s->__a = A48; s->__c = C48;
    return 0;
}
unsigned short *seed48(unsigned short seed[3])
{
    memcpy(seed48_old, global_state.__x, sizeof seed48_old);
    if (seed48_r(seed, &global_state)) return NULL;
    return seed48_old;
}

int lcong48_r(unsigned short param[7], struct drand48_data *s)
{
    if (!param || !s) return -1;
    memcpy(s->__x, param, 3 * sizeof(unsigned short));
    s->__a = pack48(param + 3); s->__c = param[6]; s->__init = 1;
    return 0;
}
void lcong48(unsigned short param[7]) { (void)lcong48_r(param, &global_state); }
