/* A bounded stack, not a z-index trick. Even INT_MAX page z-index must remain
 * below the most recently shown modal. The nodes are serial-checked and the
 * stack is reset before navigation frees their document arena. Removing open
 * directly or detaching a dialog is noticed by every consumer via prune().
 * Correction: popovers now share ordering/geometry, but carry a distinct kind:
 * they never create a backdrop or modal input/focus confinement. Fullscreen
 * and author ::backdrop styling remain outside this consumer. */
#define TOP_LAYER_MAX 32
struct top_entry { struct node *node, *restore; uint32_t serial, restore_serial; struct node *source; uint32_t source_serial; int kind; };
static struct top_entry g_top[TOP_LAYER_MAX];
static int g_top_n, g_top_pruning;
static void (*g_top_request_close)(struct node *);
static void (*g_top_hide_popover)(struct node *);
static struct node *g_top_pointer_owner;
static uint32_t g_top_pointer_serial;
static int g_top_pointer_armed;
/* 0 modal, 1 auto popover, 2 manual popover. Hint needs separate stack rules
 * and is refused instead of pretending that auto is equivalent. */
static int top_popover_kind(const struct node *n)
{
    const char *v=n?dom_attr(n,"popover"):0;
    if(!v)return 0;
    if(!*v||tag_is(v,"auto"))return 1;
    if(tag_is(v,"hint"))return -1;
    return 2; /* HTML invalid-value default is manual. */
}
static int top_inside(const struct node *n,const struct node *ancestor)
{for(;n;n=n->parent)if(n==ancestor)return 1;return 0;}
static int top_connected(const struct node *n)
{
    if (!n) return 0;
    while (n->parent) n=n->parent;
    return n->type==N_DOCUMENT;
}
static void top_restore(struct top_entry e)
{
    if (e.restore && e.restore->serial==e.restore_serial && top_connected(e.restore))
        focus_set(e.restore);
}
static void top_restore_popover(struct top_entry e)
{
    if(!top_inside(focus_current(),e.node))return;
    if(e.restore&&e.restore->serial==e.restore_serial&&top_connected(e.restore))focus_set(e.restore);
    else focus_set(0);
}
static void top_prune(void)
{
    if (g_top_pruning) return;
    g_top_pruning=1;
    for (int i=g_top_n-1;i>=0;i--) {
        struct top_entry e=g_top[i];
        if (e.node->serial==e.serial && top_connected(e.node) &&
            (e.kind ? top_popover_kind(e.node)==e.kind : dom_attr(e.node,"open")!=0)) continue;
        int was_top=i==g_top_n-1;
        for(int j=i+1;j<g_top_n;j++) g_top[j-1]=g_top[j];
        g_top_n--;
        if(e.kind) top_restore_popover(e);
        else if(was_top) top_restore(e);
    }
    g_top_pruning=0;
}
int top_layer_count(void){top_prune();return g_top_n;}
struct node *top_layer_at(int i){top_prune();return i>=0&&i<g_top_n?g_top[i].node:0;}
struct node *top_layer_current(void){top_prune();for(int i=g_top_n-1;i>=0;i--)if(!g_top[i].kind)return g_top[i].node;return 0;}
int top_layer_contains(const struct node *n)
{top_prune();for(int i=0;i<g_top_n;i++)if(g_top[i].node==n)return 1;return 0;}
int top_layer_is_popover(const struct node *n)
{top_prune();for(int i=0;i<g_top_n;i++)if(g_top[i].node==n&&g_top[i].kind)return 1;return 0;}
int top_layer_is_hidden_popover(const struct node *n)
{return n&&n->type==N_ELEM&&dom_attr(n,"popover")&&!top_layer_is_popover(n);}
int top_layer_is_modal(const struct node *n)
{top_prune();for(int i=0;i<g_top_n;i++)if(g_top[i].node==n&&!g_top[i].kind)return 1;return 0;}
struct node *top_layer_owner(const struct node *n)
{
    top_prune();
    for(;n;n=n->parent) for(int i=g_top_n-1;i>=0;i--)if(g_top[i].node==n)return g_top[i].node;
    return 0;
}
int top_layer_allows_input(const struct node *n)
{
#ifdef TOP_LAYER_NO_INPUT
    (void)n;return 1;
#else
    struct node *top=top_layer_current();
    if(!top)return 1;
    for(;n;n=n->parent)if(n==top)return 1;
    return 0;
#endif
}
int top_layer_push(struct node *n)
{
    if(!n||n->type!=N_ELEM||!tag_is(n->tag,"dialog")||!top_connected(n)||!dom_attr(n,"open"))return 0;
    if(top_layer_is_modal(n))return 1;
    if(g_top_n==TOP_LAYER_MAX)return 0;
    struct node *prior=focus_current();
    struct top_entry *e=&g_top[g_top_n++];
    e->node=n;e->serial=n->serial;e->restore=prior;e->restore_serial=prior?prior->serial:0;
    e->source=0;e->source_serial=0;e->kind=0;
    return 1;
}
int top_layer_push_popover(struct node *n,struct node *source)
{
    top_prune();
    int kind=top_popover_kind(n);
    if(!n||n->type!=N_ELEM||!top_connected(n)||kind<1||top_layer_contains(n))return 0;
    if(tag_is(n->tag,"dialog")&&dom_attr(n,"open"))return 0;
#ifdef POPOVER_NO_NATIVE_LAYER
    return 0;
#endif
    if(g_top_n==TOP_LAYER_MAX)return 0;
    struct node *prior=focus_current();
    struct top_entry *e=&g_top[g_top_n++];
    e->node=n;e->serial=n->serial;e->kind=kind;
    e->restore=prior;e->restore_serial=prior?prior->serial:0;
    e->source=source;e->source_serial=source?source->serial:0;
    return 1;
}
int top_layer_remove(struct node *n)
{
    /* Do not prune first: close() has just removed open, and this operation
     * must still own restoration rather than lose the entry to lazy cleanup. */
    for(int i=0;i<g_top_n;i++)if(g_top[i].node==n){
        struct top_entry e=g_top[i];int was_top=i==g_top_n-1;
        for(int j=i+1;j<g_top_n;j++)g_top[j-1]=g_top[j];g_top_n--;
        if(e.kind)top_restore_popover(e);
        else if(was_top)top_restore(e);return 1;
    }
    return 0;
}
void top_layer_reset(void){g_top_n=0;g_top_pruning=0;g_top_request_close=0;g_top_hide_popover=0;g_top_pointer_owner=0;g_top_pointer_serial=0;g_top_pointer_armed=0;}
void top_layer_set_close_request(void (*fn)(struct node *)){g_top_request_close=fn;}
void top_layer_set_popover_hide(void (*fn)(struct node *)){g_top_hide_popover=fn;}
/* Dismiss only an auto entry. A source button counts as inside so pointerup
 * does not close it before that button's click activation toggles it again. */
