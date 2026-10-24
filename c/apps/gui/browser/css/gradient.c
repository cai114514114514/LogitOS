/* CSS Images 3/4 gradient grammar. Included by extra.c, like the existing
 * responsibility-specific grammar modules, so narrow host source lists keep
 * linking the real producer. No geometry, stop fixup or color interpolation
 * happens here: percentages and PX stops cannot be ordered until layout.
 *
 * Whole-value validation is transactional. The old parser accepted a valid
 * function followed by garbage and removed !important itself. Importance is
 * declaration syntax owned by the cascade, not part of an <image> value.
 * Unsupported interpolation spaces, old vendor angle conventions, calculated
 * sizes/positions refuse. Images4 circle percentages retain the scaled-diagonal
 * basis contract for the sampler, and a single color becomes a uniform image.
 * Literal numbers accept decimal/exponent syntax within finite double range;
 * exponents beyond 308 are an explicit representation bound, never truncated.
 */
#include "css/gradient.h"
#include <string.h>
#include <float.h>

#ifndef CSS_GRADIENT_COLOR_VALIDATOR
#include "weaksym.h"
extern int img_css_color(const char *,int,unsigned char[4]) LOGIT_WEAK;
LOGIT_WEAK_STUB(img_css_color);
static int grad_color_valid(const char *s,int n)
{
    unsigned char rgba[4];
    /* A standalone parser without the literal-color authority refuses, rather
     * than returning a plausible stop for a color nobody can render. */
    return LOGIT_HAVE(img_css_color)&&img_css_color(s,n,rgba);
}
#else
static int grad_color_valid(const char *s,int n)
{return CSS_GRADIENT_COLOR_VALIDATOR(s,n);}
#endif

