/* CSS Images 3 §§3.1–3.4; conic geometry from CSS Images 4 §3.3.
 * This module is textually included by renderer.c so the existing host source
 * lists keep one painter implementation. It can also compile alone for its
 * focused sampler test. There is no second color parser or stop algorithm. */
#include "gradient.h"
#include "../../../../../include/weaksym.h"
#include <math.h>
#include <string.h>

#ifndef PAINT_GRADIENT_EMBEDDED
extern int img_css_color(const char *, int, unsigned char *) LOGIT_WEAK;
LOGIT_WEAK_STUB(img_css_color);
#endif

#define GRAD_TAU 6.28318530717958647693
enum { GRAD_RADIAL_NORMAL, GRAD_RADIAL_MIRROR, GRAD_RADIAL_SOLID };

static double gradient_min(double a, double b) { return a < b ? a : b; }
static double gradient_max(double a, double b) { return a > b ? a : b; }
static double gradient_length(struct cg_length v, double basis)
{ return v.px + v.percent * basis; }

static int gradient_color(const struct cgstop *s, unsigned cur, int cur_alpha,
                           double rgba[4])
{
    unsigned char c[4];
    if (s->current_color) {
        c[0]=(unsigned char)(cur>>16); c[1]=(unsigned char)(cur>>8);
        c[2]=(unsigned char)cur; c[3]=(unsigned char)cur_alpha;
    } else if (!s->color || !LOGIT_HAVE(img_css_color) ||
               !img_css_color(s->color,s->colorlen,c)) return 0;
    for (int k=0;k<3;k++) {
#ifdef CSS_GRADIENT_NO_PREMULT
        rgba[k]=c[k]; /* Test-only old straight RGB interpolation. */
#else
        rgba[k]=(double)c[k]*c[3]/255.0;
#endif
    }
    rgba[3]=c[3];
    return 1;
}

static void gradient_radial_geometry(const struct cgradient *s, double w,
                                     double h, struct paint_gradient *p)
{
    double left=fabs(p->cx), right=fabs(w-p->cx);
    double top=fabs(p->cy), bottom=fabs(h-p->cy);
    int close=s->extent==CG_CLOSEST_SIDE || s->extent==CG_CLOSEST_CORNER;
    double x=close?gradient_min(left,right):gradient_max(left,right);
    double y=close?gradient_min(top,bottom):gradient_max(top,bottom);
    if (s->extent==CG_EXPLICIT) {
        /* Images 3 refused circle percentages because the basis was unclear.
         * Images 4 §3.2.2 now defines the scaled diagonal, not width or min
         * side. Ellipse percentages still use their respective dimensions. */
        double basis=s->shape==CG_CIRCLE?hypot(w,h)/1.4142135623730950488:w;
        p->rx=gradient_length(s->radius[0],basis);
        p->ry=s->shape==CG_CIRCLE?p->rx:gradient_length(s->radius[1],h);
    } else if (s->shape==CG_CIRCLE) {
        p->rx=(s->extent==CG_CLOSEST_CORNER || s->extent==CG_FARTHEST_CORNER)
              ?hypot(x,y):(close?gradient_min(x,y):gradient_max(x,y));
        p->ry=p->rx;
    } else {
        p->rx=x; p->ry=y;
        /* The nearest/farthest corner uses the aspect ratio of the matching
         * side ellipse. The chosen corner has (x,y), so sqrt(2) scales both
         * radii even when the center is outside the box. */
        if (s->extent==CG_CLOSEST_CORNER || s->extent==CG_FARTHEST_CORNER) {
            p->rx*=1.4142135623730950488; p->ry*=1.4142135623730950488;
        }
    }
    p->length=p->rx;
    if (s->shape==CG_CIRCLE && p->rx==0) {
        /* The spec says an arbitrarily small positive circle. Keeping zero
         * as the percentage basis while measuring real px radial distance
         * gives its limit: explicit px stops still paint concentric circles. */
        p->radial_mode=GRAD_RADIAL_NORMAL;
    } else if (p->rx==0) p->radial_mode=GRAD_RADIAL_MIRROR;
    else if (p->ry==0) p->radial_mode=GRAD_RADIAL_SOLID;
}

