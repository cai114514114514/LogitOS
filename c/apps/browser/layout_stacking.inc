/* CSS 2.2 Appendix E paint groups, flattened only after their ancestry is
 * known. The old zsort inherited ONE z integer; parent z=8/child z=1 then
 * painted child text before its parent's background. Its proposed four-level
 * key also silently loses deeper ancestors. This tree has heap-sized nodes
 * and an iterative traversal: depth is never truncated to a guessed number.
 *
 * True contexts are atomic. Positioned-auto, floats and inline-block/flex/grid
 * items are temporary paint groups: their positioned/context descendants still
 * participate in the enclosing TRUE context. Sorted output remains the sole
 * forward-paint/backward-hit order, including generated boxes and flex order.
 * Sources: https://www.w3.org/TR/CSS22/zindex.html and CSS Flexbox 1 section 4.3.
 * No tree, node pointer map or sort key outlives this completed layout pass. */
enum { LS_BACK,LS_NEGATIVE,LS_BLOCK,LS_FLOAT,LS_INLINE,LS_POSITIONED,LS_POSITIVE };
struct ls_node { struct node *node; int parent,group,context; };
struct ls_group { int owner,parent,begin,end,cursor; };
struct ls_event { int group,phase,z,rank,sequence,ref; };
struct ls_work { struct node *node; int parent; };
struct ls_tree {
    struct ls_node *nodes;int nn,ncap;
    struct ls_group *groups;int ng,gcap;
    struct ls_event *events;int ne,ecap;
    struct ls_work *work;int nw,wcap;
    int *hash,hcap;
};
static int ls_grow(void **p,int *cap,int need,unsigned long size)
{
    if(need<=*cap)return 1;
    int n=*cap?*cap:64;while(n<need){if(n>0x1fffffff)return 0;n*=2;}
    if(size>~0UL/(unsigned long)n)return 0;
    void *v=kmalloc((unsigned long)n*size);if(!v)return 0;
    if(*p)memcpy(v,*p,(unsigned long)*cap*size);kfree(*p);*p=v;*cap=n;return 1;
}
static unsigned long ls_hash(const struct node *n)
{unsigned long x=(unsigned long)n;x^=x>>17;x*=0xed5ad4bbUL;x^=x>>11;return x;}
static int ls_find(const struct ls_tree *t,const struct node *n)
{
    if(!n||!t->hcap)return -1;
    unsigned i=(unsigned)ls_hash(n)&(unsigned)(t->hcap-1);
    while(t->hash[i]){int k=t->hash[i]-1;if(t->nodes[k].node==n)return k;i=(i+1)&(unsigned)(t->hcap-1);}return -1;
}
static int ls_map(struct ls_tree *t,int id)
{
    if(t->nn*2>=t->hcap){
        int nc=t->hcap?t->hcap*2:128;if(nc<t->hcap)return 0;
        int *h=kmalloc((unsigned long)nc*sizeof *h);if(!h)return 0;memset(h,0,(unsigned long)nc*sizeof *h);
        for(int k=0;k<t->nn;k++){unsigned i=(unsigned)ls_hash(t->nodes[k].node)&(unsigned)(nc-1);while(h[i])i=(i+1)&(unsigned)(nc-1);h[i]=k+1;}
        kfree(t->hash);t->hash=h;t->hcap=nc;return 1;
    }
    unsigned i=(unsigned)ls_hash(t->nodes[id].node)&(unsigned)(t->hcap-1);while(t->hash[i])i=(i+1)&(unsigned)(t->hcap-1);t->hash[i]=id+1;return 1;
}
static int ls_event(struct ls_tree *t,int group,int phase,int z,int rank,int sequence,int ref)
{
    if(!ls_grow((void **)&t->events,&t->ecap,t->ne+1,sizeof *t->events))return 0;
    t->events[t->ne++]=(struct ls_event){group,phase,z,rank,sequence,ref};return 1;
}
static int ls_group(struct ls_tree *t,int owner,int parent,int phase,int z)
{
    if(!ls_grow((void **)&t->groups,&t->gcap,t->ng+1,sizeof *t->groups))return -1;
    int id=t->ng++;t->groups[id]=(struct ls_group){owner,parent,-1,-1,-1};
    if(parent>=0&&!ls_event(t,parent,phase,z,owner,-1,-id-1))return -1;return id;
}
static int ls_push(struct ls_tree *t,struct node *n,int parent)
{
    if(!ls_grow((void **)&t->work,&t->wcap,t->nw+1,sizeof *t->work))return 0;
    t->work[t->nw++]=(struct ls_work){n,parent};return 1;
}
static int ls_order(const struct ls_work *w)
{const struct cstyle *s=w->node?w->node->style:0;return s?s->order:0;}
/* A flex/grid parent's children use order-modified tree order. Preserve DOM
 * order at equal order and never use node allocation serials: moving an old
 * node in the DOM must change ties. Sorting this one sibling list also moves
 * each whole subtree, not just that item's own background. */
