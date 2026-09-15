#include "dom.h"
#include "js_dom.h"
#include <stdlib.h>
/* Native CharacterData records, queued without calling JS inside DOM writes.
 * The previous prototype setter wrapper missed forms.c and the precise native
 * replaceData path. Freeze old bytes and ordinary ancestry at mutation time;
 * delivery after reparenting must not change which subtree observed the write.
 * takeRecords/observe/disconnect drain this queue before changing registrations.
 * ChildList/attributes still use the old JS producers, except splitText's
 * native insertion (which has no JS wrapper). Migrating all other writes needs
 * retained detached node identities across destroy/recycle, not another wrap.
 * Remaining limitation: destroy/recycle before delivery invalidates a queued
 * reference and drops that record. A serial guard prevents reporting it against
 * a recycled node but cannot preserve the destroyed DOM identity; that requires
 * changing node ownership, not replacing this guard with a dangling wrapper.
 */
struct mo_node_ref { struct node *node; uint32_t serial; };
struct native_mo_record {
    struct native_mo_record *next;
    unsigned old_len, ancestors;
    char *old_value;
    struct mo_node_ref *chain;
    struct mo_node_ref added, previous, following;
    int split;
};
static struct dom_subscription mo_subscription;
static struct native_mo_record *mo_head, *mo_tail;
static unsigned mo_count;
static int mo_flushing, mo_enabled, mo_scheduled;
struct native_mo_watch { struct mo_node_ref ref; unsigned flags; };
static struct native_mo_watch *mo_watches;
static unsigned mo_watch_count;
/* Scheduling an empty observer notification is not harmless: it changes where
 * a later mutation in the same script runs relative to Promise jobs. This
 * registration snapshot lets the native sink decide eligibility without
 * calling JS. JS remains the owner of observer records and oldValue policy. */
static int native_mo_interested(struct node *target, int split)
{
    unsigned type = split ? 1u : 2u;
    for (unsigned i=0;i<mo_watch_count;i++) {
        struct native_mo_watch *w=&mo_watches[i];
        if (!(w->flags&type) || !w->ref.node || w->ref.node->serial!=w->ref.serial) continue;
        for (struct node *n=target;n;n=n->parent) {
            if(n==w->ref.node && (n==target || (w->flags&4u))) return 1;
            if(dom_is_shadow_root(n) || !(w->flags&4u)) break;
        }
    }
    return 0;
}
static int native_mo_update_watches(JSContext *ctx, JSValueConst list)
{
    JSValue length=JS_GetPropertyStr(ctx,list,"length");uint32_t count=0;
    if(JS_IsException(length) || JS_ToUint32(ctx,&count,length)) {JS_FreeValue(ctx,length);return -1;}
    JS_FreeValue(ctx,length);
    struct native_mo_watch *next=count?calloc(count,sizeof *next):NULL;
    if(count&&!next){JS_ThrowOutOfMemory(ctx);return -1;}
    for(uint32_t i=0;i<count;i++) {
        JSValue row=JS_GetPropertyUint32(ctx,list,i);
        JSValue node=JS_GetPropertyUint32(ctx,row,0);
        struct node *n=js_dom_node_from(node);
        if(n)next[i].ref=(struct mo_node_ref){n,n->serial};
        JS_FreeValue(ctx,node);
        for(unsigned k=1;k<=3;k++) {
            JSValue flag=JS_GetPropertyUint32(ctx,row,k);
            if(JS_ToBool(ctx,flag)>0)next[i].flags|=1u<<(k-1);
            JS_FreeValue(ctx,flag);
        }
        JS_FreeValue(ctx,row);
    }
    free(mo_watches);mo_watches=next;mo_watch_count=count;
    return 0;
}
static JSValue g_native_mo_hook = JS_UNDEFINED;
static JSValue g_native_mo_delivery = JS_UNDEFINED;
int js_platform_mutations_flush(JSContext *ctx);
/* Queue placement is part of the observable contract. In the first native
 * implementation P1; text.data=x; P2 produced P1,P2,MO: flushing at pump entry
 * queued a second Promise too late. This one job is shared by native records
 * and JS child/attribute producers, and invokes delivery in its own turn. */
