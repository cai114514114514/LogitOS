/* A face test must retain Z until after composition. Testing the 2D paint
 * determinant confuses a mirrored scaleX(-1) with a turned-away rotateY(180).
 * The ordinary affine subset below uses the Z component of the transformed
 * normal (inverse-transpose). Perspective projection, intersecting 3D planes
 * and depth sorting remain outside this painter: a non-affine matrix declines
 * this new cull rather than deriving facing from a discarded perspective row.
 * Translation/origin/scroll do not affect an affine normal, so only the 3x3
 * linear part is needed. No retained DOM/stylesheet pointers or allocation. */
static int bf_transformable(const struct node *n)
{
    const struct cstyle *s=n && n->type==N_ELEM?n->style:0;
    if(!s || s->display==DISP_NONE || s->display==DISP_CONTENTS)return 0;
    if(s->display!=DISP_INLINE || s->outer_inline || s->pos_abs || s->flt!=FLT_NONE)return 1;
    const struct cstyle *p=n->parent?n->parent->style:0;
    if(p && (p->display==DISP_FLEX || p->display==DISP_GRID))return 1;
    const char *t=n->tag;
    return t && (!strcmp(t,"img") || !strcmp(t,"svg") || !strcmp(t,"canvas") ||
           !strcmp(t,"video") || !strcmp(t,"iframe") || !strcmp(t,"input") ||
           !strcmp(t,"button") || !strcmp(t,"select") || !strcmp(t,"textarea"));
}
static int bf_preserves(const struct node *n)
{
    const struct cstyle *s=n?n->style:0;
    /* These grouping values are actually represented by this engine. Other
     * grouping effects (filters, paint containment, masks) are not a claim of
     * this bounded facing implementation. Keep computed preserve-3d unchanged;
     * it is only the USED accumulation boundary which becomes flat. */
    return bf_transformable(n) && s->preserve_3d && !s->opacity_context &&
           s->opacity==255 && s->overflow_x==OVF_VISIBLE && s->overflow_y==OVF_VISIBLE;
}
static int bf_local(const struct node *n,double out[9])
{
    for(int i=0;i<9;i++)out[i]=(i%4)==0?1.0:0.0;
    const struct cstyle *s=n?n->style:0;
    if(!bf_transformable(n) || !s->xraw[XR_TRANSFORM])return 1;
    if(!LOGIT_HAVE(ci_transform_parse_context) || !LOGIT_HAVE(ci_transform_matrix))return 0;
    struct ci_xform xf;
    if(css_node_transform_parse(n,s->xraw[XR_TRANSFORM],
                                 s->xrawlen[XR_TRANSFORM],&xf)!=0)return 1;
    int x=0,y=0,w=0,h=0;layout_node_box((struct node *)n,&x,&y,&w,&h);
    double m[16];ci_transform_matrix(&xf,w,h,m);
    if(m[3]!=0 || m[7]!=0 || m[11]!=0 || m[15]!=1)return 0;
    for(int col=0;col<3;col++)for(int row=0;row<3;row++)out[col*3+row]=m[col*4+row];
    return 1;
}
static void bf_premul(double a[9],const double b[9])
{
    double t[9];
    for(int col=0;col<3;col++)for(int row=0;row<3;row++) {
        double v=0;for(int k=0;k<3;k++)v+=b[k*3+row]*a[col*3+k];
        t[col*3+row]=v;
    }
    memcpy(a,t,sizeof t);
}
static int bf_own_plane(const struct node *n)
{
    if(!bf_transformable(n))return 0;
    const struct cstyle *s=n->style;
    if(s->backface_hidden)return 1;
    if(!s->xraw[XR_TRANSFORM] || !LOGIT_HAVE(ci_transform_parse_context))return 0;
    struct ci_xform xf;
    if(css_node_transform_parse(n,s->xraw[XR_TRANSFORM],
                                 s->xrawlen[XR_TRANSFORM],&xf)!=0)return 0;
    /* CSS Transforms defines a 3D transformed element by its function list,
     * including identity 3D functions. A plain 2D transform/none is not a new
     * 3D plane under this rule and must not escape its parent's hidden face. */
    for(int i=0;i<xf.n;i++)switch(xf.f[i].kind) {
        case CI_TRANSLATEZ:case CI_TRANSLATE3D:case CI_SCALEZ:case CI_SCALE3D:
        case CI_ROTATEX:case CI_ROTATEY:case CI_ROTATEZ:case CI_ROTATE3D:
        case CI_MATRIX3D:case CI_PERSPECTIVE:return 1;
    }
    return 0;
}
/* 1 back-facing, 2 edge-on, 0 front-facing or unsupported. The epsilon only
 * removes floating-point cosine residue at exact quarter turns. It is relative
 * to matrix scale, not a fixed angle threshold that hides ordinary near-90deg
 * faces. A singular/non-finite transform is left to existing transform paint. */
static int bf_facing(const struct node *n,const struct node *boundary)
{
    double a[9];if(!bf_local(n,a))return 0;
    for(const struct node *p=n==boundary?0:n->parent;p && bf_preserves(p);p=p==boundary?0:p->parent) {
        double b[9];if(!bf_local(p,b))return 0;bf_premul(a,b);
    }
    double scale=0;for(int i=0;i<9;i++){double v=a[i]<0?-a[i]:a[i];if(v>scale)scale=v;}
    if(!(scale>0) || scale>1e100)return 0;
    for(int i=0;i<9;i++)a[i]/=scale;
    double c=a[0]*a[4]-a[3]*a[1];
    double det=a[0]*(a[4]*a[8]-a[7]*a[5])-a[3]*(a[1]*a[8]-a[7]*a[2])+a[6]*(a[1]*a[5]-a[4]*a[2]);
    if(!(det>1e-14 || det< -1e-14))return 0;
    if(c<1e-14 && c> -1e-14)return 2;
    return (c<0)!=(det<0);
}
static int item_backface_culled(const struct node *n)
{
#ifdef PAINT_BACKFACE_VISIBLE_LEGACY
    /* Exact former behavior: CSS state still parses, while both consumers
     * ignore the face. This makes independent pixel/hit assertions go red. */
    (void)n;return 0;
#else
    const struct node *boundary=LOGIT_HAVE(top_layer_owner)?top_layer_owner((struct node *)n):0;
    int separate_plane=0;
    const struct node *child=0;
    for(const struct node *p=n;p;child=p,p=p==boundary?0:p->parent) {
        const struct cstyle *s=p->type==N_ELEM?p->style:0;
        if(!s)continue;
        if(child) {
            if(!bf_preserves(p))separate_plane=0;
            else if(bf_transformable(child)) {
                /* A transformed/hidden face participating in preserve-3d
                 * owns a plane; a back-facing parent plane cannot hide its
                 * independently front-facing child. At a flat ancestor the
                 * whole scene is composited together again. */
                if(bf_own_plane(child))separate_plane=1;
            }
        }
        if(!separate_plane && s->backface_hidden && bf_transformable(p) && bf_facing(p,boundary))return 1;
    }
    return 0;
#endif
}
