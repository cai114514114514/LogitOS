/* Physical edges must enter the same per-declaration cascade as logical
 * edges, including ordinary px and zero declarations: capturing only calc
 * would let an earlier extension value defeat a later native declaration.
 * Relative values remain source spans until the element's font is known.
 * Mapping remains horizontal LTR; this pass does not add cascade layers. */
extern int css_native_supports_decl(const char *,int,const char *,int) LOGIT_WEAK;
LOGIT_WEAK_STUB(css_native_supports_decl);
enum { PB_PADDING,PB_MARGIN,PB_INSET };
struct pb_property { const char *name; unsigned char kind,count,edge[4],physical; };
static const struct pb_property pb_properties[]={
    {"padding",0,4,{0,1,2,3},1},{"padding-top",0,1,{0},1},
    {"padding-right",0,1,{1},1},{"padding-bottom",0,1,{2},1},{"padding-left",0,1,{3},1},
    {"margin",1,4,{0,1,2,3},1},{"margin-top",1,1,{0},1},
    {"margin-right",1,1,{1},1},{"margin-bottom",1,1,{2},1},{"margin-left",1,1,{3},1},
    {"top",2,1,{0},1},{"right",2,1,{1},1},{"bottom",2,1,{2},1},{"left",2,1,{3},1},
    {"padding-inline",0,2,{3,1},0},{"padding-block",0,2,{0,2},0},
    {"padding-inline-start",0,1,{3},0},{"padding-inline-end",0,1,{1},0},
    {"padding-block-start",0,1,{0},0},{"padding-block-end",0,1,{2},0},
    {"margin-inline",1,2,{3,1},0},{"margin-block",1,2,{0,2},0},
    {"margin-inline-start",1,1,{3},0},{"margin-inline-end",1,1,{1},0},
    {"margin-block-start",1,1,{0},0},{"margin-block-end",1,1,{2},0},
    {"inset",2,4,{0,1,2,3},0},{"inset-inline",2,2,{3,1},0},{"inset-block",2,2,{0,2},0},
    {"inset-inline-start",2,1,{3},0},{"inset-inline-end",2,1,{1},0},
    {"inset-block-start",2,1,{0},0},{"inset-block-end",2,1,{2},0}
};
/* This sentinel has static lifetime; all other raw spans retain the existing
 * stylesheet/inline-attribute ownership. A native-accepted value outside the
 * typed grammar restores the pre-extension computed edge, not the previous
 * extension result. Invalid declarations never create a barrier. */
static const char pb_native[]="@native";
static int pb_keyword(const char *s,int n)
{
    struct im_scan k={s,n,0,0,0};im_trivia(&k);int start=k.i;
    while(k.i<n && ident(s[k.i]))k.i++;
    int end=k.i;im_trivia(&k);if(k.i!=n)return 0;
    if(ieq(s+start,end-start,"inherit"))return 2;
    if(ieq(s+start,end-start,"initial") || ieq(s+start,end-start,"unset"))return 1;
    return 0;
}
static int pb_components(const char *value,int len,const struct pb_property *q,
                         const char **raw,int *lengths)
{
    struct im_scan k={value,len,0,0,0};int start[4],end[4],count=0;
    for(;;) {
        im_trivia(&k);if(k.i==k.n)break;
        if(count==q->count)return 0;
        start[count]=k.i;struct im_value v;int flag;
        if(q->kind==PB_PADDING) {if(!sm_component(&k,&v,0,&flag))return 0;}
        else if(!im_component(&k,&v,&flag))return 0;
        end[count++]=k.i;
        int saved=k.i;im_trivia(&k);if(k.i<k.n && saved==k.i)return 0;
    }
    if(!count)return 0;
    /* Validation is atomic across the whole shorthand, including the fourth
     * component. A malformed later component cannot publish an earlier edge. */
    for(int i=0;i<q->count;i++) {
        int part=i<count?i:(i==3 && count>1?1:0),edge=q->edge[i];
        raw[edge]=value+start[part];lengths[edge]=end[part]-start[part];
    }
    return 1;
}
static void parse_physical_boxes(const char *d,int n,struct xpatch *p)
{
    /* parse_cascade_decls supplies one declaration. Dispatch once by property
     * name, preserving case-insensitivity without rescanning the full value
     * once for every physical and logical alias. */
    struct im_scan scan={d,n,0,0,0};im_trivia(&scan);int start=scan.i;
    while(scan.i<n && ident(d[scan.i]))scan.i++;
    int end=scan.i;im_trivia(&scan);if(scan.i==n || d[scan.i++]!=':')return;
    const char *value=d+scan.i;int len=n-scan.i;
    for(unsigned i=0;i<sizeof pb_properties/sizeof pb_properties[0];i++) {
        const struct pb_property *q=&pb_properties[i];if(!ieq(d+start,end-start,q->name))continue;
        const char **raw=q->kind==PB_PADDING?p->padding_raw:q->kind==PB_MARGIN?p->margin_raw:p->inset_raw;
        int *lens=q->kind==PB_PADDING?p->padding_len:q->kind==PB_MARGIN?p->margin_len:p->inset_len;
        if(pb_keyword(value,len)) {
            for(int j=0;j<q->count;j++){raw[q->edge[j]]=value;lens[q->edge[j]]=len;}
        } else if(q->physical && !pb_components(value,len,q,raw,lens) &&
                  LOGIT_HAVE(css_native_supports_decl) &&
                  css_native_supports_decl(q->name,(int)strlen(q->name),value,len)) {
            for(int j=0;j<q->count;j++){raw[q->edge[j]]=pb_native;lens[q->edge[j]]=sizeof pb_native-1;}
        }
        return;
    }
}
int css_box_supports_decl(const char *prop,int plen,const char *value,int vlen)
{
#ifdef CSS_PHYSICAL_SPACING_LEGACY
    (void)prop;(void)plen;(void)value;(void)vlen;return 0;
#else
    if(!prop || !value || plen<=0 || vlen<=0)return 0;
    for(unsigned i=0;i<sizeof pb_properties/sizeof pb_properties[0];i++) {
        const struct pb_property *q=&pb_properties[i];if(!ieq(prop,plen,q->name))continue;
        if(pb_keyword(value,vlen))return 1;
        const char *raw[4]={0};int lengths[4]={0};
        return pb_components(value,vlen,q,raw,lengths);
    }
    return 0;
#endif
}
/* A compact snapshot of only the twelve edges. Percent and auto metadata is
 * copied along with the cache so inheritance re-resolves at the child's own
 * containing block, and native fallback cannot retain a prior calc addend. */