static int ls_children(struct ls_tree *t,struct node *n,int parent)
{
    int first=t->nw;
    for(struct node *c=layout_first(n);c;c=layout_next(c)){
        if(c->type!=N_ELEM&&c->type!=N_TEXT)continue;
        const struct cstyle *s=c->style;if(s&&s->display==DISP_NONE)continue;
        if(!ls_push(t,c,parent))return 0;
    }
    const struct cstyle *s=n?n->style:0;int count=t->nw-first;
    if(s&&(s->display==DISP_FLEX||s->display==DISP_GRID)&&count>1){
        int need=0;for(int i=first+1;i<t->nw;i++)if(ls_order(&t->work[i])<ls_order(&t->work[i-1]))need=1;
        if(need){struct ls_work *tmp=kmalloc((unsigned long)count*sizeof *tmp);if(!tmp)return 0;
            for(int w=1;w<count;w*=2){for(int i=0;i<count;i+=2*w){int m=i+w<count?i+w:count,r=i+2*w<count?i+2*w:count,a=i,b=m,o=i;
                while(a<m&&b<r)tmp[o++]=ls_order(&t->work[first+b])<ls_order(&t->work[first+a])?t->work[first+b++]:t->work[first+a++];
                while(a<m)tmp[o++]=t->work[first+a++];while(b<r)tmp[o++]=t->work[first+b++];}
                memcpy(t->work+first,tmp,(unsigned long)count*sizeof *tmp);}
            kfree(tmp);
        }
    }
    for(int a=first,b=t->nw-1;a<b;a++,b--){struct ls_work w=t->work[a];t->work[a]=t->work[b];t->work[b]=w;}return 1;
}
#include "css_transform_context.h"
static int ls_transform(const struct node *n)
{
    const struct cstyle *s=n?n->style:0;
    if(!s||!s->xraw[XR_TRANSFORM]||!LOGIT_HAVE(ci_transform_parse_context))return 0;
    struct ci_xform xf;
    return css_node_transform_parse(n,s->xraw[XR_TRANSFORM],
                                     s->xrawlen[XR_TRANSFORM],&xf)==0&&xf.n>0;
}
static int ls_build(struct ls_tree *t,struct node *root)
{
    if(ls_group(t,-1,-1,0,0)<0||!ls_push(t,root,-1))return 0;
    int layers=LOGIT_HAVE(top_layer_count)?top_layer_count():0;
    for(int layer=-1;layer<layers;layer++){
        if(layer>=0){struct node *n=LOGIT_HAVE(top_layer_at)?top_layer_at(layer):0;if(!n||n->doc!=root->doc)continue;if(!ls_push(t,n,0))return 0;}
        while(t->nw){struct ls_work w=t->work[--t->nw];struct node *n=w.node;if(!n||ls_find(t,n)>=0)continue;
            if(!ls_grow((void **)&t->nodes,&t->ncap,t->nn+1,sizeof *t->nodes))return 0;
            int id=t->nn++,parent=w.parent,group=parent>=0?t->nodes[parent].group:0,context=parent>=0?t->nodes[parent].context:0;
            t->nodes[id]=(struct ls_node){n,parent,group,context};if(!ls_map(t,id))return 0;
            const struct cstyle *s=n->type==N_ELEM?n->style:0;
            if(s&&s->display!=DISP_CONTENTS){
                const struct cstyle *ps=parent>=0?t->nodes[parent].node->style:0;
                int positioned=s->position!=POS_STATIC;
                int flexitem=!s->pos_abs&&ps&&(ps->display==DISP_FLEX||ps->display==DISP_GRID);
                int top=LOGIT_HAVE(top_layer_contains)&&top_layer_contains(n);
                int real=(root->doc&&n==dom_doc_element(root->doc))||top||
                    (s->has_z&&(positioned||flexitem))||s->position==POS_FIXED||s->position==POS_STICKY||s->opacity<255||s->opacity_context||ls_transform(n);
                if(real){int z=s->has_z&&(positioned||flexitem)?s->z_index:0;
                    group=ls_group(t,id,top?0:context,z<0?LS_NEGATIVE:z>0?LS_POSITIVE:LS_POSITIONED,z);if(group<0)return 0;context=group;
                }else if(positioned){group=ls_group(t,id,context,LS_POSITIONED,0);if(group<0)return 0;}
                else if(s->flt){group=ls_group(t,id,group,LS_FLOAT,0);if(group<0)return 0;}
                else if(s->display==DISP_INLINE_BLOCK||s->outer_inline||flexitem){group=ls_group(t,id,group,LS_INLINE,0);if(group<0)return 0;}
                t->nodes[id].group=group;t->nodes[id].context=context;
            }
            if(!ls_children(t,n,id))return 0;
        }
    }return 1;
}
static struct node *ls_origin(const struct item *it)
{
    if(it->generated_style&&it->generated_style->generated_owner){
        struct node *owner=it->generated_style->generated_owner;
        if(owner->style&&it->pseudo>=1&&it->pseudo<=2)return ((struct cstyle *)owner->style)->generated[it->pseudo-1];
    }return it->node;
}
static int ls_less(const struct ls_event *a,const struct ls_event *b)
{
    if(a->group!=b->group)return a->group<b->group;
    if(a->phase!=b->phase)return a->phase<b->phase;
    if(a->z!=b->z)return a->z<b->z;
    if(a->rank!=b->rank)return a->rank<b->rank;
    return a->sequence<b->sequence;
}
static int zsort(struct node *root)
{
    if(nitem<2)return 1;struct ls_tree t={0};struct ls_event *tmp=0;struct item *dst=0;int ok=0;
    if(!ls_build(&t,root))goto end;
    for(int i=0;i<nitem;i++){
        struct node *n=ls_origin(&items[i]);int id=ls_find(&t,n);
        while(id<0&&n){n=n->parent;id=ls_find(&t,n);}
        int group=id>=0?t.nodes[id].group:0,phase=LS_INLINE;
        const struct cstyle *s=n&&n->type==N_ELEM?n->style:0;
        if(items[i].type==IT_RECT){
            if(id>=0&&t.groups[group].owner==id)phase=LS_BACK;
            else if(s&&s->display!=DISP_INLINE&&s->display!=DISP_INLINE_BLOCK&&!s->outer_inline)phase=LS_BLOCK;
        }
        if(!ls_event(&t,group,phase,0,id>=0?id:0,i,i))goto end;
    }
    tmp=kmalloc((unsigned long)t.ne*sizeof *tmp);dst=kmalloc((unsigned long)nitem*sizeof *dst);if(!tmp||!dst)goto end;
    for(int width=1;width<t.ne;width*=2){
        for(int i=0;i<t.ne;i+=2*width){int m=i+width<t.ne?i+width:t.ne,r=i+2*width<t.ne?i+2*width:t.ne,a=i,b=m,o=i;
            while(a<m&&b<r)tmp[o++]=ls_less(&t.events[b],&t.events[a])?t.events[b++]:t.events[a++];
            while(a<m)tmp[o++]=t.events[a++];while(b<r)tmp[o++]=t.events[b++];}
        memcpy(t.events,tmp,(unsigned long)t.ne*sizeof *tmp);
    }
    for(int i=0;i<t.ne;i++){struct ls_group *g=&t.groups[t.events[i].group];if(g->begin<0)g->begin=g->cursor=i;g->end=i+1;}
    int out=0,group=0;
    while(group>=0){struct ls_group *g=&t.groups[group];
        if(g->cursor==g->end){group=g->parent;continue;}
        struct ls_event *e=&t.events[g->cursor++];
        if(e->ref<0){group=-e->ref-1;continue;}
        if(out>=nitem)goto end;
        dst[out]=items[e->ref];
        unsigned long offset=(unsigned long)items[e->ref].text-(unsigned long)items[e->ref].marker;
        if(offset<sizeof dst[out].marker)dst[out].text=dst[out].marker+offset;
        out++;
    }
    if(out!=nitem)goto end;memcpy(items,dst,(unsigned long)nitem*sizeof *items);
    /* List markers borrow their own inline marker[] backing. A bytewise
     * permutation without rebasing leaves text pointing into the old slot
     * (or the freed sort buffer); ordinary DOM/source spans stay borrowed. */
    for(int i=0;i<nitem;i++){unsigned long offset=(unsigned long)dst[i].text-(unsigned long)dst[i].marker;
        if(offset<sizeof items[i].marker)items[i].text=items[i].marker+offset;
    }ok=1;
end:
    kfree(tmp);kfree(dst);kfree(t.nodes);kfree(t.groups);kfree(t.events);kfree(t.work);kfree(t.hash);return ok;
}