static JSValue native_mo_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    if (ctx != g_ctx || !mo_scheduled) return JS_UNDEFINED;
    /* Keep scheduled set while transferring native records into JS: emit()
     * requests this same job and must not schedule a duplicate for itself. */
    js_platform_mutations_flush(ctx);
    mo_scheduled = 0;
    if (!JS_IsFunction(ctx, g_native_mo_delivery)) return JS_UNDEFINED;
    return JS_Call(ctx, g_native_mo_delivery, JS_UNDEFINED, 0, NULL);
}
static void native_mo_schedule(JSContext *ctx)
{
    if (mo_scheduled || ctx != g_ctx) return;
    if (JS_EnqueueJob(ctx, native_mo_job, 0, NULL) < 0) {
        printf("[platform] native mutation notification enqueue failed\n");
        /* A void DOM sink cannot propagate a QuickJS exception to the writer.
         * Consume this OOM after diagnosing it; leave the next call's exception
         * slot clean and permit a later mutation to retry scheduling. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    mo_scheduled = 1;
}
static void native_mo_free(struct native_mo_record *r)
{ free(r->old_value); free(r->chain); free(r); }
static void native_mo_notify(void *opaque, const struct dom_mutation *m)
{
    (void)opaque;
    if((m->kind!=DOM_MUT_TEXT && m->kind!=DOM_MUT_SPLIT) || !m->node || !g_ctx || !mo_enabled)return;
    struct node *target=m->kind==DOM_MUT_SPLIT?m->parent:m->node;
    if(!target)return; /* Detached splitText has no childList target. */
    if(!native_mo_interested(target,m->kind==DOM_MUT_SPLIT))return;
#ifdef MO_NATIVE_CHAR_NEGCTL
    return;
#endif
    if(mo_count>=4096){printf("[platform] native mutation queue exhausted (4096 records)\n");return;}
    struct native_mo_record *r=calloc(1,sizeof *r);
    if(!r){printf("[platform] native mutation allocation failed\n");return;}
    r->split=m->kind==DOM_MUT_SPLIT;
    if(r->split){
        r->added=(struct mo_node_ref){m->related,m->related->serial};
        r->previous=(struct mo_node_ref){m->node,m->node->serial};
        struct node *after=m->related->next;
        if(after)r->following=(struct mo_node_ref){after,after->serial};
    }
    unsigned count=0;
    for(struct node *n=target;n;n=n->parent){count++;if(dom_is_shadow_root(n))break;}
    r->chain=malloc((size_t)count*sizeof *r->chain);
    r->old_value=malloc((size_t)m->old_len+1);
    if(!r->chain||!r->old_value){printf("[platform] native mutation allocation failed\n");native_mo_free(r);return;}
    if(m->old_len)memcpy(r->old_value,m->old_value,m->old_len);
    r->old_value[m->old_len]=0;r->old_len=m->old_len;
    for(struct node *n=target;n;n=n->parent){
        r->chain[r->ancestors++]=(struct mo_node_ref){n,n->serial};
        if(dom_is_shadow_root(n))break;
    }
    if(mo_tail)mo_tail->next=r;else mo_head=r;mo_tail=r;mo_count++;
    native_mo_schedule(g_ctx);
}
int js_platform_mutations_flush(JSContext *ctx)
{
    if(ctx!=g_ctx||mo_flushing||!JS_IsFunction(ctx,g_native_mo_hook))return 0;
    mo_flushing=1;int count=0;
    while(mo_head){
        struct native_mo_record *r=mo_head;mo_head=r->next;if(!mo_head)mo_tail=NULL;mo_count--;
        struct node *target=r->chain[0].node;
        if(target->serial==r->chain[0].serial){
            JSValue a[7]={js_dom_wrap_node(ctx,target),JS_NewStringLen(ctx,r->old_value,r->old_len),JS_NewArray(ctx),JS_NULL,JS_NULL,JS_NULL,JS_NewBool(ctx,r->split)};
            struct mo_node_ref refs[3]={r->added,r->previous,r->following};
            for(int k=0;k<3;k++)if(refs[k].node&&refs[k].node->serial==refs[k].serial)
                a[3+k]=js_dom_wrap_node(ctx,refs[k].node);
            for(unsigned i=0,j=0;i<r->ancestors;i++)if(r->chain[i].node->serial==r->chain[i].serial)
                JS_SetPropertyUint32(ctx,a[2],j++,js_dom_wrap_node(ctx,r->chain[i].node));
            JSValue v=JS_Call(ctx,g_native_mo_hook,JS_UNDEFINED,7,(JSValueConst*)a);
            if(JS_IsException(v))JS_FreeValue(ctx,JS_GetException(ctx));
            JS_FreeValue(ctx,v);for(int i=0;i<7;i++)JS_FreeValue(ctx,a[i]);count++;
        }
        native_mo_free(r);
    }
    mo_flushing=0;return count;
}
static JSValue native_mo_drain(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)self;
    js_platform_mutations_flush(ctx);
    if(argc > 2 && native_mo_update_watches(ctx,argv[2])<0)return JS_EXCEPTION;
    if(argc && !JS_IsUndefined(argv[0]))mo_enabled=JS_ToBool(ctx,argv[0])>0;
    if(argc > 1 && JS_ToBool(ctx,argv[1]) > 0)native_mo_schedule(ctx);
    return JS_UNDEFINED;
}
static void native_mo_close(JSContext *ctx)
{
    dom_unsubscribe(&mo_subscription);
    free(mo_watches);mo_watches=NULL;mo_watch_count=0;
    while(mo_head){struct native_mo_record *r=mo_head;mo_head=r->next;native_mo_free(r);}
    mo_tail=NULL;mo_count=0;mo_flushing=0;mo_enabled=0;mo_scheduled=0;
    JS_FreeValue(ctx,g_native_mo_hook);g_native_mo_hook=JS_UNDEFINED;
    JS_FreeValue(ctx,g_native_mo_delivery);g_native_mo_delivery=JS_UNDEFINED;
}
