/* Inset <length-percentage> arithmetic (CSS Values 4 10.1/10.9 and
 * CSS Position 3 3.2). The old one_px path rejected calc(.25rem * 0), leaving
 * a fixed dialog wrapper with all four has_* bits clear. The guest then put
 * the focus-owning dialog below the page rather than over it.
 *
 * Keep dimensions until type checking is over: zero times a length is still
 * a length; zero times TWO lengths must not masquerade as one. Keep the
 * percentage flag even for 0%, since an indefinite relative-height basis is
 * still auto. Resolve font/viewport units at application, percentages at
 * layout, never against a fictitious 16px font or viewport-sized absolute CB.
 *
 * LibCSS's calc bytecode currently only supplies its length-or-calc properties,
 * not these absent shorthand/logical properties; its specified serializer
 * does not validate arithmetic. Neither can be used as a computed length
 * oracle here. This bounded, allocation-free affine evaluator deliberately
 * declines min/max/clamp, dimension cancellation, font-metric units (ch/ex),
 * CSS-wide keywords and unresolved var()/env(), rather than inventing values.
 * It does NOT add physical top/right/bottom/left calc or repair their separate
 * LibCSS-vs-extension cascade. See css_extra_cascade.inc's existing boundary.
 * 32 nesting levels, 256 numeric leaves and magnitude <=1e12 bound work and
 * conversion; out-of-domain declarations are refused as a whole, not clipped
 * into another expression. No host timing claim is attached to these bounds.
 */
enum { IM_PX, IM_PCT, IM_EM, IM_REM, IM_VW, IM_VH, IM_VMIN, IM_VMAX, IM_N };
struct im_value { double v[IM_N]; int dimension, percent; };
struct im_scan { const char *s; int n, i, leaves, depth; };

static int im_bound(const struct im_value *v)
{
    for (int i=0;i<IM_N;i++)
        if (!(v->v[i]>=-1e12 && v->v[i]<=1e12)) return 0; /* also refuses NaN */
    return 1;
}
/* Return whether real whitespace occurred, not just comments. CSS requires
 * whitespace around binary +/-; comments alone do not supply that token. */
