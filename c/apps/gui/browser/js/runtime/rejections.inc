/* QuickJS announces rejection when it happens, BEFORE await/catch attaches a
 * reaction. Calling page code from that hook falsely reported caught digest
 * errors as Uncaught. Store identity/reason and notify on a later page turn,
 * after the microtask queue drains. NEW -> REPORTED -> HANDLED is separate
 * from QuickJS's tracker callback; tracker reentry never calls JavaScript.
 * Keep at most 256 records. Under sustained unhandled rejection floods the
 * oldest REPORTED record is evicted, so late rejectionhandled beyond that
 * history is deliberately not promised. Pending records are never silently
 * replaced: overflow is diagnosed. All JS roots are released on page close. */
#define REJECTION_LIMIT 256
struct rejection_record { JSValue promise, reason; unsigned long seq; int state; };
static struct rejection_record rejections[REJECTION_LIMIT];
static unsigned long rejection_seq;
static int rejection_overflow;

static void rejection_drop(JSContext *ctx, struct rejection_record *r)
{
    JS_FreeValue(ctx,r->promise); JS_FreeValue(ctx,r->reason);
    r->promise=r->reason=JS_UNDEFINED; r->state=0;
}
static void rejection_tracker(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                              int is_handled, void *opaque)
{
    (void)opaque;
    if(ctx!=g_ctx) return; /* child realms need their own notification owner */
#ifdef JS_REJECTION_IMMEDIATE_NEGCTL
    if(!is_handled && JS_IsFunction(ctx,g_reject_hook)) {
        JSValue a[3]={JS_DupValue(ctx,promise),JS_DupValue(ctx,reason),JS_FALSE};
        JSValue v=JS_Call(ctx,g_reject_hook,JS_UNDEFINED,3,(JSValueConst*)a);
        if(JS_IsException(v))JS_FreeValue(ctx,JS_GetException(ctx));
        JS_FreeValue(ctx,v);JS_FreeValue(ctx,a[0]);JS_FreeValue(ctx,a[1]);
    }
    return;
#endif
    int slot=-1,old=-1;
    for(int i=0;i<REJECTION_LIMIT;i++) {
        struct rejection_record *r=&rejections[i];
        if(r->state && JS_VALUE_GET_PTR(r->promise)==JS_VALUE_GET_PTR(promise)) {
            if(is_handled) {
                if(r->state==1)rejection_drop(ctx,r);
                else {r->state=3;r->seq=++rejection_seq;}
            }
            return;
        }
        if(!r->state && slot<0)slot=i;
        if(r->state==2 && (old<0 || r->seq<rejections[old].seq))old=i;
    }
    if(is_handled)return;
    if(slot<0 && old>=0){slot=old;rejection_drop(ctx,&rejections[old]);}
    if(slot<0){if(!rejection_overflow++)printf("[platform] rejection notification queue exhausted (256 pending)\n");return;}
    struct rejection_record *r=&rejections[slot];
    r->promise=JS_DupValue(ctx,promise);r->reason=JS_DupValue(ctx,reason);
    r->state=1;r->seq=++rejection_seq;
}
int js_platform_rejections_pending(void)
{
    for(int i=0;i<REJECTION_LIMIT;i++)if(rejections[i].state==1 || rejections[i].state==3)return 1;
    return 0;
}
int js_platform_rejections_flush(JSContext *ctx)
{
    if(ctx!=g_ctx || JS_IsJobPending(JS_GetRuntime(ctx)) || !JS_IsFunction(ctx,g_reject_hook))return 0;
    unsigned long turn=rejection_seq;int ran=0;
    for(int i=0;i<REJECTION_LIMIT;i++) {
        struct rejection_record *r=&rejections[i];
        if((r->state!=1 && r->state!=3)||r->seq>turn)continue;
        int handled=r->state==3;
        JSValue args[3]={JS_DupValue(ctx,r->promise),JS_DupValue(ctx,r->reason),JS_NewBool(ctx,handled)};
        if(handled)rejection_drop(ctx,r);else r->state=2;
        JSValue v=JS_Call(ctx,g_reject_hook,JS_UNDEFINED,3,(JSValueConst*)args);
        if(JS_IsException(v))JS_FreeValue(ctx,JS_GetException(ctx));
        JS_FreeValue(ctx,v);JS_FreeValue(ctx,args[0]);JS_FreeValue(ctx,args[1]);ran++;
    }
    return ran;
}
static void rejections_close(JSContext *ctx)
{
    for(int i=0;i<REJECTION_LIMIT;i++)if(rejections[i].state)rejection_drop(ctx,&rejections[i]);
    rejection_seq=0;rejection_overflow=0;
}
