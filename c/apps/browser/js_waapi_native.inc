/* WAAPI's computed-read overlay was never a display-list producer. Keep that
 * old implementation above for the broader interpolation surface, but commit
 * ONLY opacity/transform here: those are the two properties whose native
 * painter contract already exists. No inline style rewriting (it changes the
 * cascade base and makes cancel restore an animation's own middle frame).
 *
 * Ownership: one page/context, matching css_anim's existing global owner.
 * Every target is a retained JS wrapper, resolved afresh through node_from;
 * removed DOM nodes can be tombstoned without dereferencing a saved node.
 * Transform output lives in the entry, never in a freed JS_ToCString span.
 * snapshot() removes our pointers BEFORE a cascade can replace its base;
 * note()/tick() replay AFTER CSS animations, giving WAAPI replace precedence.
 * close() removes borrowed pointers before releasing JS references.
 *
 * Capacity is explicit: 128 targets, 2047-byte resolved transforms. Overflow
 * keeps the CSS base value and prints once; it is not a claim of painted
 * support for every property the interpolation library can serialize. */
#include "js_dom.h"
struct node *js_dom_node_from(JSValueConst v) LOGIT_WEAK;
LOGIT_WEAK_STUB(js_dom_node_from);
#define WA_CAP 128
#define WA_TEXT 2048
struct wa_entry {
    JSValue target;
    struct cstyle *applied_style; /* compare only: never dereference a freed style */
    int used, applied, seen, op, xf, last_op, last_hidden;
    int base_op, base_hidden, base_op0;
    const char *base_xf;
    unsigned short base_xflen;
    char text[WA_TEXT];
};
static struct wa_entry g_wa[WA_CAP];
static JSContext *g_wa_ctx;
static JSValue g_wa_callback;
static int g_wa_registered, g_wa_pending, g_wa_running, g_wa_warned;
static struct cstyle *wa_style(struct wa_entry *e)
{
    if(!LOGIT_HAVE(js_dom_node_from))return 0;
    struct node *n=js_dom_node_from(e->target);
    return n ? (struct cstyle *)n->style : 0;
}
static void wa_restore(void)
{
    for(int i=0;i<WA_CAP;i++)if(g_wa[i].used && g_wa[i].applied){
        struct wa_entry *e=&g_wa[i];struct cstyle *st=wa_style(e);
        if(st && st==e->applied_style){
            if(e->op){st->opacity=e->base_op;st->hidden=e->base_hidden;st->op0=e->base_op0;}
            if(e->xf && st->xraw[XR_TRANSFORM]==e->text){st->xraw[XR_TRANSFORM]=e->base_xf;st->xrawlen[XR_TRANSFORM]=e->base_xflen;}
        }
        e->applied=0;
    }
}
/* Cascade completion replays the last sampled frame without entering JS.
 * Calling the sampler here would recurse through getComputedStyle's flush and
 * run page-overridable effect accessors outside js_page's interrupt budget. */