static int im_trivia(struct im_scan *k)
{
    int space=0;
    for (;;) {
        while(k->i<k->n && spc(k->s[k->i])) { k->i++;space=1; }
        if(k->i+1>=k->n || k->s[k->i]!='/' || k->s[k->i+1]!='*') return space;
        k->i+=2;
        while(k->i+1<k->n && !(k->s[k->i]=='*' && k->s[k->i+1]=='/')) k->i++;
        k->i=k->i+1<k->n?k->i+2:k->n;
    }
}
static int im_digit(char c){return c>='0' && c<='9';}
static int im_sum(struct im_scan *,struct im_value *);
static int im_atom(struct im_scan *k,struct im_value *out)
{
    memset(out,0,sizeof *out);im_trivia(k);
    int p=k->i,calc=p+5<=k->n && ieq(k->s+p,4,"calc") && k->s[p+4]=='(';
    if(calc || (p<k->n && k->s[p]=='(')) {
        if(++k->depth>32)return 0;
        k->i+=calc?5:1;
        if(!im_sum(k,out))return 0;
        im_trivia(k);
        if(k->i>=k->n || k->s[k->i++]!=')')return 0;
        k->depth--;return 1;
    }
    if(++k->leaves>256)return 0;
    int sign=1,seen=0;
    if(p<k->n && (k->s[p]=='+' || k->s[p]=='-')) {if(k->s[p]=='-')sign=-1;p++;}
    double number=0;
    while(p<k->n && im_digit(k->s[p])) {number=number*10+k->s[p++]-'0';seen=1;if(number>1e12)return 0;}
    if(p+1<k->n && k->s[p]=='.' && im_digit(k->s[p+1])) {
        double scale=.1;p++;
        while(p<k->n && im_digit(k->s[p])) {number+=(k->s[p++]-'0')*scale;scale*=.1;seen=1;}
    }
    if(!seen)return 0;
    if(p<k->n && (k->s[p]=='e' || k->s[p]=='E')) {
        int q=p+1,negative=0,exponent=0;
        if(q<k->n && (k->s[q]=='+' || k->s[q]=='-')) {negative=k->s[q]=='-';q++;}
        if(q<k->n && im_digit(k->s[q])) {
            while(q<k->n && im_digit(k->s[q])) {exponent=exponent*10+k->s[q++]-'0';if(exponent>308)return 0;}
            while(exponent--)number*=negative?.1:10;
            p=q;
        }
    }
    number*=sign;
    int u=p,unit=-1;
    if(p<k->n && k->s[p]=='%') {p++;unit=IM_PCT;out->percent=1;}
    else {
        while(p<k->n && ((k->s[p]>='a' && k->s[p]<='z') || (k->s[p]>='A' && k->s[p]<='Z')))p++;
        static const char *const names[]={"px","%","em","rem","vw","vh","vmin","vmax"};
        for(int i=0;i<IM_N;i++)if(ieq(k->s+u,p-u,names[i])) {unit=i;break;}
        if(p>u && unit<0) {
            double factor=0;
            if(ieq(k->s+u,p-u,"in"))factor=96;
            else if(ieq(k->s+u,p-u,"cm"))factor=96/2.54;
            else if(ieq(k->s+u,p-u,"mm"))factor=96/25.4;
            else if(ieq(k->s+u,p-u,"q"))factor=96/101.6;
            else if(ieq(k->s+u,p-u,"pt"))factor=96.0/72;
            else if(ieq(k->s+u,p-u,"pc"))factor=16;
            else return 0;
            unit=IM_PX;number*=factor;
        }
    }
    out->dimension=unit>=0;
    out->v[unit>=0?unit:IM_PX]=number;k->i=p;
    return im_bound(out);
}
static int im_product(struct im_scan *k,struct im_value *out)
{
    if(!im_atom(k,out))return 0;
    for (;;) {
        int saved=k->i;im_trivia(k);
        if(k->i>=k->n || (k->s[k->i]!='*' && k->s[k->i]!='/')) {k->i=saved;return 1;}
        char op=k->s[k->i++];struct im_value b;
        if(!im_atom(k,&b))return 0;
        if(op=='/') {
            if(b.dimension || b.v[IM_PX]==0)return 0;
            for(int i=0;i<IM_N;i++)out->v[i]/=b.v[IM_PX];
        } else {
            if(out->dimension && b.dimension)return 0;
            if(b.dimension) {
                double scalar=out->v[IM_PX];*out=b;
                for(int i=0;i<IM_N;i++)out->v[i]*=scalar;
            } else for(int i=0;i<IM_N;i++)out->v[i]*=b.v[IM_PX];
        }
        if(!im_bound(out))return 0;
    }
}
static int im_sum(struct im_scan *k,struct im_value *out)
{
    if(!im_product(k,out))return 0;
    for (;;) {
        int saved=k->i,before=im_trivia(k);
        if(k->i>=k->n || (k->s[k->i]!='+' && k->s[k->i]!='-')) {k->i=saved;return 1;}
        char op=k->s[k->i++];
        if(!before || !im_trivia(k))return 0;
        struct im_value b;if(!im_product(k,&b) || b.dimension!=out->dimension)return 0;
        for(int i=0;i<IM_N;i++)out->v[i]+=op=='+'?b.v[i]:-b.v[i];
        out->percent|=b.percent;if(!im_bound(out))return 0;
    }
}
static int im_component(struct im_scan *k,struct im_value *v,int *automatic)
{
    im_trivia(k);int p=k->i;
    if(p+4<=k->n && ieq(k->s+p,4,"auto")) {k->i+=4;*automatic=1;memset(v,0,sizeof *v);return 1;}
    *automatic=0;
    int calc=p+5<=k->n && ieq(k->s+p,4,"calc") && k->s[p+4]=='(';
    /* Parentheses alone are legal INSIDE calc, not as a property value. */
    if(p<k->n && k->s[p]=='(')return 0;
    if(!im_atom(k,v))return 0;
    return v->dimension || (!calc && v->v[IM_PX]==0);
}
/* One grammar/list validator for both absent-property producers. A complete
 * declaration is validated before ANY destination edge is changed. */