static void gradient_average(struct paint_gradient *p)
{
    double span=p->stop[p->nstop-1].pos-p->stop[0].pos;
    for (int i=1;i<p->nstop;i++) {
        /* A zero repeating period uses the SAME colors equally spaced, not
         * the last color and not an equal average of stops. Endpoint colors
         * therefore have half the weight of interior colors. */
        double weight=span>0?(p->stop[i].pos-p->stop[i-1].pos)/span
                            :1.0/(p->nstop-1);
        for (int k=0;k<4;k++)
            p->average[k]+=(p->stop[i-1].rgba[k]+p->stop[i].rgba[k])*weight*.5;
    }
}

int css_gradient_prepare_rgba(const struct cgradient *s, double w, double h,
                              unsigned cur, int cur_alpha,
                              struct paint_gradient *p)
{
    if (!s || !p || s->kind<CG_LINEAR || s->kind>CG_CONIC ||
        s->nstop<2 || s->nstop>CG_MAXSTOP || !isfinite(w) || !isfinite(h) ||
        w<0 || h<0 || cur_alpha<0 || cur_alpha>255) return 0;
    memset(p,0,sizeof *p);
    p->kind=s->kind; p->repeating=s->repeating;
    p->cx=w*.5; p->cy=h*.5;
    if (s->kind==CG_LINEAR) {
        if (s->dir==CG_DIR_CORNER) {
            double d=hypot(w,h);
            p->ux=d>0?h/d:0; p->uy=d>0?w/d:-1;
            if (s->corner==CG_CORNER_TL || s->corner==CG_CORNER_BL) p->ux=-p->ux;
            if (s->corner==CG_CORNER_TL || s->corner==CG_CORNER_TR) p->uy=-p->uy;
        } else {
            /* Exact quadrant axes preserve the old 1xN ramp optimization;
             * sin(PI) residue must not turn an axis ramp into a 2D surface. */
            int a=s->angle_mdeg%360000; if (a<0) a+=360000;
            if (a==0) { p->ux=0;p->uy=-1; }
            else if (a==90000) { p->ux=1;p->uy=0; }
            else if (a==180000) { p->ux=0;p->uy=1; }
            else if (a==270000) { p->ux=-1;p->uy=0; }
            else { double r=a*GRAD_TAU/360000.0; p->ux=sin(r);p->uy=-cos(r); }
        }
        p->length=fabs(w*p->ux)+fabs(h*p->uy);
    } else {
        p->cx=gradient_length(s->center[0],w);
        p->cy=gradient_length(s->center[1],h);
        if (s->kind==CG_RADIAL) gradient_radial_geometry(s,w,h,p);
        else { p->length=1; p->rotation=s->angle_mdeg/360000.0; }
    }
    if (!isfinite(p->cx) || !isfinite(p->cy) || !isfinite(p->length) ||
        p->rx<0 || p->ry<0 || !isfinite(p->rx) || !isfinite(p->ry)) return 0;

