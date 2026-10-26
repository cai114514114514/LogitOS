#ifndef OPENLOGIT_EASING_H
#define OPENLOGIT_EASING_H
/* Header-only numerical primitive so font/layout host probes need no runtime
 * or allocator just to share CSS easing with the SDK. */
static inline int ol_ease_finite(double x){return x==x && x<=1.7976931348623157e308 && x>=-1.7976931348623157e308;}
/* Invert x with a safeguarded Newton bracket. Keep double precision here:
 * CSS transforms amplify a small easing error into visible displacement.
 * Unlike a fixed small iteration count, the residual is the accuracy bound. */
static inline double ol_bezier_poly(double t,double p,double q)
{double u=1-t;return 3*u*u*t*p+3*u*t*t*q+t*t*t;}
static inline double ol_ease_bezier(double x1,double y1,double x2,double y2,double t)
{
    if(t<=0)return 0;if(t>=1)return 1;
    if(!ol_ease_finite(t)||!ol_ease_finite(x1)||!ol_ease_finite(x2)||!ol_ease_finite(y1)||!ol_ease_finite(y2)||x1<0||x1>1||x2<0||x2>1)return t;
    double lo=0,hi=1,x=t;
    for(int i=0;i<100;i++) {
        double residual=ol_bezier_poly(x,x1,x2)-t;
        if(residual>-1e-12 && residual<1e-12)break;
        if(residual<0)lo=x;else hi=x;
        double u=1-x,dx=3*u*u*x1+6*u*x*(x2-x1)+3*x*x*(1-x2);
        double next=dx>1e-12?x-residual/dx:(lo+hi)*.5;
        x=next>lo&&next<hi?next:(lo+hi)*.5;
    }
    return ol_bezier_poly(x,y1,y2);
}
/* Integer (16.16) counterpart for ol_ease256_ex: same safeguarded Newton as
 * above, but every step is integer arithmetic so ring 3 and the kernel agree
 * bit for bit. x1,y1,x2,y2 and t are 16.16 (65536 = 1.0); the result is 16.16
 * and may overshoot [0,1] through y1/y2. x1,x2 outside [0,1] yield linear. */
static inline int ol_fx_bez_poly(long long s, long long p, long long q)
{
    long long u = 65536 - s, uu = (u * u) >> 16, b = (uu * s) >> 16;
    long long us = (u * s) >> 16, d = (us * s) >> 16, e = (((s * s) >> 16) * s) >> 16;
    return (int)(((3 * b * p) >> 16) + ((3 * d * q) >> 16) + e);
}
static inline int ol_ease_bezier_fx(int x1, int y1, int x2, int y2, int t)
{
    if (t <= 0)
        return 0;
    if (t >= 65536)
        return 65536;
    if (x1 < 0 || x1 > 65536 || x2 < 0 || x2 > 65536)
        return t;
    long long lo = 0, hi = 65536, s = t;
    for (int i = 0; i < 64; i++) {
        long long r = ol_fx_bez_poly(s, x1, x2) - t;
        if (r >= -1 && r <= 1)
            break;
        if (r < 0)
            lo = s;
        else
            hi = s;
        long long u = 65536 - s;
        long long dx = (3 * ((u * u) >> 16) * x1 >> 16) + (6 * ((u * s) >> 16) * (x2 - (long long)x1) >> 16) +
                       (3 * ((s * s) >> 16) * (65536 - (long long)x2) >> 16);
        long long next = dx > 16 ? s - (r << 16) / dx : (lo + hi) >> 1;
        s = (next > lo && next < hi) ? next : (lo + hi) >> 1;
    }
    return ol_fx_bez_poly(s, y1, y2);
}
#endif
