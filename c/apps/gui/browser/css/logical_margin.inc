/* The old logical margin producer accepted px only. A real modal's
 * margin-block:auto consequently lost both auto bits; a 300px-high flex
 * fixture placed its 60px panel at y=0 instead of y=120. Preserve the margin
 * KIND, not an early used pixel value, and let the existing flex/block solver
 * distribute auto space. The same bounded affine grammar as insets handles
 * calc/em/rem/viewport units and percentages without another ad-hoc parser.
 *
 * This keeps the extension's existing horizontal LTR mapping and unlayered
 * author cascade boundary. It does not claim vertical-writing mapping,
 * additional math functions which the shared grammar explicitly refuses.
 * Correction: css_physical_box.inc now brings physical edges and
 * initial/unset/inherit into the same unlayered author cascade. */
static void parse_logical_margins(const char *d,int len,struct xpatch *p)
{
    const int inl[]={3,1},blk[]={0,2};
    im_capture(d,len,"margin-inline",inl,2,p->margin_raw,p->margin_len);
    im_capture(d,len,"margin-block",blk,2,p->margin_raw,p->margin_len);
    im_capture(d,len,"margin-inline-start",inl,1,p->margin_raw,p->margin_len);
    im_capture(d,len,"margin-inline-end",inl+1,1,p->margin_raw,p->margin_len);
    im_capture(d,len,"margin-block-start",blk,1,p->margin_raw,p->margin_len);
    im_capture(d,len,"margin-block-end",blk+1,1,p->margin_raw,p->margin_len);
}
static void apply_logical_margins(struct cstyle *st,const struct xpatch *p)
{
    for(int edge=0;edge<4;edge++)if(p->margin_raw[edge]) {
        struct im_scan k={p->margin_raw[edge],p->margin_len[edge],0,0,0};
        struct im_value v;int automatic,px,pct;
        if(!im_component(&k,&v,&automatic) || k.i!=k.n || !im_resolved(&v,st,&px,&pct))continue;
        int *cache=edge==0?&st->mt:edge==1?&st->mr:edge==2?&st->mb:&st->ml;
        *cache=px;st->margin_pct_offset[edge]=px;st->margin_pct[edge]=pct;
        st->margin_auto&=~(1u<<edge);if(automatic)st->margin_auto|=1u<<edge;
    }
}