    double pos[CG_MAXSTOP]; unsigned char have[CG_MAXSTOP];
    int first=-1,last=-1;
    for (int i=0;i<s->nstop;i++) if (!s->stop[i].hint) {
        if (first<0) first=i;
        last=i;
    }
    if (first!=0 || last!=s->nstop-1 || first==last) return 0;
    for (int i=0;i<s->nstop;i++) {
        const struct cgstop *t=&s->stop[i]; have[i]=1;
        if (t->pos_kind==CG_POS_PCT) pos[i]=t->pos*p->length/10000.0;
        else if (t->pos_kind==CG_POS_PX && s->kind!=CG_CONIC) pos[i]=t->pos;
        else if (t->pos_kind==CG_POS_TURN && s->kind==CG_CONIC) pos[i]=t->pos;
        else if (t->pos_kind!=CG_POS_AUTO || t->hint) return 0;
        else if (i==first) pos[i]=0;
        else if (i==last) pos[i]=p->length;
        else { pos[i]=0; have[i]=0; }
        if (!isfinite(pos[i])) return 0;
    }
    /* Old grad_build claimed that distributing AUTO then clamping neighbors
     * was the spec rule. Correction: Images 3 §3.4.3 explicitly clamps ALL
     * declared positions first. red 80px, white 0px, black, blue 100px must
     * put black at 90px, not 80px. Never clip positions to [0%,100%]. */
    double preceding=pos[first];
#ifndef CSS_GRADIENT_BAD_FIXUP
    for (int i=first+1;i<=last;i++) if (have[i]) {
        if (pos[i]<preceding) pos[i]=preceding;
        preceding=pos[i];
    }
#endif
    for (int i=first+1;i<last;i++) if (!have[i]) {
        int j=i; while (j<=last && !have[j]) j++;
        for (int k=i;k<j;k++)
            pos[k]=pos[i-1]+(pos[j]-pos[i-1])*((double)(k-i+1)/(j-i+1));
        i=j-1;
    }
#ifdef CSS_GRADIENT_BAD_FIXUP
    /* Test-only old ordering: retain the real downstream interpolation so
     * the independent black-at-90px oracle can watch this mistake go red. */
    for (int i=first+1;i<=last;i++) {
        if (pos[i]<preceding) pos[i]=preceding;
        preceding=pos[i];
    }
#endif
    /* The token grammar accepts finite doubles. A resolved span that itself
     * overflows is outside this painter's numeric representation: refuse it
     * rather than send NaN through an RGBA byte conversion. */
    if (!isfinite(pos[last]-pos[first])) return 0;
    for (int i=first;i<=last;i++) {
        if (s->stop[i].hint) continue;
        struct paint_stop *t=&p->stop[p->nstop++];
        t->pos=pos[i];t->hint=.5;
        if (!gradient_color(&s->stop[i],cur,cur_alpha,t->rgba)) return 0;
        if (i>first && s->stop[i-1].hint) {
            struct paint_stop *prev=&p->stop[p->nstop-2];
            double d=t->pos-prev->pos;
            prev->hint=d>0?(pos[i-1]-prev->pos)/d:.5;
        }
    }
    gradient_average(p);
    return 1;
}

int css_gradient_prepare(const struct cgradient *s, double w, double h,
                         unsigned cur, struct paint_gradient *p)
{
    return css_gradient_prepare_rgba(s,w,h,cur,255,p);
}

static double gradient_parameter(const struct paint_gradient *p,double x,double y)
{
    x-=p->cx;y-=p->cy;
    if (p->kind==CG_LINEAR) return x*p->ux+y*p->uy+p->length*.5;
    if (p->kind==CG_CONIC) {
        /* At the exact center there is no unique ray. Choose its zero ray;
         * physical pixel sampling handles the unresolved repeating footprint. */
        double t=(x==0 && y==0)?0:atan2(x,-y)/GRAD_TAU;
        t-=p->rotation; return t-floor(t);
    }
    if (p->radial_mode==GRAD_RADIAL_MIRROR) return fabs(x);
    if (p->rx==0) return hypot(x,y); /* zero circle, px-stop limit */
    if (p->radial_mode==GRAD_RADIAL_SOLID) return p->stop[p->nstop-1].pos;
    double ratio=p->rx/p->ry;
    double sy=y==0?0:isfinite(ratio)?y*ratio:(y/p->ry)*p->rx;
    return hypot(x,sy);
}