struct grad_span {const char *s;int n;};
static int grad_space(int c)
{return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\f';}
static int grad_word(struct grad_span s,const char *word)
{
    int i=0;
    while(i<s.n&&word[i]) {
        int c=(unsigned char)s.s[i];if(c>='A'&&c<='Z')c+=32;
        if(c!=word[i])return 0;i++;
    }
    return i==s.n&&!word[i];
}
static int grad_color(struct grad_span value)
{
    /* A property-level color validator also accepts CSS-wide keywords. They
     * are not <color> components inside a gradient; native fallback must not
     * turn linear-gradient(inherit,blue) into a valid literal image. */
    if(grad_word(value,"inherit")||grad_word(value,"initial")||grad_word(value,"unset")||
       grad_word(value,"revert")||grad_word(value,"revert-layer"))return 0;
    return grad_color_valid(value.s,value.n);
}
static int grad_trivia(const char *s,int n,int i)
{
    for(;;) {
        while(i<n&&grad_space(s[i]))i++;
        if(i+1>=n||s[i]!='/'||s[i+1]!='*')return i;
        i+=2;while(i+1<n&&!(s[i]=='*'&&s[i+1]=='/'))i++;
        if(i+1>=n)return n;i+=2;
    }
}
static struct grad_span grad_trim(struct grad_span s)
{
    int first=grad_trivia(s.s,s.n,0);s.s+=first;s.n-=first;
    /* Trailing comments are trivia too. Locate the last actual token rather
     * than searching backwards for a comment opener inside a quoted color. */
    int i=0,last=0,quote=0;
    while(i<s.n) {
        if(!quote) {
            int next=grad_trivia(s.s,s.n,i);
            if(next!=i){i=next;continue;}
        }
        int c=s.s[i++];
        if(c=='\\'&&i<s.n)i++;
        else if(quote){if(c==quote)quote=0;}
        else if(c=='\''||c=='"')quote=c;
        last=i;
    }
    s.n=last;return s;
}
static int grad_atoms(struct grad_span s,struct grad_span *out,int cap)
{
    int i=0,count=0;
    while((i=grad_trivia(s.s,s.n,i))<s.n) {
        if(count==cap)return -1;
        int start=i,depth=0,quote=0;
        while(i<s.n) {
            int c=s.s[i];
            if(c=='\\'&&i+1<s.n){i+=2;continue;}
            if(quote){if(c==quote)quote=0;i++;continue;}
            if(c=='\''||c=='"'){quote=c;i++;continue;}
            if(c=='(')depth++;
            else if(c==')'){if(!depth)return -1;depth--;}
            if(!depth&&(grad_space(c)||(c=='/'&&i+1<s.n&&s.s[i+1]=='*')))break;
            i++;
        }
        if(depth||quote)return -1;
        out[count++]=(struct grad_span){s.s+start,i-start};
    }
    return count;
}
static int grad_parts(struct grad_span s,struct grad_span *out,int cap)
{
    int start=0,depth=0,quote=0,count=0;
    for(int i=0;i<=s.n;i++) {
        if(i<s.n) {
            int c=s.s[i];
            if(c=='\\'&&i+1<s.n){i++;continue;}
            if(quote){if(c==quote)quote=0;continue;}
            if(c=='\''||c=='"'){quote=c;continue;}
            if(c=='/'&&i+1<s.n&&s.s[i+1]=='*') {
                int next=grad_trivia(s.s,s.n,i);i=next-1;continue;
            }
            if(c=='(')depth++;else if(c==')'){if(!depth)return -1;depth--;}
        }
        if(i==s.n||(!depth&&!quote&&s.s[i]==',')) {
            if(count==cap)return -1;
            struct grad_span part=grad_trim((struct grad_span){s.s+start,i-start});
            if(!part.n)return -1;out[count++]=part;start=i+1;
        }
    }
    return depth||quote?-1:count;
}

/* Keep unit parsing separate from CSS number syntax. A trailing '.' is not
 * part of a number: 1. and 1.e2 must refuse rather than becoming 1. */
static int grad_number(struct grad_span s,double *number,struct grad_span *unit)
{
    int i=0,negative=0,digits=0;double value=0,fraction=.1;
    if(i<s.n&&(s.s[i]=='+'||s.s[i]=='-'))negative=s.s[i++]=='-';
    while(i<s.n&&s.s[i]>='0'&&s.s[i]<='9') {
        int digit=s.s[i++]-'0';if(value>(DBL_MAX-digit)/10)return 0;
        value=value*10+digit;digits++;
    }
    if(i<s.n&&s.s[i]=='.') {
        if(i+1==s.n||s.s[i+1]<'0'||s.s[i+1]>'9')return 0;i++;
        while(i<s.n&&s.s[i]>='0'&&s.s[i]<='9') {
            value+=(s.s[i++]-'0')*fraction;fraction*=.1;digits++;
        }
    }
    if(!digits)return 0;
    if(i<s.n&&(s.s[i]=='e'||s.s[i]=='E')) {
        int p=i+1,sign=1,exponent=0,any=0;
        if(p<s.n&&(s.s[p]=='+'||s.s[p]=='-'))sign=s.s[p++]=='-'?-1:1;
        if(p<s.n&&s.s[p]>='0'&&s.s[p]<='9') {
            while(p<s.n&&s.s[p]>='0'&&s.s[p]<='9') {
                exponent=exponent*10+s.s[p++]-'0';any++;
                if(exponent>308)return 0;
            }
            if(!any)return 0;
            while(exponent--) {if(sign>0&&value>DBL_MAX/10)return 0;value=sign>0?value*10:value/10;}
            i=p;
        }
    }
    *number=negative?-value:value;*unit=(struct grad_span){s.s+i,s.n-i};return 1;
}
static int grad_length(struct grad_span token,int fs,int root,struct cg_length *out)
{
    struct grad_span unit;double value,factor=0;
    if(!grad_number(token,&value,&unit))return 0;
    out->px=out->percent=0;
    if(grad_word(unit,"%")){out->percent=value/100;return 1;}
    if(!unit.n){if(value!=0)return 0;return 1;}
    if(grad_word(unit,"px"))factor=1;
    else if(grad_word(unit,"em"))factor=fs;
    else if(grad_word(unit,"rem"))factor=root;
    else if(grad_word(unit,"in"))factor=96;
    else if(grad_word(unit,"cm"))factor=96/2.54;
    else if(grad_word(unit,"mm"))factor=96/25.4;
    else if(grad_word(unit,"q"))factor=96/101.6;
    else if(grad_word(unit,"pt"))factor=96/72.0;
    else if(grad_word(unit,"pc"))factor=16;
    else return 0;
    if(factor<=0||value>DBL_MAX/factor||value<-DBL_MAX/factor)return 0;
    out->px=value*factor;return 1;
}
static int grad_turn(struct grad_span token,double *turn)
{
    struct grad_span unit;double value;
    if(!grad_number(token,&value,&unit))return 0;
    if(!unit.n){if(value!=0)return 0;*turn=0;return 1;}
    if(grad_word(unit,"deg"))*turn=value/360;
    else if(grad_word(unit,"grad"))*turn=value/400;
    else if(grad_word(unit,"rad"))*turn=value/6.2831853071795864769;
    else if(grad_word(unit,"turn"))*turn=value;
    else return 0;
    return 1;
}
static int grad_direction(double turn)
{
    /* Binary remainder avoids a libm dependency in the parser's narrow host
     * links. Work is bounded by double's exponent (~1024 iterations), including
     * huge finite angles; signed stop positions NEVER use this reduction. */
    int negative=turn<0;double v=negative?-turn:turn,step=1;
    while(step<=v/2)step*=2;
    while(step>=1){if(v>=step)v-=step;step/=2;}
    if(negative&&v>0)v=1-v;
    int milli=(int)(v*360000+.5);return milli==360000?0:milli;
}
static int grad_keyword(struct grad_span token)
{
    if(grad_word(token,"left"))return 1;
    if(grad_word(token,"right"))return 2;
    if(grad_word(token,"top"))return 3;
    if(grad_word(token,"bottom"))return 4;
    if(grad_word(token,"center"))return 5;
    return 0;
}
static void grad_axis_keyword(int word,struct cg_length *out)
{out->px=0;out->percent=word==2||word==4?1:word==5?.5:0;}
static int grad_position(const struct grad_span *a,int n,int fs,int root,struct cg_length out[2])
{
    if(n<1||n>4)return 0;
    out[0]=(struct cg_length){0,.5};out[1]=out[0];
    if(n<=2) {
        int k0=grad_keyword(a[0]),k1=n==2?grad_keyword(a[1]):5;
        if(n==1) {
            if(k0){grad_axis_keyword(k0,&out[k0==3||k0==4]);return 1;}
            return grad_length(a[0],fs,root,&out[0]);
        }
        if((k0==3||k0==4)||(k1==1||k1==2)) {
            if((k0!=3&&k0!=4&&k0!=5)||(k1!=1&&k1!=2&&k1!=5))return 0;
            grad_axis_keyword(k1,&out[0]);grad_axis_keyword(k0,&out[1]);return 1;
        }
        if(k0==3||k0==4||k1==1||k1==2)return 0;
        if(k0)grad_axis_keyword(k0,&out[0]);else if(!grad_length(a[0],fs,root,&out[0]))return 0;
        if(k1)grad_axis_keyword(k1,&out[1]);else if(!grad_length(a[1],fs,root,&out[1]))return 0;
        return 1;
    }
    int used[2]={0,0};
    for(int i=0;i<n;i++) {
        int word=grad_keyword(a[i]);if(!word)return 0;
        int axis=word==3||word==4;
        if(word==5){axis=used[0]?1:0;}
        if(used[axis])return 0;used[axis]=1;grad_axis_keyword(word,&out[axis]);
        struct cg_length delta;
        if(i+1<n&&!grad_keyword(a[i+1])) {
            if(word==5||!grad_length(a[++i],fs,root,&delta))return 0;
            int sign=word==2||word==4?-1:1;
            out[axis].px+=sign*delta.px;out[axis].percent+=sign*delta.percent;
        }
    }
    return used[0]&&used[1];
}
static int grad_linear_header(const struct grad_span *a,int n,struct cgradient *g)
{
    if(!n)return 1;
    if(n==1) {double turn;if(!grad_turn(a[0],&turn))return 0;g->angle_mdeg=grad_direction(turn);return 1;}
    if(n<2||n>3||!grad_word(a[0],"to"))return 0;
    int k0=grad_keyword(a[1]),k1=n==3?grad_keyword(a[2]):0;
    if(!k0||k0==5||k1==5)return 0;
    if(n==2){g->angle_mdeg=k0==1?270000:k0==2?90000:k0==3?0:180000;return 1;}
    int x=k0<=2?k0:k1,y=k0>=3?k0:k1;
    if(x<1||x>2||y<3||y>4)return 0;
    g->dir=CG_DIR_CORNER;
    g->corner=y==3?(x==1?CG_CORNER_TL:CG_CORNER_TR):(x==1?CG_CORNER_BL:CG_CORNER_BR);return 1;
}
static int grad_radial_header(const struct grad_span *a,int n,int fs,int root,struct cgradient *g)
{
    int shape=0,extent=0,radii=0,last_radius=-1;
    for(int i=0;i<n;i++) {
        if(grad_word(a[i],"at")) {
            if(!grad_position(a+i+1,n-i-1,fs,root,g->center))return 0;
            break;
        }
        if(grad_word(a[i],"circle")||grad_word(a[i],"ellipse")) {
            if(shape)return 0;shape=1;g->shape=grad_word(a[i],"circle")?CG_CIRCLE:CG_ELLIPSE;continue;
        }
        int keyword=-1;
        if(grad_word(a[i],"closest-side"))keyword=CG_CLOSEST_SIDE;
        if(grad_word(a[i],"farthest-side"))keyword=CG_FARTHEST_SIDE;
        if(grad_word(a[i],"closest-corner"))keyword=CG_CLOSEST_CORNER;
        if(grad_word(a[i],"farthest-corner"))keyword=CG_FARTHEST_CORNER;
        if(keyword>=0){if(extent||radii)return 0;extent=1;g->extent=keyword;continue;}
        if(extent||radii==2||(radii&&last_radius!=i-1)||
           !grad_length(a[i],fs,root,&g->radius[radii]))return 0;
        if(g->radius[radii].px<0||g->radius[radii].percent<0)return 0;
        last_radius=i;radii++;
    }
    if(radii) {
        if(!shape)g->shape=radii==1?CG_CIRCLE:CG_ELLIPSE;
        if((g->shape==CG_CIRCLE&&radii!=1)||(g->shape==CG_ELLIPSE&&radii!=2))return 0;
        if(g->shape==CG_CIRCLE)g->radius[1]=g->radius[0];g->extent=CG_EXPLICIT;
    }
    return 1;
}
static int grad_conic_header(const struct grad_span *a,int n,int fs,int root,struct cgradient *g)
{
    int i=0;
    if(i<n&&grad_word(a[i],"from")) {
        double turn;if(++i==n||!grad_turn(a[i++],&turn))return 0;g->angle_mdeg=grad_direction(turn);
    }
    if(i<n&&grad_word(a[i],"at"))return grad_position(a+i+1,n-i-1,fs,root,g->center);
    return i==n;
}
static int grad_header(struct grad_span part,int fs,int root,struct cgradient *g)
{
    struct grad_span a[12],plain[12];int n=grad_atoms(part,a,12),count=0,interpolation=0;
    if(n<0)return 0;
    for(int i=0;i<n;i++) {
        if(grad_word(a[i],"in")) {
            if(interpolation++)return 0;
            /* The interpolation method is a whole unordered grammar term.
             * It may precede/follow geometry, never split "to top right" or
             * the components of an at-position into a newly valid prefix. */
            if(i!=0&&i+2!=n)return 0;
            if(i+1==n||!grad_word(a[++i],"srgb"))return 0;
        } else plain[count++]=a[i];
    }
    return g->kind==CG_LINEAR?grad_linear_header(plain,count,g):
        g->kind==CG_RADIAL?grad_radial_header(plain,count,fs,root,g):grad_conic_header(plain,count,fs,root,g);
}
static int grad_stop_position(struct grad_span token,int fs,int root,int conic,struct cgstop *stop)
{
    struct grad_span unit;double value;
    if(!grad_number(token,&value,&unit))return 0;
    if(grad_word(unit,"%")) {
        if(value>DBL_MAX/100||value<-DBL_MAX/100)return 0;
#ifdef CSS_NEGCTL_GRAD_PCT_AS_PX
        stop->pos_kind=CG_POS_PX;stop->pos=value;
#else
        stop->pos_kind=CG_POS_PCT;stop->pos=value*100;
#endif
        return 1;
    }
    if(conic){stop->pos_kind=CG_POS_TURN;return grad_turn(token,&stop->pos);}
    struct cg_length length;if(!grad_length(token,fs,root,&length))return 0;
    stop->pos_kind=CG_POS_PX;stop->pos=length.px;return 1;
}
static int grad_stops(const struct grad_span *parts,int n,int fs,int root,struct cgradient *g)
{
    int colors=0,previous_hint=0;
    for(int i=0;i<n;i++) {
        struct grad_span a[4];int count=grad_atoms(parts[i],a,4);
        if(count<1||count>3)return 0;
        struct cgstop stop={0};
        int current=grad_word(a[0],"currentcolor");
        if(current||grad_color(a[0])) {
            stop.color=a[0].s;stop.colorlen=a[0].n;stop.current_color=(unsigned char)current;
            if(count>1&&!grad_stop_position(a[1],fs,root,g->kind==CG_CONIC,&stop))return 0;
            if(g->nstop==CG_MAXSTOP)return 0;g->stop[g->nstop++]=stop;colors++;previous_hint=0;
            if(count==3) {
                if(!grad_stop_position(a[2],fs,root,g->kind==CG_CONIC,&stop)||g->nstop==CG_MAXSTOP)return 0;
                g->stop[g->nstop++]=stop;
            }
        } else {
            if(count!=1||i==0||i==n-1||previous_hint||
               !grad_stop_position(a[0],fs,root,g->kind==CG_CONIC,&stop)||g->nstop==CG_MAXSTOP)return 0;
            stop.hint=1;g->stop[g->nstop++]=stop;previous_hint=1;
        }
    }
    /* Old Images3 rule required two color clauses. Images4 §3.5.1 now accepts
     * one: render a uniform image, including repetition. Expand only a lone
     * AUTO/position stop; a two-position single color is already two stops. */
    if(colors==1&&g->nstop==1)g->stop[g->nstop++]=g->stop[0];
    return colors>=1;
}
int css_gradient_parse(const char *v,int len,int fs,int root,struct cgradient *out)
{
    if(!out)return 0;memset(out,0,sizeof *out);
    if(!v||len<=0||fs<=0||root<=0)return 0;
    struct grad_span value=grad_trim((struct grad_span){v,len});int p=0;
    while(p<value.n&&((value.s[p]>='a'&&value.s[p]<='z')||
          (value.s[p]>='A'&&value.s[p]<='Z')||value.s[p]=='-'))p++;
    if(p==value.n||value.s[p]!='(')return 0;
    struct grad_span name={value.s,p};struct cgradient g={0};
    if(grad_word(name,"linear-gradient"))g.kind=CG_LINEAR;
    else if(grad_word(name,"radial-gradient"))g.kind=CG_RADIAL;
    else if(grad_word(name,"conic-gradient"))g.kind=CG_CONIC;
    else if(grad_word(name,"repeating-linear-gradient")){g.kind=CG_LINEAR;g.repeating=1;}
    else if(grad_word(name,"repeating-radial-gradient")){g.kind=CG_RADIAL;g.repeating=1;}
    else if(grad_word(name,"repeating-conic-gradient")){g.kind=CG_CONIC;g.repeating=1;}
    else return 0;
#ifdef CSS_GRADIENT_LINEAR_ONLY
    if(g.kind!=CG_LINEAR||g.repeating)return 0;
#endif
    if(value.s[value.n-1]!=')')return 0;
    g.angle_mdeg=g.kind==CG_LINEAR?180000:0;
    g.center[0]=(struct cg_length){0,.5};g.center[1]=g.center[0];
    struct grad_span parts[CG_MAXSTOP+1];
    int count=grad_parts((struct grad_span){value.s+p+1,value.n-p-2},parts,CG_MAXSTOP+1);
    if(count<1)return 0;
    struct grad_span a[4];int atoms=grad_atoms(parts[0],a,4),first=0;
    /* A valid first color clause means no header. Otherwise it must parse as
     * the complete geometry/interpolation header, not a salvaged prefix. */
    if(atoms<1||(!grad_word(a[0],"currentcolor")&&!grad_color(a[0]))) {
        if(!grad_header(parts[0],fs,root,&g))return 0;first=1;
    }
    if(!grad_stops(parts+first,count-first,fs,root,&g))return 0;
    *out=g;return 1;
}
int css_gradient_supports(const char *v,int len)
{struct cgradient g;return css_gradient_parse(v,len,16,16,&g);}
