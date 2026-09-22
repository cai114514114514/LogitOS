/* Element scrolling is a presentation offset, shared by JS geometry and the
 * real painter. Do not translate the already intersected layout clip: nested
 * scrollers move their inner clip but leave the outer clip stationary. Rebuild
 * that intersection from the padding boxes, each with ITS ancestors' offset.
 * CSS transforms remain the painter's following stage; vertical/RTL negative
 * scroll origins and native form-control text scrolling are separate engines. */
extern struct node *top_layer_owner(const struct node *) LOGIT_WEAK;
extern struct node *top_layer_at(int) LOGIT_WEAK;
extern struct node *top_layer_current(void) LOGIT_WEAK;
extern int top_layer_count(void) LOGIT_WEAK;
extern int top_layer_allows_input(const struct node *) LOGIT_WEAK;
LOGIT_WEAK_STUB(top_layer_owner);
LOGIT_WEAK_STUB(top_layer_at);
LOGIT_WEAK_STUB(top_layer_current);
LOGIT_WEAK_STUB(top_layer_count);
LOGIT_WEAK_STUB(top_layer_allows_input);

/* The nearest presentation boundary wins: a top-layer dialog can be inside
 * a fixed panel in the DOM while escaping that panel's intervening scroller.
 * Giving fixed unconditional priority silently reintroduces its outer clip. */
static const struct node *js_cssom_scroll_boundary(const struct node *n)
{
    const struct node *layer=LOGIT_HAVE(top_layer_owner)?top_layer_owner(n):0;
    const struct node *fixed=css_viewport_fixed_owner(n);
    for(const struct node *p=n;p;p=p->parent)
        if(p==layer||p==fixed)return p;
    return 0;
}

void js_cssom_scroll_offset(const struct node *n, int *x, int *y)
{
    int sx=0, sy=0;
#ifndef ELEMENT_SCROLL_NO_PROJECTION
    const struct node *boundary=js_cssom_scroll_boundary(n);
    if (g_scrolln && n && n!=boundary)
        for (struct node *p=n->parent;p;p=p==boundary?0:p->parent) {
            if (is_root_el(p)||is_body_el(p)) continue;
            struct scrollent *e=scroll_for(p,0);
            if(e) { sx+=e->left; sy+=e->top; }
        }
#endif
    if(x)*x=sx; if(y)*y=sy;
}
void js_cssom_project_item(struct item *e)
{
    if(!e || !e->node) return;
    const struct node *fixed=css_viewport_fixed_owner(e->node);
    if(!g_scrolln&&!fixed)return;
    int sx,sy; js_cssom_scroll_offset(e->node,&sx,&sy);
    if(!sx && !sy && !fixed)return;
    e->x-=sx; e->y-=sy;
    if(!e->has_clip)return;
    int l=-0x3fffffff,t=l,r=0x3fffffff,b=r,found=0;
    const struct node *boundary=js_cssom_scroll_boundary(e->node);
    for(struct node *p=e->node==boundary?0:e->node->parent;p;p=p==boundary?0:p->parent) {
        const struct cstyle *s=p->type==N_ELEM?(const struct cstyle *)p->style:0;
        if(!s || (s->overflow_x==OVF_VISIBLE && s->overflow_y==OVF_VISIBLE))continue;
        int x,y,w,h,ax,ay;
        if(!border_box(p,&x,&y,&w,&h))continue;
        js_cssom_scroll_offset(p,&ax,&ay);
        x+=s->border_w[3]-ax; y+=s->border_w[0]-ay;
        w-=s->border_w[3]+s->border_w[1];h-=s->border_w[0]+s->border_w[2];
        if(x>l)l=x;if(y>t)t=y;if(x+w<r)r=x+w;if(y+h<b)b=y+h;found=1;
    }
    if(found) {e->clip_x=l;e->clip_y=t;e->clip_w=r>l?r-l:0;e->clip_h=b>t?b-t:0;}
    else if(fixed)e->has_clip=0; /* an outer scroller cannot clip viewport-fixed ink */
}
int js_cssom_scroll_element_by(struct node *n,int dx,int dy)
{
    flush_layout();
    const struct node *boundary=LOGIT_HAVE(top_layer_owner)?top_layer_owner(n):0;
    for(struct node *p=n;p;p=p==boundary?0:p->parent) {
        if(p->type!=N_ELEM || is_root_el(p)||is_body_el(p))continue;
        const struct cstyle *s=sty(p);if(!s)continue;
        /* overflow:hidden accepts script scrolling but never a wheel default.
         * Controls have their own text viewport, not display-list children. */
        if(is_tag(p,"input")||is_tag(p,"textarea")||is_tag(p,"select"))continue;
        int ox=el_scroll_read(p,1),oy=el_scroll_read(p,0);
        if(dx && (s->overflow_x==OVF_AUTO||s->overflow_x==OVF_SCROLL))el_scroll_write(p,1,(double)ox+dx);
        if(dy && (s->overflow_y==OVF_AUTO||s->overflow_y==OVF_SCROLL))el_scroll_write(p,0,(double)oy+dy);
        if(ox!=el_scroll_read(p,1)||oy!=el_scroll_read(p,0))return 1;
    }
    return 0;
}
/* A pending entry is a weak pointer. Validate by walking LIVE nodes before
 * reading its serial: a handler can remove/reuse an earlier scroller. Never
 * retain an entry pointer across dispatch, because listeners can realloc it. */
static struct node *scroll_live(struct node *root,struct node *wanted,uint32_t serial)
{
    for(struct node *p=root;p;) {
        if(p==wanted)return p->serial==serial?p:0;
        if(p->first_child){p=p->first_child;continue;}
        while(p && p!=root && !p->next)p=p->parent;
        if(!p||p==root)break;p=p->next;
    }
    return 0;
}
int js_cssom_element_scroll_pending(void)
{
    for(int i=0;i<g_scrolln;i++)if(g_scroll[i].pending)return 1;
    return 0;
}
/* Called after layout, not for every painted item: the live box/overflow
 * table is now authoritative for shrink/resize/style changes. Otherwise a
 * previously legal offset can leave a shorter scroller permanently blank. */
void js_cssom_reconcile_element_scroll(void)
{
    for(int i=0;i<g_scrolln;i++) {
        struct scrollent *e=&g_scroll[i];
        struct node *n=scroll_live(js_dom_root(),e->n,e->serial);
        if(!n){e->pending=0;continue;}
        int mx=scroll_max_axis(n,1),my=scroll_max_axis(n,0),changed=0;
        if(mx>=0 && e->left>mx){e->left=mx;changed=1;}
        if(my>=0 && e->top>my){e->top=my;changed=1;}
        if(changed && !e->pending)e->pending=++g_scroll_seq;
    }
}
int js_cssom_dispatch_element_scroll(void)
{
    int changed=0;
    unsigned long long cutoff=g_scroll_seq;
    /* First enqueue order, not table allocation order. A callback's new
     * pending event has a later sequence and waits for the next render step. */
    for(;;) {
        int next=-1;
        for(int i=0;i<g_scrolln;i++)if(g_scroll[i].pending && g_scroll[i].pending<=cutoff &&
            (next<0 || g_scroll[i].pending<g_scroll[next].pending))next=i;
        if(next<0)break;
        struct node *n=scroll_live(js_dom_root(),g_scroll[next].n,g_scroll[next].serial);
        g_scroll[next].pending=0;
        if(!n)continue;
        struct js_event_init init={0};js_dom_dispatch(n,"scroll",&init);changed=1;
    }
    return changed;
}
