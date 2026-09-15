/* Gap and logical padding share the bounded, typed length grammar used by
 * insets. Keep source spans until applying to an element: em/rem and viewport
 * units cannot be resolved while compiling a stylesheet once for all nodes.
 *
 * Percentages in gap require the flex/grid cyclic percentage sizing rules and
 * remain unsupported here. Logical padding retains its existing horizontal
 * LTR mapping. Physical edges now enter the same author extension cascade
 * through css_physical_box.inc; cascade layers remain outside this pass. Padding percentages use the
 * existing containing-width resolver, with a separate immutable px addend. */
static int sm_component(struct im_scan *k,struct im_value *v,int gap,int *normal)
{
    im_trivia(k);int start=k->i;
    *normal=0;
    if(gap && start+6<=k->n && ieq(k->s+start,6,"normal")) {
        k->i+=6;memset(v,0,sizeof *v);*normal=1;return 1;
    }
    int automatic;
    if(!im_component(k,v,&automatic) || automatic || (gap && v->percent))return 0;
    int calc=start+5<=k->n && ieq(k->s+start,4,"calc") && k->s[start+4]=='(';
    /* A negative literal is outside these properties' range. A calc value is
     * range-clamped after its relative units and percentage basis resolve. */
    if(!calc)for(int i=0;i<IM_N;i++)if(v->v[i]<0)return 0;
    return 1;
}
static void sm_capture(const char *d,int len,const char *key,const int *edges,
                       int count,int gap,const char **raw,int *lengths)
{
    int vs,ve;if(!find_decl(d,len,key,&vs,&ve))return;
    struct im_scan k={d+vs,ve-vs,0,0,0};int start[2],end[2],n=0;
    while(k.i<k.n) {
        im_trivia(&k);if(k.i==k.n)break;
        if(n==count)return;
        start[n]=k.i;struct im_value v;int normal;
        if(!sm_component(&k,&v,gap,&normal))return;
        end[n++]=k.i;
        int saved=k.i;im_trivia(&k);
        if(k.i<k.n && k.i==saved)return;
    }
    if(!n)return;
    /* Only publish after the entire declaration validated: a bad second
     * component must not erase an earlier cascade winner's first component. */
    for(int i=0;i<count;i++) {
        int q=i<n?i:0;
        raw[edges[i]]=d+vs+start[q];lengths[edges[i]]=end[q]-start[q];
    }
}
static void parse_spacing_lengths(const char *d,int len,struct xpatch *p)
{
    const int gaps[]={0,1},inl[]={3,1},blk[]={0,2};
    sm_capture(d,len,"gap",gaps,2,1,p->gap_raw,p->gap_len);
    sm_capture(d,len,"row-gap",gaps,1,1,p->gap_raw,p->gap_len);
    sm_capture(d,len,"column-gap",gaps+1,1,1,p->gap_raw,p->gap_len);
    sm_capture(d,len,"grid-gap",gaps,2,1,p->gap_raw,p->gap_len);
    sm_capture(d,len,"grid-row-gap",gaps,1,1,p->gap_raw,p->gap_len);
    sm_capture(d,len,"grid-column-gap",gaps+1,1,1,p->gap_raw,p->gap_len);
    sm_capture(d,len,"padding-inline",inl,2,0,p->padding_raw,p->padding_len);
    sm_capture(d,len,"padding-block",blk,2,0,p->padding_raw,p->padding_len);
    sm_capture(d,len,"padding-inline-start",inl,1,0,p->padding_raw,p->padding_len);
    sm_capture(d,len,"padding-inline-end",inl+1,1,0,p->padding_raw,p->padding_len);
    sm_capture(d,len,"padding-block-start",blk,1,0,p->padding_raw,p->padding_len);
    sm_capture(d,len,"padding-block-end",blk+1,1,0,p->padding_raw,p->padding_len);
}
static int sm_resolve(const char *s,int n,const struct cstyle *st,int gap,int *px,int *pct)
{
    struct im_scan k={s,n,0,0,0};struct im_value v;int normal;
    if(!sm_component(&k,&v,gap,&normal) || k.i!=k.n)return 0;
    return im_resolved(&v,st,px,pct);
}
static void apply_spacing_lengths(struct cstyle *st,const struct xpatch *p)
{
    for(int axis=0;axis<2;axis++)if(p->gap_raw[axis]) {
        int px,pct;
        if(!sm_resolve(p->gap_raw[axis],p->gap_len[axis],st,1,&px,&pct))continue;
        if(px<0)px=0;
        if(axis==0)st->grid_gap_y=px;else st->grid_gap_x=px;
    }
    for(int edge=0;edge<4;edge++)if(p->padding_raw[edge]) {
        int px,pct;
        if(!sm_resolve(p->padding_raw[edge],p->padding_len[edge],st,0,&px,&pct))continue;
        int *cache=edge==0?&st->pt:edge==1?&st->pr:edge==2?&st->pb:&st->pl;
        int *percent=edge==0?&st->pt0:edge==1?&st->pr0:edge==2?&st->pb0:&st->pl0;
        /* Convert the grammar's percent*1024 to the existing percent*100
         * storage. Cast before multiplying, preserving signed calc terms. */
        long scaled=(long)pct*100;
        *percent=(int)((scaled+(scaled<0?-512:512))/1024);
        st->padding_pct_offset[edge]=*percent?px:0;
        *cache=px<0?0:px;
    }
}
