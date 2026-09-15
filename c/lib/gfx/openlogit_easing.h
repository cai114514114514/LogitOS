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

#endif
