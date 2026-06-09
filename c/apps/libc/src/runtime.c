/* 128-bit integer division compiler-rt helpers (libbf uses __int128 division;
 * a freestanding target has no compiler-rt). Binary long division. */
typedef unsigned __int128 u128;
typedef signed   __int128 s128;

static u128 udivmod(u128 n, u128 d, u128 *rem)
{
    u128 q = 0, r = 0;
    if (d == 0) { if (rem) *rem = 0; return ~(u128)0; }   /* avoid trap */
    for (int i = 127; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= ((u128)1 << i); }
    }
    if (rem) *rem = r;
    return q;
}

u128 __udivti3(u128 a, u128 b) { return udivmod(a, b, 0); }
u128 __umodti3(u128 a, u128 b) { u128 r; udivmod(a, b, &r); return r; }

s128 __divti3(s128 a, s128 b)
{
    int neg = 0; u128 ua, ub, q;
    if (a < 0) { ua = (u128)(-a); neg ^= 1; } else ua = (u128)a;
    if (b < 0) { ub = (u128)(-b); neg ^= 1; } else ub = (u128)b;
    q = udivmod(ua, ub, 0);
    return neg ? -(s128)q : (s128)q;
}
s128 __modti3(s128 a, s128 b)
{
    int neg = a < 0; u128 ua, ub, r;
    ua = a < 0 ? (u128)(-a) : (u128)a;
    ub = b < 0 ? (u128)(-b) : (u128)b;
    udivmod(ua, ub, &r);
    return neg ? -(s128)r : (s128)r;
}