static void im_capture(const char *d,int len,const char *key,const int *edges,int count,const char **raw,int *lengths)
{
    int vs,ve;if(!find_decl(d,len,key,&vs,&ve))return;
    struct im_scan k={d+vs,ve-vs,0,0,0};int start[4],end[4],n=0;
    while(k.i<k.n) {
        im_trivia(&k);if(k.i==k.n)break;
        if(n==count)return;
        start[n]=k.i;struct im_value v;int automatic;
        if(!im_component(&k,&v,&automatic))return;
        end[n++]=k.i;
        int saved=k.i;im_trivia(&k);
        if(k.i<k.n && k.i==saved)return; /* no partial acceptance of suffixes */
    }
    if(!n)return;
    for(int i=0;i<count;i++) {
        int q=i<n?i:(i==2?0:(i==3 && n>1?1:0));
        raw[edges[i]]=d+vs+start[q];lengths[edges[i]]=end[q]-start[q];
    }
}
static void inset_capture(const char *d,int len,const char *key,const int *edges,int count,struct xpatch *p)
{ im_capture(d,len,key,edges,count,p->inset_raw,p->inset_len); }
static void parse_inset_lengths(const char *d,int len,struct xpatch *p)
{
    const int all[]={0,1,2,3},inl[]={3,1},blk[]={0,2};
    inset_capture(d,len,"inset",all,4,p);
    inset_capture(d,len,"inset-inline",inl,2,p);inset_capture(d,len,"inset-block",blk,2,p);
    inset_capture(d,len,"inset-inline-start",inl,1,p);inset_capture(d,len,"inset-inline-end",inl+1,1,p);
    inset_capture(d,len,"inset-block-start",blk,1,p);inset_capture(d,len,"inset-block-end",blk+1,1,p);
}
static int im_resolved(const struct im_value *v,const struct cstyle *st,int *pixel,int *percent)
{
    double vw=css_media_width(),vh=css_media_height();
    double px=v->v[IM_PX]+v->v[IM_EM]*st->font_px+v->v[IM_REM]*css_root_px()+
        (v->v[IM_VW]*vw+v->v[IM_VH]*vh+v->v[IM_VMIN]*(vw<vh?vw:vh)+v->v[IM_VMAX]*(vw>vh?vw:vh))/100;
    double pct=v->v[IM_PCT]*1024;
    if(!(px>=-0x3fffffff && px<=0x3fffffff && pct>=-2147483647 && pct<=2147483647))return 0;
    *pixel=(int)(px+(px<0?-.5:.5));*percent=(int)(pct+(pct<0?-.5:.5));return 1;
}
static void apply_inset_lengths(struct cstyle *st,const struct xpatch *p)
{
    for(int edge=0;edge<4;edge++)if(p->inset_raw[edge]) {
        struct im_scan k={p->inset_raw[edge],p->inset_len[edge],0,0,0};
        struct im_value v;int automatic;
        if(!im_component(&k,&v,&automatic) || k.i!=k.n)continue;
        int value,pct;if(!im_resolved(&v,st,&value,&pct))continue;
        if(edge==0){st->top=value;st->has_top=!automatic;}
        else if(edge==1){st->right=value;st->has_right=!automatic;}
        else if(edge==2){st->bottom=value;st->has_bottom=!automatic;}
        else {st->left=value;st->has_left=!automatic;}
        st->inset_pct[edge]=pct;
        st->inset_pct_mask&=~(1u<<edge);
        if(v.percent && !automatic)st->inset_pct_mask|=1u<<edge;
    }
}
