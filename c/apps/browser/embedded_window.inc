/* The native network-frame WindowProxy bridge. A proxy belongs to its
 * recipient runtime; only serialized data crosses the boundary. In
 * particular, duplicating a child's JSValue in the parent is NOT a clone.
 *
 * Deliberate limits: depth one, 32 stable host identities per top document,
 * 64 queued messages, 64 KiB serialized data/message. Originally all transfer
 * lists were refused. Native port packets now detach and transfer endpoints;
 * other transferables remain refused. Same-origin direct DOM access and
 * cross-window navigation remain unavailable across independent runtimes.
 * Neither limitation grants cross-origin access to a parent document.
 */
#define PF_WINDOWS 32
#define PF_MESSAGES 64
#define PF_MESSAGE_BYTES (64*1024)
struct pf_window {
    struct node *host,*root;uint32_t serial;
    JSContext *parent_ctx;
    JSValue proxy,dispatch;
    struct pframe *child;
    unsigned epoch;
    char origin[PF_URL];
    struct dom_subscription subscription;
};
struct pf_message {
    struct pf_window *window;unsigned epoch;int to_parent;
    unsigned char *bytes;size_t len;
    struct js_port_packet *packet;
    char origin[PF_URL],target[PF_URL];
};
static struct pf_window frame_windows[PF_WINDOWS];
static struct pf_message frame_messages[PF_MESSAGES];
static int frame_window_count,frame_message_count;
static int frame_messages_pending(void){return frame_message_count!=0;}

