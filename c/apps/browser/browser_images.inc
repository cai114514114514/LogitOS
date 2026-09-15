/* SPDX-License-Identifier: MIT
 * new Image() used to construct the right DOM element but only display-list
 * images reached IO. A detached preload waited forever; naturalWidth and
 * complete were undefined. The live verification widget exposed this AFTER
 * dynamic stylesheet loading was repaired (it now painted "Image loading").
 *
 * The DOM sink only records intent, including off-document source writes.
 * Outer frames own IO, real decoding, then load/error. Pending JS handles keep
 * detached nodes alive and invalidate on explicit destruction; completed jobs
 * release those handles. A raw identity is compared only against live nodes,
 * never dereferenced. Pixels use layout's existing 256-entry/64 MiB cache.
 * Image.decode(), responsive srcset selection and canvas drawImage from a
 * detached element are not implemented by this resource-lifecycle patch.
 */
#define DOM_IMAGE_MAX 256
struct dom_image_job {
    struct node *identity;
    uint32_t serial;
    JSValue wrapper;
    char *src;
    int fd, pending, broken;
};
static struct dom_image_job g_dom_images[DOM_IMAGE_MAX];
static int g_dom_image_count;
static unsigned long long g_dom_image_generation=(unsigned long long)-1;
static void dom_images_reset(void)
{
    js_dom_set_image_sink(0);
    for(int i=0;i<g_dom_image_count;i++){
        struct dom_image_job *p=&g_dom_images[i];
        if(p->fd>=0)bfetch_release(p->fd);
        if(js_page_ctx())JS_FreeValue(js_page_ctx(),p->wrapper);
        free(p->src);
    }
    g_dom_image_count=0;g_dom_image_generation=(unsigned long long)-1;
}
static void dom_image_note(struct node *n)
{
#ifndef BROWSER_NO_DOM_IMAGE_LOADING
    if(!n||!tag_is(n->tag,"img")||!js_page_ctx())return;
    const char *src=dom_attr(n,"src");if(!src)src="";
    int i=0;
    for(;i<g_dom_image_count;i++)if(g_dom_images[i].identity==n&&g_dom_images[i].serial==n->serial)break;
    if(i<g_dom_image_count&&g_dom_images[i].src&&str_eq(g_dom_images[i].src,src))return;
    if(i==g_dom_image_count){
        if(!src[0])return;
        if(i==DOM_IMAGE_MAX){printf("[image-dom] per-document job limit 256 reached\n");return;}
        memset(&g_dom_images[i],0,sizeof g_dom_images[i]);g_dom_images[i].fd=-1;
        g_dom_images[i].wrapper=JS_UNDEFINED;g_dom_image_count++;
    }
    struct dom_image_job *p=&g_dom_images[i];
    if(p->fd>=0)bfetch_release(p->fd);
    JS_FreeValue(js_page_ctx(),p->wrapper);free(p->src);
    p->src=sheet_copy(src,(int)strlen(src));p->fd=-1;p->broken=0;
    p->identity=n;p->serial=n->serial;p->pending=p->src&&src[0];
    p->wrapper=p->pending?js_dom_node_value(js_page_ctx(),n):JS_UNDEFINED;
#endif
}
static void dom_image_scan(struct node *n)
{
    if(!n)return;if(n->type==N_ELEM && tag_is(n->tag,"img"))dom_image_note(n);
    for(struct node *c=n->first_child;c;c=c->next)dom_image_scan(c);
}
static int dom_images_pending(void)
{
    for(int i=0;i<g_dom_image_count;i++)if(g_dom_images[i].pending)return 1;
    return 0;
}
static int dom_images_wants(const char *src)
{
    for(int i=0;i<g_dom_image_count;i++)
        if(g_dom_images[i].pending&&g_dom_images[i].src&&str_eq(g_dom_images[i].src,src))return 1;
    return 0;
}
static JSValue dom_image_get(JSContext *ctx,JSValueConst value,int magic)
{
    struct node *n=js_dom_node_from(value);
    if(!n||!tag_is(n->tag,"img"))return JS_ThrowTypeError(ctx,"receiver is not an image");
    const char *src=dom_attr(n,"src");int w=0,h=0;
    int state=layout_img_dimensions(src,&w,&h);
    if(magic==0){
        int complete=!src||!src[0]||state!=0;
        for(int i=0;i<g_dom_image_count;i++)if(g_dom_images[i].identity==n&&g_dom_images[i].serial==n->serial){
            if(!g_dom_images[i].pending&&g_dom_images[i].broken)complete=1;break;}
        return JS_NewBool(ctx,complete);
    }
    if(magic>=3){
        /* Keep existing width/height setters. Rendered dimensions come from
         * the actual box; detached images fall back to attributes/intrinsics. */
        browser_cssom_reflow();
        int bw=0,bh=0;
        if(layout_node_box(n,0,0,&bw,&bh))return JS_NewInt32(ctx,magic==3?bw:bh);
        const char *attr=dom_attr(n,magic==3?"width":"height");
        if(attr){char *end;unsigned long size=strtoul(attr,&end,10);if(end!=attr)return JS_NewUint32(ctx,(uint32_t)size);}
    }
    return JS_NewInt32(ctx,(magic==1||magic==3)?w:h);
}
static void dom_images_install(void)
{
#ifndef BROWSER_NO_DOM_IMAGE_LOADING
    JSContext *ctx=js_page_ctx();JSValue global=JS_GetGlobalObject(ctx);
    JSValue ctor=JS_GetPropertyStr(ctx,global,"HTMLImageElement");
    JSValue proto=JS_GetPropertyStr(ctx,ctor,"prototype");
    if(JS_IsObject(proto)){
        const char *names[]={"complete","naturalWidth","naturalHeight","width","height"};
        for(int i=0;i<5;i++){
            JSAtom atom=JS_NewAtom(ctx,names[i]);JSPropertyDescriptor old;
            JSValue setter=JS_UNDEFINED;
            if(JS_GetOwnProperty(ctx,&old,proto,atom)>0){
                if(i>=3)setter=old.setter;else JS_FreeValue(ctx,old.setter);
                JS_FreeValue(ctx,old.value);JS_FreeValue(ctx,old.getter);
            }
            JS_DefinePropertyGetSet(ctx,proto,atom,
                JS_NewCFunctionMagic(ctx,(JSCFunctionMagic *)dom_image_get,names[i],0,JS_CFUNC_getter_magic,i),
                setter,JS_PROP_CONFIGURABLE|JS_PROP_ENUMERABLE);
            JS_FreeAtom(ctx,atom);
        }
    }
    JS_FreeValue(ctx,proto);JS_FreeValue(ctx,ctor);JS_FreeValue(ctx,global);
    js_dom_set_image_sink(dom_image_note);
#endif
}
static int dom_images_frame(void)
{
#ifndef BROWSER_NO_DOM_IMAGE_LOADING
    if(!g_root||!js_page_ctx())return 0;
    unsigned long long generation=js_dom_mutation_generation();
    if(generation!=g_dom_image_generation){g_dom_image_generation=generation;dom_image_scan(g_root);}
    if(dom_images_pending())bfetch_pump();
    int changed=0;
    for(int i=0;i<g_dom_image_count;i++){
        struct dom_image_job *p=&g_dom_images[i];if(!p->pending)continue;
        struct node *n=js_dom_node_from(p->wrapper);
        if(!n){if(p->fd>=0)bfetch_release(p->fd);p->fd=-1;p->pending=0;
            JS_FreeValue(js_page_ctx(),p->wrapper);p->wrapper=JS_UNDEFINED;continue;}
        int state=layout_img_dimensions(p->src,0,0);
        if(!state){
#ifndef BROWSER_DUPLICATE_IMAGE_IO
            /* The original detached-image implementation opened its own
             * bfetch alongside the display-list loader. browser-loading's
             * held responses exposed two requests for each inserted img and
             * two navigation cancellations. Both consumers now use ONE
             * bounded queue, body handoff and decoded cache. Keeping a DOM
             * wrapper is event ownership, never another transport ownership.
             * Do not cancel merely on detachment: new Image() is intentionally
             * off-document. A source replacement or navigation drops interest.
             * The named control restores the duplicate-IO path below. */
            unsigned char *bytes=0;int len=0;
            int rc=image_fetch_ready(p->src,&bytes,&len);
            if(rc>0)continue;
            state=layout_img_store(p->src,rc==0?bytes:0,rc==0?len:0)?1:-1;
            free(bytes);
#else
            if(p->fd<0){
                /* res_fetch owns data: decoding already; ordinary network
                 * images use persistent bfetch requests, not synchronous IO. */
                if(starts_ci(p->src,"data:")){
                    unsigned char *bytes=0;int len=0;
                    int rc=res_fetch(p->src,&bytes,&len);
                    state=rc==0&&layout_img_store(p->src,bytes,len)?1:-1;free(bytes);
                }else{
                    char absolute[2048];
                    if(bfetch_resolve(committed_page_url(),p->src,absolute,sizeof absolute)<0)state=-1;
                    else {p->fd=bfetch_start_from(committed_page_url(),p->src);if(p->fd<0)continue;}
                }
            }
            if(p->fd>=0){
                if(bfetch_state(p->fd)==BF_PENDING)continue;
                int len=0;const unsigned char *bytes=bfetch_body(p->fd,&len);
                state=bfetch_state(p->fd)==BF_DONE && bfetch_status(p->fd)/100==2 &&
                      layout_img_store(p->src,bytes,len)?1:-1;
                if(state>0){const char *final=bfetch_url(p->fd);if(final)tab_keep_res(tab_cur(),final,bytes,len);}
                bfetch_release(p->fd);p->fd=-1;
            }
#endif
        }
        if(p->fd>=0){bfetch_release(p->fd);p->fd=-1;}
        p->pending=0;p->broken=state<0;
        JSValue wrapper=p->wrapper;p->wrapper=JS_UNDEFINED;
        struct js_event_init ev={0};js_dom_dispatch(n,state>0?"load":"error",&ev);
        JS_FreeValue(js_page_ctx(),wrapper);changed=1;
    }
    return changed;
#else
    return 0;
#endif
}