static void wa_reapply(void)
{
    for(int i=0;i<WA_CAP;i++)if(g_wa[i].used){
        struct wa_entry *e=&g_wa[i];struct cstyle *st=wa_style(e);if(!st)continue;
        e->base_op=st->opacity;e->base_hidden=st->hidden;e->base_op0=st->op0;
        e->base_xf=st->xraw[XR_TRANSFORM];e->base_xflen=st->xrawlen[XR_TRANSFORM];
        if(e->op){st->opacity=e->last_op;st->hidden=st->hidden&&!st->op0;st->op0=0;e->last_hidden=st->hidden;}
        if(e->xf){st->xraw[XR_TRANSFORM]=e->text;st->xrawlen[XR_TRANSFORM]=(unsigned short)strlen(e->text);}
        e->applied=e->op||e->xf;e->applied_style=st;
    }
}
void js_anim_close(JSContext *ctx)
{
    if(!g_wa_registered || ctx!=g_wa_ctx)return;
    wa_restore();
    for(int i=0;i<WA_CAP;i++)if(g_wa[i].used)JS_FreeValue(ctx,g_wa[i].target);
    memset(g_wa,0,sizeof g_wa);
    JS_FreeValue(ctx,g_wa_callback);g_wa_callback=JS_UNDEFINED;
    g_wa_ctx=0;g_wa_registered=g_wa_pending=g_wa_running=g_wa_warned=0;
}
static JSValue wa_register(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    /* Interpolation-only host links have no DOM or painter to retain. */
    if(!LOGIT_HAVE(js_dom_node_from))return JS_UNDEFINED;
    if(argc<1 || !JS_IsFunction(ctx,argv[0]))return JS_ThrowTypeError(ctx,"animation sampler must be callable");
    if(g_wa_registered && g_wa_ctx!=ctx)return JS_ThrowTypeError(ctx,"animation context already active");
    js_anim_close(ctx);g_wa_ctx=ctx;g_wa_callback=JS_DupValue(ctx,argv[0]);g_wa_registered=1;
    return JS_UNDEFINED;
}
static JSValue wa_wake(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;(void)argc;(void)argv;
    if(g_wa_registered && g_wa_ctx==ctx)g_wa_pending=1;
    return JS_UNDEFINED;
}
static void wa_warning(void)
{
    if(!g_wa_warned){printf("[waapi] native paint capacity/value declined; CSS base retained\n");g_wa_warned=1;}
}
static void wa_render(void)
{
    if(!g_wa_registered)return;
    JSContext *ctx=g_wa_ctx;
    g_wa_pending=0;
    JSValue result=JS_Call(ctx,g_wa_callback,JS_UNDEFINED,0,0);
    if(JS_IsException(result)){
        JSValue ex=JS_GetException(ctx);const char *msg=JS_ToCString(ctx,ex);
        printf("[waapi] sampling failed: %s\n",msg?msg:"exception");
        if(msg)JS_FreeCString(ctx,msg);JS_FreeValue(ctx,ex);g_wa_running=0;
        JS_FreeValue(ctx,result);return;
    }
    JSValue v=JS_GetPropertyUint32(ctx,result,0);g_wa_running=JS_ToBool(ctx,v)>0;JS_FreeValue(ctx,v);
    v=JS_GetPropertyStr(ctx,result,"length");uint32_t length=0;JS_ToUint32(ctx,&length,v);JS_FreeValue(ctx,v);
    for(int i=0;i<WA_CAP;i++)g_wa[i].seen=0;
#ifndef JS_WAAPI_NO_PAINT
    for(uint32_t j=1;j+2<length;j+=3){
        JSValue target=JS_GetPropertyUint32(ctx,result,j);
        struct node *n=LOGIT_HAVE(js_dom_node_from)?js_dom_node_from(target):0;
        struct cstyle *st=n?(struct cstyle *)n->style:0;
        if(!st){JS_FreeValue(ctx,target);continue;}
        struct wa_entry *e=0;
        for(int i=0;i<WA_CAP;i++)if(g_wa[i].used && js_dom_node_from(g_wa[i].target)==n){e=&g_wa[i];break;}
        if(!e)for(int i=0;i<WA_CAP;i++)if(!g_wa[i].used){e=&g_wa[i];memset(e,0,sizeof *e);e->target=JS_DupValue(ctx,target);e->used=1;break;}
        JS_FreeValue(ctx,target);
        if(!e){wa_warning();continue;}
        int old_op=e->op,old_xf=e->xf;int old_val=e->last_op,old_hidden=e->last_hidden;
        e->seen=1;e->op=e->xf=0;
        e->base_op=st->opacity;e->base_hidden=st->hidden;e->base_op0=st->op0;
        e->base_xf=st->xraw[XR_TRANSFORM];e->base_xflen=st->xrawlen[XR_TRANSFORM];
        v=JS_GetPropertyUint32(ctx,result,j+1);
        if(!JS_IsNull(v) && !JS_IsUndefined(v)){
            const char *txt=JS_ToCString(ctx,v);char *end=0;
            double op=txt?strtod(txt,&end):-1;
            if(txt && end!=txt && *end==0 && op==op){
                if(op<0)op=0;if(op>1)op=1;
                st->opacity=(int)(op*255+0.5);e->op=1;
                /* Opacity never makes visibility:hidden visible. css_extra
                 * marks op0 separately so only opacity's hidden bit lifts. */
                st->hidden=st->hidden && !st->op0;st->op0=0;
            }
            if(txt)JS_FreeCString(ctx,txt);
        }
        JS_FreeValue(ctx,v);
        e->last_op=st->opacity;e->last_hidden=st->hidden;
        if(old_op!=e->op || (e->op && (old_val!=e->last_op || old_hidden!=e->last_hidden))){g_dirty=1;g_need_layout=1;}
        v=JS_GetPropertyUint32(ctx,result,j+2);
        if(!JS_IsNull(v) && !JS_IsUndefined(v)){
            size_t len=0;const char *txt=JS_ToCStringLen(ctx,&len,v);
            struct ci_xform parsed;
            if(txt && len<WA_TEXT && ci_transform_parse(txt,(int)len,16,16,&parsed)==0){
                if(!old_xf || strlen(e->text)!=len || memcmp(e->text,txt,len))g_dirty=1;
                memcpy(e->text,txt,len);e->text[len]=0;e->xf=1;
                st->xraw[XR_TRANSFORM]=e->text;st->xrawlen[XR_TRANSFORM]=(unsigned short)len;
            }else wa_warning();
            if(txt)JS_FreeCString(ctx,txt);
        }
        JS_FreeValue(ctx,v);
        if(old_xf && !e->xf)g_dirty=1;
        e->applied=e->op || e->xf;e->applied_style=st;
    }
#endif
    for(int i=0;i<WA_CAP;i++)if(g_wa[i].used && !g_wa[i].seen){
        if(g_wa[i].op){g_dirty=1;g_need_layout=1;}
        if(g_wa[i].xf)g_dirty=1;
        JS_FreeValue(ctx,g_wa[i].target);memset(&g_wa[i],0,sizeof g_wa[i]);
    }
    JS_FreeValue(ctx,result);
}