static struct node *top_pointer_boundary(const struct node *target)
{
    top_prune();
    for(int i=g_top_n-1;i>=0;i--) {
        struct top_entry *e=&g_top[i];
        if(!e->kind)break; /* modal interaction cannot dismiss a lower layer */
        if(e->kind!=1)continue;
        if(top_inside(target,e->node) || (e->source && e->source->serial==e->source_serial &&
                                          top_inside(target,e->source))) return e->node;
    }
    return 0;
}
void top_layer_pointer_down(struct node *target)
{
    g_top_pointer_armed=1;
    g_top_pointer_owner=top_pointer_boundary(target);
    g_top_pointer_serial=g_top_pointer_owner?g_top_pointer_owner->serial:0;
}
int top_layer_pointer_up(struct node *target)
{
    struct node *until=top_pointer_boundary(target);
    int same=g_top_pointer_armed && until==g_top_pointer_owner && (!until||until->serial==g_top_pointer_serial);
    g_top_pointer_armed=0;
    g_top_pointer_owner=0;g_top_pointer_serial=0;
    if(!same)return 0;
    int changed=0;
    /* A beforetoggle listener can re-enter. Bound work to the initial stack
     * size, and re-read after each callback rather than keeping stale indices. */
    for(int tries=g_top_n;tries>0;tries--) {
        top_prune();struct node *n=0;
        for(int i=g_top_n-1;i>=0;i--){if(!g_top[i].kind)break;if(g_top[i].kind==1){n=g_top[i].node;break;}}
        if(!n||n==until||!g_top_hide_popover)break;
        g_top_hide_popover(n);
        if(top_layer_is_popover(n))break;
        changed=1;
    }
    return changed;
}
int top_layer_escape(void)
{
    top_prune();
    for(int i=g_top_n-1;i>=0;i--) {
        if(!g_top[i].kind)break;
        if(g_top[i].kind==1) {if(g_top_hide_popover)g_top_hide_popover(g_top[i].node);return 1;}
    }
    struct node *n=top_layer_current();if(!n)return 0;
    const char *by=dom_attr(n,"closedby");
    if(by&&tag_is(by,"none"))return 1;
    if(g_top_request_close)g_top_request_close(n);
    return 1;
}
