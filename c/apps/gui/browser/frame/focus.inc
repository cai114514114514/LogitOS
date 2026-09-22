/* Network documents deliberately do not install js_forms: its edit/selection
 * tables still belong to the top document. Focus is different: focus.c now
 * has an owner selected with js_page_context, so its real holder, events and
 * CSS queries can be shared without exposing fake form editing operations.
 * This is element focus, not a Window.focus shim or a CAPTCHA-specific path.
 * Nested browsing contexts, focus-visible and full scroll-into-view remain
 * outside this first embedded focus consumer, as does native text editing. */
static int frame_focus_dispatch(struct node *n,const char *type,int bubbles,int cancelable)
{
    struct js_event_init ev={0};ev.bubbles=bubbles;ev.cancelable=cancelable;
    return js_dom_dispatch(n,type,&ev);
}
static JSValue frame_element_focus(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int blur)
{
    (void)argc;(void)argv;
    struct pframe *f=active_frame;struct node *n=js_dom_node_from(self);
    if(!n||n->type!=N_ELEM)return JS_ThrowTypeError(ctx,"Illegal focus receiver");
    if(!f||f->ctx!=ctx||!frame_connected(f,n,n->serial))return JS_UNDEFINED;
    if(blur){if(focus_current()==n)focus_set(NULL);f->dirty=1;return JS_UNDEFINED;}
    /* A style mutation can hide the target in this same callback. Resolve
     * the child's own cascade before asking native eligibility, not the
     * parent's CSS cache and not just the target's inline style. */
    if((f->dirty||js_dom_mutation_generation()!=f->mutation)&&!style_layout(f))
        return JS_ThrowInternalError(ctx,"Cannot resolve embedded focus style");
    if(!focus_is_focusable(n))return JS_UNDEFINED;
    focus_set(n);
    if(focus_current()==n){f->want_focus=1;f->dirty=1;}
    return JS_UNDEFINED;
}
static JSValue frame_active_element(JSContext *ctx,JSValueConst self)
{
    (void)self;struct pframe *f=active_frame;
    if(!f||f->ctx!=ctx)return JS_NULL;
    struct node *n=focus_current();
    if(!n)for(n=f->root;n;n=next_node(n,f->root))if(tag(n,"body"))break;
    if(!n)for(n=f->root;n;n=next_node(n,f->root))if(tag(n,"html"))break;
    return js_dom_node_value(ctx,n);
}
static int frame_focus_install(struct pframe *f)
{
#ifdef PF_TEST_NO_ELEMENT_FOCUS
    (void)f;return 1;
#else
    JSContext *ctx=js_page_ctx();JSValue g=JS_GetGlobalObject(ctx);
    JSValue ctor=JS_GetPropertyStr(ctx,g,"HTMLElement"),proto=JS_GetPropertyStr(ctx,ctor,"prototype");
    JSValue doc=JS_GetPropertyStr(ctx,g,"document");
    int ok=JS_IsObject(proto)&&JS_IsObject(doc);
    if(ok){
        JSValue a=JS_NewCFunctionMagic(ctx,frame_element_focus,"focus",0,JS_CFUNC_generic_magic,0);
        JSValue b=JS_NewCFunctionMagic(ctx,frame_element_focus,"blur",0,JS_CFUNC_generic_magic,1);
        ok=JS_DefinePropertyValueStr(ctx,proto,"focus",a,JS_PROP_WRITABLE|JS_PROP_CONFIGURABLE)>=0;
        ok=JS_DefinePropertyValueStr(ctx,proto,"blur",b,JS_PROP_WRITABLE|JS_PROP_CONFIGURABLE)>=0&&ok;
        JSAtom prop=JS_NewAtom(ctx,"activeElement");
        ok=JS_DefinePropertyGetSet(ctx,doc,prop,JS_NewCFunction2(ctx,(JSCFunction*)frame_active_element,"get activeElement",0,JS_CFUNC_getter,0),JS_UNDEFINED,JS_PROP_CONFIGURABLE|JS_PROP_ENUMERABLE)>=0&&ok;
        JS_FreeAtom(ctx,prop);
    }
    JS_FreeValue(ctx,doc);JS_FreeValue(ctx,proto);JS_FreeValue(ctx,ctor);JS_FreeValue(ctx,g);
    if(ok){focus_reset();fc_set_dispatch(frame_focus_dispatch);}
    return ok;
#endif
}
/* Script cannot switch document owners while on its JS stack. Commit the
 * parent iframe holder after returning to the parent event-loop boundary. */
static void frame_focus_commit_parent(struct pframe *f)
{
    if(!f->want_focus)return;f->want_focus=0;
    if(f->retire||!f->window||!frame_window_connected(f->window)||!focus_is_focusable(f->host))return;
    focused_frame=f;focus_set(f->host);
}