struct pb_edge { int px,pct,offset;unsigned char automatic; };
struct pb_base { struct pb_edge edge[3][4]; };
static struct pb_edge pb_read(const struct cstyle *s,int kind,int e)
{
    struct pb_edge v={0};if(!s){v.automatic=kind==PB_INSET;return v;}
    if(kind==PB_PADDING) {
        v.px=e==0?s->pt:e==1?s->pr:e==2?s->pb:s->pl;
        v.pct=e==0?s->pt0:e==1?s->pr0:e==2?s->pb0:s->pl0;
        v.offset=s->padding_pct_offset[e];
    } else if(kind==PB_MARGIN) {
        v.px=e==0?s->mt:e==1?s->mr:e==2?s->mb:s->ml;
        v.pct=s->margin_pct[e];v.offset=s->margin_pct_offset[e];
        v.automatic=!!(s->margin_auto&(1u<<e));
    } else {
        v.px=e==0?s->top:e==1?s->right:e==2?s->bottom:s->left;
        v.pct=s->inset_pct[e];v.offset=!!(s->inset_pct_mask&(1u<<e));
        v.automatic=!(e==0?s->has_top:e==1?s->has_right:e==2?s->has_bottom:s->has_left);
    }
    return v;
}
static void pb_write(struct cstyle *s,int kind,int e,struct pb_edge v)
{
    if(kind==PB_PADDING) {
        int *px=e==0?&s->pt:e==1?&s->pr:e==2?&s->pb:&s->pl;
        int *pct=e==0?&s->pt0:e==1?&s->pr0:e==2?&s->pb0:&s->pl0;
        *px=v.px;*pct=v.pct;s->padding_pct_offset[e]=v.offset;
    } else if(kind==PB_MARGIN) {
        int *px=e==0?&s->mt:e==1?&s->mr:e==2?&s->mb:&s->ml;
        *px=v.px;s->margin_pct[e]=v.pct;s->margin_pct_offset[e]=v.offset;
        s->margin_auto&=~(1u<<e);if(v.automatic)s->margin_auto|=1u<<e;
    } else {
        int *px=e==0?&s->top:e==1?&s->right:e==2?&s->bottom:&s->left;
        *px=v.px;s->inset_pct[e]=v.pct;
        s->inset_pct_mask&=~(1u<<e);if(v.offset)s->inset_pct_mask|=1u<<e;
        if(e==0)s->has_top=!v.automatic;else if(e==1)s->has_right=!v.automatic;
        else if(e==2)s->has_bottom=!v.automatic;else s->has_left=!v.automatic;
    }
}
static void pb_snapshot(const struct cstyle *s,struct pb_base *base)
{
    for(int k=0;k<3;k++)for(int e=0;e<4;e++)base->edge[k][e]=pb_read(s,k,e);
}
static void apply_box_keywords(struct node *n,const struct xpatch *p,const struct pb_base *base)
{
    for(int kind=0;kind<3;kind++)for(int e=0;e<4;e++) {
        const char *raw=kind==PB_PADDING?p->padding_raw[e]:kind==PB_MARGIN?p->margin_raw[e]:p->inset_raw[e];
        int len=kind==PB_PADDING?p->padding_len[e]:kind==PB_MARGIN?p->margin_len[e]:p->inset_len[e];
        if(!raw)continue;
        int keyword=pb_keyword(raw,len);
        struct pb_edge v={0};v.automatic=kind==PB_INSET;
        if(raw==pb_native)v=base->edge[kind][e];
        else if(keyword==2)v=pb_read(n->parent?n->parent->style:0,kind,e);
        else if(keyword!=1)continue;
        pb_write(n->style,kind,e,v);
    }
}