static int frame_origin(const char *url,char *out)
{
    urlrec *u=url_parse_w(url,-1,0);if(!u)return 0;
    char *s=url_get(u,URLC_ORIGIN);int ok=s&&strcmp(s,"null")&&strlen(s)<PF_URL;
    if(ok)strcpy(out,s);free(s);url_free_w(u);return ok;
}
static int frame_window_connected(struct pf_window *w)
{
    if(!w->host||w->host->serial!=w->serial)return 0;
    struct node *n=w->host;while(n&&n!=w->root)n=n->parent;return n==w->root;
}
static void frame_window_mutation(void *owner,const struct dom_mutation *m)
{
    struct pf_window *w=owner;
    if(m->kind!=DOM_MUT_REMOVE&&m->kind!=DOM_MUT_DESTROY)return;
    struct node *n=w->host;while(n&&n!=m->node)n=n->parent;
    if(!n)return;
    /* REMOVE precedes unlink. Reinsert in the SAME JS callback must not
     * resurrect the old document or its queued messages. Only retire here;
     * actual runtime teardown waits until the JS entry unwinds. */
    w->epoch++;if(w->child){w->child->retire=1;w->child=0;}
}
static JSValue frame_post_message(JSContext *ctx,JSValueConst self,int argc,
                                 JSValueConst *argv,int to_parent,JSValue *data)
{
    (void)self;int index=-1;JS_ToInt32(ctx,&index,data[0]);
    if(index<0||index>=frame_window_count)return JS_UNDEFINED;
    struct pf_window *w=&frame_windows[index];struct pframe *f=w->child;
    if((to_parent&&(!f||js_page_ctx()!=ctx||!active_frame||active_frame!=f))||
       (!to_parent&&ctx!=w->parent_ctx))return JS_ThrowTypeError(ctx,"Invalid window owner");
    if(argc<1)return JS_ThrowTypeError(ctx,"postMessage requires a message");
    JSValue target=argc>1?JS_DupValue(ctx,argv[1]):JS_UNDEFINED;
    JSValue transfer=argc>2?JS_DupValue(ctx,argv[2]):JS_UNDEFINED;
    if(JS_IsObject(target)){
        JSValue options=target;
        JS_FreeValue(ctx,transfer);transfer=JS_GetPropertyStr(ctx,options,"transfer");
        target=JS_GetPropertyStr(ctx,options,"targetOrigin");JS_FreeValue(ctx,options);
    }
    if(JS_IsException(target)||JS_IsException(transfer)){
        JS_FreeValue(ctx,target);JS_FreeValue(ctx,transfer);return JS_EXCEPTION;
    }
    if(!LOGIT_HAVE(js_ports_prepare)&&!JS_IsUndefined(transfer)){
        JS_FreeValue(ctx,target);JS_FreeValue(ctx,transfer);
        return js_dom_throw_dom(ctx,"DataCloneError","Window transfer lists are not supported");
    }
    char source[PF_URL],dest[PF_URL];
    if(to_parent){if(!frame_origin(f->url,source)){JS_FreeValue(ctx,target);JS_FreeValue(ctx,transfer);return JS_UNDEFINED;}}
    else strcpy(source,w->origin);
    if(JS_IsUndefined(target))strcpy(dest,source);
    else {
        size_t len=0;const char *s=JS_ToCStringLen(ctx,&len,target);
        int ok=s&&len==strlen(s)&&len<PF_URL;
        if(ok){if(!strcmp(s,"/"))strcpy(dest,source);else if(!strcmp(s,"*"))strcpy(dest,"*");
            else ok=frame_origin(s,dest);}
        if(s)JS_FreeCString(ctx,s);
        if(!ok){JS_FreeValue(ctx,target);JS_FreeValue(ctx,transfer);return js_dom_throw_dom(ctx,"SyntaxError","Invalid target origin");}
    }
    JS_FreeValue(ctx,target);
    if(LOGIT_HAVE(js_ports_prepare)){
        struct js_port_packet *p=js_ports_prepare(ctx,argv[0],transfer);JS_FreeValue(ctx,transfer);
        if(!p)return JS_EXCEPTION;
        if(js_ports_packet_size(p)>PF_MESSAGE_BYTES||frame_message_count==PF_MESSAGES){
            js_ports_discard(p);return js_dom_throw_dom(ctx,"QuotaExceededError","Window message queue limit");
        }
        if(!js_ports_commit(ctx,p)){js_ports_discard(p);return JS_EXCEPTION;}
        if(!f||!frame_window_connected(w)){js_ports_discard(p);return JS_UNDEFINED;}
        struct pf_message *m=&frame_messages[frame_message_count++];memset(m,0,sizeof *m);
        m->window=w;m->epoch=w->epoch;m->to_parent=to_parent;m->packet=p;
        strcpy(m->origin,source);strcpy(m->target,dest);return JS_UNDEFINED;
    }
    JS_FreeValue(ctx,transfer);
    size_t len=0;uint8_t *serialized=JS_WriteObject(ctx,&len,argv[0],JS_WRITE_OBJ_REFERENCE);
    if(!serialized){JS_FreeValue(ctx,JS_GetException(ctx));return js_dom_throw_dom(ctx,"DataCloneError","Message cannot be cloned");}
    if(len>PF_MESSAGE_BYTES||frame_message_count==PF_MESSAGES){js_free(ctx,serialized);
        return js_dom_throw_dom(ctx,"QuotaExceededError","Window message queue limit");}
    /* Clone errors occur even for an absent recipient. Navigation/removal
     * after this point discards the message rather than delivering it into
     * a different document that happens to reuse this frame slot. */
    if(!f||!frame_window_connected(w)){js_free(ctx,serialized);return JS_UNDEFINED;}
    unsigned char *copy=malloc(len);if(!copy){js_free(ctx,serialized);return JS_ThrowOutOfMemory(ctx);}
    memcpy(copy,serialized,len);js_free(ctx,serialized);
    struct pf_message *m=&frame_messages[frame_message_count++];memset(m,0,sizeof *m);
    m->window=w;m->epoch=w->epoch;m->to_parent=to_parent;m->bytes=copy;m->len=len;
    strcpy(m->origin,source);strcpy(m->target,dest);return JS_UNDEFINED;
}
static JSValue frame_make_proxy(JSContext *ctx,struct pf_window *w,int to_parent)
{
    static const char src[]="(function(post){var p=Object.create(null);"
        "var deny=function(){throw new DOMException('Window DOM access is unavailable','SecurityError')};"
        "Object.defineProperties(p,{postMessage:{value:post},window:{get:function(){return p}},"
        "self:{get:function(){return p}},frames:{get:function(){return p}},"
        "document:{get:deny},location:{get:deny,set:deny}});return p})";
    JSValue fn=JS_Eval(ctx,src,sizeof src-1,"<embedded-window>",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(fn))return fn;
    JSValue id=JS_NewInt32(ctx,(int)(w-frame_windows));
    JSValue post=JS_NewCFunctionData(ctx,frame_post_message,2,to_parent,1,&id);
    JSValue proxy=JS_Call(ctx,fn,JS_UNDEFINED,1,(JSValueConst *)&post);
    JS_FreeValue(ctx,post);JS_FreeValue(ctx,fn);
    if(!JS_IsException(proxy)){
        JSValue parent=to_parent?JS_DupValue(ctx,proxy):JS_GetGlobalObject(ctx);
        JS_DefinePropertyValueStr(ctx,proxy,"parent",JS_DupValue(ctx,parent),0);
        JS_DefinePropertyValueStr(ctx,proxy,"top",parent,0);
    }return proxy;
}
static JSValue frame_make_dispatch(JSContext *ctx)
{
    static const char src[]="(function(Event){return function(data,origin,source,ports){"
        "return new Event('message',{data:data,origin:origin,source:source,ports:ports||[]});"
        "}})(MessageEvent)";
    return JS_Eval(ctx,src,sizeof src-1,"<embedded-message>",JS_EVAL_TYPE_GLOBAL);
}
static struct pf_window *frame_window_for(JSContext *ctx,struct node *host)
{
    for(int i=0;i<frame_window_count;i++){
        struct pf_window *w=&frame_windows[i];
        if(w->parent_ctx==ctx&&w->host==host&&w->serial==host->serial)return w;
    }
    if(frame_window_count==PF_WINDOWS)return 0;
    struct pf_window *w=&frame_windows[frame_window_count];memset(w,0,sizeof *w);
    w->host=host;w->serial=host->serial;w->root=js_dom_root();w->parent_ctx=ctx;
    if(!frame_origin(js_page_location(),w->origin))return 0;
    w->proxy=frame_make_proxy(ctx,w,0);
    w->dispatch=frame_make_dispatch(ctx);
    if(JS_IsException(w->proxy)||JS_IsException(w->dispatch)){
        JS_FreeValue(ctx,w->proxy);JS_FreeValue(ctx,w->dispatch);return 0;
    }
    dom_subscribe(w->root->doc,&w->subscription,frame_window_mutation,w);
    frame_window_count++;return w;
}
JSValue passive_frame_window(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)self;if(active_frame||argc<1)return JS_NULL; /* nested owners absent */
    struct node *host=js_dom_node_from(argv[0]);if(!tag(host,"iframe"))return JS_NULL;
    struct pf_window *w=frame_window_for(ctx,host);
    return w?JS_DupValue(ctx,w->proxy):JS_NULL;
}
static int frame_window_bind(struct pframe *f)
{
    f->parent_proxy=f->message_dispatch=JS_UNDEFINED;
    f->window=frame_window_for(js_page_ctx(),f->host);return f->window!=0;
}
static int frame_window_open_child(struct pframe *f)
{
    JSContext *ctx=js_page_ctx();struct pf_window *w=f->window;
    w->child=f;w->epoch++;
    f->parent_proxy=frame_make_proxy(ctx,w,1);f->message_dispatch=frame_make_dispatch(ctx);
    if(JS_IsException(f->parent_proxy)||JS_IsException(f->message_dispatch))return 0;
    JSValue global=JS_GetGlobalObject(ctx);
    int a=JS_DefinePropertyValueStr(ctx,global,"parent",JS_DupValue(ctx,f->parent_proxy),0);
    int b=JS_DefinePropertyValueStr(ctx,global,"top",JS_DupValue(ctx,f->parent_proxy),0);
    JS_FreeValue(ctx,global);return a>=0&&b>=0;
}
static void frame_window_close_child(struct pframe *f)
{
    JSContext *ctx=js_page_ctx();
    if(ctx){JS_FreeValue(ctx,f->parent_proxy);JS_FreeValue(ctx,f->message_dispatch);}
    f->parent_proxy=f->message_dispatch=JS_UNDEFINED;
    if(f->window&&f->window->child==f){f->window->child=0;f->window->epoch++;}
}
static void frame_message_discard(struct pf_message *m)
{if(m->packet&&LOGIT_HAVE(js_ports_discard))js_ports_discard(m->packet);free(m->bytes);}
static void frame_windows_reset(void)
{
    for(int i=0;i<frame_message_count;i++)frame_message_discard(&frame_messages[i]);
    frame_message_count=0;
    for(int i=0;i<frame_window_count;i++){
        struct pf_window *w=&frame_windows[i];
        dom_unsubscribe(&w->subscription);
        JS_FreeValue(w->parent_ctx,w->proxy);JS_FreeValue(w->parent_ctx,w->dispatch);
    }
    memset(frame_windows,0,sizeof frame_windows);frame_window_count=0;
}
static int frame_messages_pump(void)
{
#ifdef PF_TEST_NO_MESSAGES
    return 0; /* Host negative: input and child execution remain intact. */
#endif
    int count=frame_message_count,changed=0;
    for(int i=0;i<count;i++){
        struct pf_message m=frame_messages[0];
        memmove(frame_messages,frame_messages+1,(size_t)(--frame_message_count)*sizeof *frame_messages);
        struct pf_window *w=m.window;struct pframe *f=w->child;char dest[PF_URL];
        if(!f||m.epoch!=w->epoch||!frame_window_connected(w)){frame_message_discard(&m);continue;}
        if(m.to_parent)strcpy(dest,w->origin);else if(!frame_origin(f->url,dest)){frame_message_discard(&m);continue;}
        if(strcmp(m.target,"*")&&strcmp(m.target,dest)){frame_message_discard(&m);continue;}
        struct js_page_context *old=0;
        if(!js_page_context_activate(m.to_parent?0:f->page,&old)){frame_message_discard(&m);continue;}
        struct layout_context *layout_old=0;if(!m.to_parent)layout_old=layout_context_activate(f->layout);
        active_frame=m.to_parent?0:f;
        JSContext *ctx=js_page_ctx();JSValue args[4];
        args[3]=JS_UNDEFINED;
        args[0]=m.packet?js_ports_read(ctx,m.packet,&args[3]):JS_ReadObject(ctx,m.bytes,m.len,JS_READ_OBJ_REFERENCE);
        frame_message_discard(&m);
        args[1]=JS_NewString(ctx,m.origin);
        args[2]=JS_DupValue(ctx,m.to_parent?w->proxy:f->parent_proxy);
        js_page_slice_begin();
        JSValue result=JS_IsException(args[0])?JS_EXCEPTION:JS_Call(ctx,m.to_parent?w->dispatch:f->message_dispatch,JS_UNDEFINED,4,(JSValueConst *)args);
        if(!JS_IsException(result)){
            JSValue delivered=js_dom_deliver_window_message(result);
            JS_FreeValue(ctx,result);result=delivered;
        }
        if(JS_IsException(result)){JSValue ex=JS_GetException(ctx);const char *why=JS_ToCString(ctx,ex);
            printf("[iframe] message dispatch failed: %s\n",why?why:"exception");
            if(why)JS_FreeCString(ctx,why);JS_FreeValue(ctx,ex);}
        JS_FreeValue(ctx,result);for(int j=0;j<4;j++)JS_FreeValue(ctx,args[j]);
        js_page_pump();js_page_slice_end();
        if(!m.to_parent){f->dirty=1;layout_context_activate(layout_old);}
        active_frame=0;js_page_context_activate(old,0);changed=1;
    }return changed;
}