static void gradient_output(const double c[4],unsigned *rgb,int *alpha)
{
    for (int k=0;k<4;k++) if (!isfinite(c[k])) {
        if (rgb) *rgb=0;
        if (alpha) *alpha=0;
        return;
    }
    int a=(int)(c[3]+.5),v[3]={0,0,0};
    if (a<0) a=0;
    if (a>255) a=255;
    if (c[3]>0) for (int k=0;k<3;k++) {
#ifdef CSS_GRADIENT_NO_PREMULT
        v[k]=(int)(c[k]+.5);
#else
        v[k]=(int)(c[k]*255.0/c[3]+.5);
#endif
        if (v[k]<0) v[k]=0;
        if (v[k]>255) v[k]=255;
    }
    if (rgb) *rgb=(unsigned)(v[0]<<16|v[1]<<8|v[2]);
    if (alpha) *alpha=a;
}

static void gradient_sample_at(const struct paint_gradient *p,double t,
                               int average,unsigned *rgb,int *alpha)
{
    if (!p || p->nstop<2) { if(rgb)*rgb=0;if(alpha)*alpha=0;return; }
    double period=p->stop[p->nstop-1].pos-p->stop[0].pos;
    int repeating=p->repeating;
#ifdef CSS_GRADIENT_NO_REPEAT
    repeating=0; /* Actual old padding behavior, not a failed parser/build. */
#endif
    if (repeating && (average || period<=0 || p->radial_mode==GRAD_RADIAL_SOLID)) {
        gradient_output(p->average,rgb,alpha);return;
    }
    if (repeating) {
        /* floor modulo, not C remainder: repeats continue BEFORE the first
         * stop, and negative positions must not turn into endpoint padding. */
        double r=fmod(t-p->stop[0].pos,period);if(r<0)r+=period;
        t=p->stop[0].pos+r;
    }
    if (t<p->stop[0].pos) { gradient_output(p->stop[0].rgba,rgb,alpha);return; }
    int i=0;while(i+1<p->nstop && t>=p->stop[i+1].pos)i++;
    if (i==p->nstop-1) { gradient_output(p->stop[i].rgba,rgb,alpha);return; }
    const struct paint_stop *a=&p->stop[i],*b=&p->stop[i+1];
    double u=(t-a->pos)/(b->pos-a->pos);
    if (a->hint<=0) u=u>0?1:0;
    else if (a->hint>=1) u=u>=1?1:0;
    else if (a->hint!=.5) u=pow(u,log(.5)/log(a->hint));
    double c[4];
    for(int k=0;k<4;k++)c[k]=a->rgba[k]+(b->rgba[k]-a->rgba[k])*u;
    gradient_output(c,rgb,alpha);
}

void css_gradient_sample(const struct paint_gradient *p,double x,double y,
                         unsigned *rgb,int *alpha)
{
    gradient_sample_at(p,p?gradient_parameter(p,x,y):0,0,rgb,alpha);
}

void css_gradient_sample_pixel(const struct paint_gradient *p,double x,double y,
                               double pw,double ph,unsigned *rgb,int *alpha)
{
    int average=0;
    if (p && p->repeating && p->nstop>=2) {
        double span=p->stop[p->nstop-1].pos-p->stop[0].pos;
        double footprint;
        if (p->kind==CG_LINEAR) footprint=fabs(p->ux*pw)+fabs(p->uy*ph);
        else if (p->kind==CG_CONIC) {
            double dx=x-p->cx,dy=y-p->cy,r2=dx*dx+dy*dy;
            footprint=r2>0?(fabs(dy*pw)+fabs(dx*ph))/(GRAD_TAU*r2):1;
        } else if (p->radial_mode==GRAD_RADIAL_SOLID) footprint=span+1;
        else if (p->radial_mode==GRAD_RADIAL_MIRROR) footprint=fabs(pw);
        else {
            double dx=x-p->cx,dy=y-p->cy;
            double ratio=p->rx==0?1:p->rx/p->ry;
            double distance=hypot(dx,dy*ratio);
            footprint=distance>0?(fabs(dx*pw)+fabs(dy*ratio*ratio*ph))/distance
                                :hypot(pw,ph*ratio);
        }
        average=span<footprint;
    }
    gradient_sample_at(p,p?gradient_parameter(p,x,y):0,average,rgb,alpha);
}
