/* Passive network frames own a DOM and CSS/layout/image context, never a JS
 * wrapper or parent fetch response. Completed child display lists are painted
 * in their host's clipped viewport. No scripts, child navigation, nested
 * frames, form submission, or parent/child DOM access is implemented here.
 * Those require independent browsing contexts, not another singleton alias.
 *
 * Bounds: four frames, one request per frame, depth one, 2 MiB encoded/raw
 * resources per frame, sixteen styles and thirty-two images. layout_context
 * independently bounds retained decoded pixels. Network also has a 512 KiB
 * per-response ceiling; decompression and URL parsing cannot silently truncate
 * a resource into a successful page. */
/* Correction (2026-09-15): network frames now own page/WebAPI/platform
 * contexts, classic script loading, native input and a WindowProxy message
 * bridge. The 512 KiB preview response ceiling is now a bounded 4 MiB, with
 * 8 MiB aggregate/frame. Modules, nested active frames, native form owners,
 * direct same-origin DOM and full parser-blocking script order remain absent.
 * The old passive paragraph describes the previous implementation only. */
#include "passive_frame.h"
#include "layout.h"
#include "css.h"
#include "css_import.h"
#include "dom.h"
#include "bfetch.h"
#include "iframe_policy.h"
#include "js_page.h"
#include "js_dom.h"
#include "js_webapi.h"
#include "js_platform.h"
#include "js_module.h"
#include "focus.h"
#define JS_WORKER_OPTIONAL
#include "js_worker.h"
#define JS_PORTS_OPTIONAL
#include "js_ports.h"
#include "browser_paint.h"
#include "../../net/http/cookies.h"
#ifndef URL_CORE_ONLY
#define URL_CORE_ONLY
#endif
#include "js_url.h"
#include "url.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define PF_MAX 4
#define PF_SHEETS 16
#define PF_IMAGES 32
#define PF_URL 600
#define PF_HTML (256*1024)
#define PF_CSS (128*1024)
#define PF_IMAGE (512*1024)
#define PF_BYTES (8*1024*1024)
#define PF_SCRIPTS 128
#define PF_SCRIPT BROWSER_EMBEDDED_BODY_MAX
#ifdef PF_TEST_SMALL_SCRIPT
#undef PF_SCRIPT
#define PF_SCRIPT (512*1024) /* Host control: former preview script ceiling. */
#endif
struct pf_script {struct node *node;uint32_t serial;int parser;};
enum { PF_FETCH=1,PF_STYLES,PF_PIXELS,PF_READY,PF_FAILED,PF_WAIT_GEOMETRY };
struct pf_sheet {struct node *node;char url[PF_URL];};
struct pframe {
    struct node *host,*root;uint32_t serial;int seen,state,id,width,height,dirty,request_kind,rawlen,scripts_disabled;
    char src[PF_URL],url[PF_URL],base[PF_URL],csp[4096],xfo[256],image[PF_URL];
    unsigned char *raw;char *css,*expanded;int csslen,csscap,expandedcap,bytes;
    struct layout_context *layout;unsigned generation;
    struct pf_sheet sheets[PF_SHEETS];int nsheet,sheet,images;
    struct js_page_context *page;
    struct pf_script scripts[PF_SCRIPTS];int nscript,script,script_id,loaded,runtime_pending;
    unsigned long long mutation;
    int scroll_x,scroll_y;
    struct node *press;uint32_t press_serial;
    int want_focus;
    struct pf_window *window;
    JSValue parent_proxy,message_dispatch;
    JSContext *ctx;struct dom_subscription policy_subscription;int policy_failed,retire;
};
static struct pframe frames[PF_MAX];
/* Only selected while a native entry owns the child's page AND layout. */
static struct pframe *active_frame,*focused_frame;
static struct node *parent_root;
static char parent_url[PF_URL],parent_base[PF_URL],parent_csp[4096];
static int parent_known;
static unsigned long long parent_mutation=(unsigned long long)-1;
static unsigned next_generation=1;
static void frame_script_offer(struct node *n);
static int frame_runtime_open(struct pframe *f);
static int frame_runtime_pump(struct pframe *f);
static int frame_window_bind(struct pframe *f);
static int frame_window_open_child(struct pframe *f);
static void frame_window_close_child(struct pframe *f);
static void frame_windows_reset(void);
static int frame_messages_pump(void);
static int frame_messages_pending(void);

int passive_frames_enabled(void){return 1;}
static int tag(const struct node *n,const char *s){return n&&n->type==N_ELEM&&!strcmp(n->tag,s);}
static struct node *next_node(struct node *n,struct node *root)
{if(n->first_child)return n->first_child;while(n&&n!=root&&!n->next)n=n->parent;return n&&n!=root?n->next:0;}
static int ci_eq(const char *a,const char *b)
{if(!a||!b)return 0;while(*a&&*b){int c=*a++,d=*b++;if(c>='A'&&c<='Z')c+=32;if(d>='A'&&d<='Z')d+=32;if(c!=d)return 0;}return !*a&&!*b;}
static int ci_has(const char *s,const char *part)
{if(!s)return 0;for(;*s;s++){const char *p=s,*q=part;while(*q&&*p){int c=*p++,d=*q++;if(c>='A'&&c<='Z')c+=32;if(c!=d)break;if(!*q)return 1;}}return 0;}

/* Standard resolution first, then explicit transport capacities. A successful
 * url.c parse is insufficient: it silently truncates overlong paths/hosts. */
static int resolve_url(const char *base,const char *ref,char *out,int cap)
{
    if(!ref||!ref[0]||strlen(ref)>=PF_URL)return 0;
    for(const char *p=ref;*p;p++)if((unsigned char)*p<32||*p=='\\')return 0;
    urlrec *b=base?url_parse_w(base,-1,0):0,*u=url_parse_w(ref,-1,b);if(b)url_free_w(b);if(!u)return 0;
    char *href=url_get(u,URLC_HREF),*proto=url_get(u,URLC_PROTOCOL),*host=url_get(u,URLC_HOSTNAME),
         *path=url_get(u,URLC_PATHNAME),*query=url_get(u,URLC_SEARCH),*hash=url_get(u,URLC_HASH),
         *user=url_get(u,URLC_USERNAME),*pass=url_get(u,URLC_PASSWORD);
    int ok=href&&proto&&host&&path&&query&&hash&&user&&pass&&
        (!strcmp(proto,"http:")||!strcmp(proto,"https:"))&&host[0]&&!strchr(host,':')&&
        !user[0]&&!pass[0]&&strlen(host)<URL_HOST_MAX&&strlen(href)<(size_t)cap&&
        strlen(path)+strlen(query)+strlen(hash)<URL_PATH_MAX;
    if(ok)strcpy(out,href);
    free(href);free(proto);free(host);free(path);free(query);free(hash);free(user);free(pass);url_free_w(u);return ok;
}
static int media_type(const char *value,const char *type)
{
    if(!value)return 0;while(*value==' '||*value=='\t')value++;
    size_t n=0;while(value[n]&&value[n]!=';'&&value[n]!=' '&&value[n]!='\t')n++;
    if(n!=strlen(type))return 0;
    for(size_t i=0;i<n;i++){int c=(unsigned char)value[i];if(c>='A'&&c<='Z')c+=32;if(c!=type[i])return 0;}
    value+=n;while(*value==' '||*value=='\t')value++;
    return !*value||*value==';';
}
static int add_policy(char *out,int cap,const char *v)
{if(!v||!*v)return 1;size_t n=strlen(out),m=strlen(v);if(n+m+1>=(size_t)cap)return 0;if(n)out[n++]='\n';memcpy(out+n,v,m+1);return 1;}
/* Deduplicate whole policy lines. A substring match would lose a newly
 * inserted restrictive policy merely because its bytes occur inside an older
 * source token. Meta restrictions remain additive after DOM removal. */
static int add_new_policies(char *out,int cap,const char *values)
{
    for(const char *p=values;*p;){const char *end=strchr(p,'\n');size_t n=end?(size_t)(end-p):strlen(p);int found=0;
        for(const char *q=out;*q;){const char *qe=strchr(q,'\n');size_t qn=qe?(size_t)(qe-q):strlen(q);if(n==qn&&!memcmp(p,q,n)){found=1;break;}if(!qe)break;q=qe+1;}
        if(n&&!found){size_t len=strlen(out);if(len+n+1>=(size_t)cap)return 0;if(len)out[len++]='\n';memcpy(out+len,p,n);out[len+n]=0;}
        if(!end)break;p=end+1;
    }return 1;
}
static int meta_policy(struct node *root,char *out,int cap)
{
    for(struct node *n=root;n;n=next_node(n,root))if(tag(n,"meta")&&ci_eq(dom_attr(n,"http-equiv"),"content-security-policy")){
        const char *v=dom_attr(n,"content");
        /* frame-ancestors is forbidden in meta. Treating it as a response
         * header would incorrectly disable XFO; refuse this unsupported case. */
        if(ci_has(v,"frame-ancestors")||!add_policy(out,cap,v))return 0;
    }
    return 1;
}
static void drop_content(struct pframe *f)
{
    f->runtime_pending=0;
    dom_unsubscribe(&f->policy_subscription);
    if(focused_frame==f)focused_frame=0;
    if(f->page){struct js_page_context *old=0;
        if(js_page_context_activate(f->page,&old)){
            focus_reset();fc_set_dispatch(NULL);
            frame_window_close_child(f);
            js_page_close();js_page_context_activate(old,0);js_page_context_destroy(f->page);
        }f->page=0;
    }
    if(f->script_id>=0&&f->nscript)bfetch_release(f->script_id);f->script_id=-1;
    if(f->id>=0)bfetch_release(f->id);f->id=-1;
    if(f->layout){struct layout_context *old=layout_context_activate(f->layout);layout_free();if(f->root)dom_free(f->root);f->root=0;layout_context_activate(old);layout_context_destroy(f->layout);f->layout=0;}
    else if(f->root){dom_free(f->root);f->root=0;}
    free(f->raw);free(f->css);free(f->expanded);f->raw=0;f->css=f->expanded=0;
}
static void fail_frame(struct pframe *f,const char *reason)
{drop_content(f);f->state=PF_FAILED;f->generation=next_generation++;printf("[iframe] passive refused: %s\n",reason);}
void passive_frames_reset(void)
{for(int i=0;i<PF_MAX;i++){if(frames[i].state)drop_content(&frames[i]);memset(&frames[i],0,sizeof frames[i]);frames[i].id=-1;}frame_windows_reset();parent_root=0;parent_known=0;parent_url[0]=parent_base[0]=parent_csp[0]=0;parent_mutation=(unsigned long long)-1;}
void passive_frames_set_parent(const char *url,const char *csp,int known)
{parent_known=known&&url&&strlen(url)<sizeof parent_url&&(!csp||strlen(csp)<sizeof parent_csp);parent_url[0]=parent_base[0]=parent_csp[0]=0;if(parent_known){strcpy(parent_url,url);strcpy(parent_base,url);if(csp)strcpy(parent_csp,csp);}parent_mutation=(unsigned long long)-1;}
int passive_frame_content_box(const struct node *n,int w,int h,int *x,int *y,int *cw,int *ch)
{
    const struct cstyle *s=n?n->style:0;if(!s)return 0;
    *x=s->border_w[3]+s->pl;*y=s->border_w[0]+s->pt;
    *cw=w-*x-s->border_w[1]-s->pr;*ch=h-*y-s->border_w[2]-s->pb;
    return *cw>0&&*ch>0&&*cw<=2048&&*ch<=2048;
}
static int host_size(const struct node *host,int *w,int *h)
{int bw,bh,x,y;return layout_node_box(host,0,0,&bw,&bh)&&passive_frame_content_box(host,bw,bh,&x,&y,w,h);}
int passive_frame_view(const struct node *host,struct passive_frame_view *out)
{for(int i=0;i<PF_MAX;i++){struct pframe *f=&frames[i];if(f->host==host&&f->state>=PF_PIXELS&&f->state<=PF_READY&&f->layout){if(out){out->layout=f->layout;out->width=f->width;out->height=f->height;out->scripts_disabled=f->scripts_disabled;out->generation=f->generation;out->scroll_x=f->scroll_x;out->scroll_y=f->scroll_y;}return 1;}}return 0;}
int passive_frames_pending(void)
{
#ifndef PF_TEST_NO_MESSAGE_WAKE
    if(frame_messages_pending())return 1;
#endif
    for(int i=0;i<PF_MAX;i++)if((frames[i].state>=PF_FETCH&&frames[i].state<PF_READY)||frames[i].runtime_pending)return 1;
    return 0;
}
static int resolve_css(void *ctx,const char *base,const char *ref,char *out,int cap)
{(void)ctx;return resolve_url(base,ref,out,cap)?0:-1;}
static int no_import(void *ctx,const char *url,unsigned char **out,int *len,char *final,int cap)
{(void)ctx;(void)url;(void)out;(void)len;(void)final;(void)cap;return -1;}
static int append_sheet(struct pframe *f,const char *text,int len,const char *base)
{
    struct css_import_budget budget={0};struct css_import_io io={resolve_css,no_import,0};
    int end=css_import_expand_alloc(text,len,base,&f->css,f->csslen,&f->csscap,PF_CSS,&budget,&io);
    if(end<0)return 0;f->csslen=end;
    if(budget.failed||budget.unsupported)printf("[iframe] passive stylesheet imports unavailable\n");return 1;
}
static int style_layout(struct pframe *f)
{
    /* Interactive CSS asks js_dom_dirty/focus_current. Styles fetched before
     * the script pump must therefore select the child owner as well. Never
     * try to switch away while a child focus callback is already on stack. */
    struct js_page_context *page_old=NULL;int switched=f->ctx&&js_page_ctx()!=f->ctx;
    if(switched&&!js_page_context_activate(f->page,&page_old))return 0;
    struct layout_context *old=layout_context_activate(f->layout);css_viewport(f->width,f->height);
    int len=css_expand_vars_alloc(f->css?f->css:"",f->csslen,&f->expanded,&f->expandedcap,PF_CSS);
    if(len>=0){css_apply(f->root,f->expanded,len);css_extra_apply(f->root,f->expanded,len);layout_page(f->root,f->width);if(!layout_items())len=-1;}
    layout_context_activate(old);if(switched)js_page_context_activate(page_old,NULL);
    if(len<0)return 0;f->dirty=0;f->generation=next_generation++;return 1;
}
static int prepare_document(struct pframe *f)
{
    f->root=dom_parse((char *)f->raw,f->rawlen);if(!f->root)return 0;
    if(!meta_policy(f->root,f->csp,sizeof f->csp)||!iframe_policy_frame(parent_url,f->url,parent_csp,f->csp,f->xfo))return 0;
    strcpy(f->base,f->url);
    for(struct node *n=f->root;n;n=next_node(n,f->root))if(tag(n,"base")&&dom_attr(n,"href")){
        char candidate[PF_URL];
        /* A blocked base is ignored, not installed as a fetch resolution
         * context. base-uri has no default-src fallback. */
        if(resolve_url(f->url,dom_attr(n,"href"),candidate,sizeof candidate)&&
           iframe_policy_base(f->url,candidate,f->csp))strcpy(f->base,candidate);
        break;
    }
    int allow_attr=iframe_policy_style_attribute(f->csp);
    for(struct node *n=f->root;n;n=next_node(n,f->root)){
        if(n->type!=N_ELEM)continue;
        if(tag(n,"script"))f->scripts_disabled=1;
        if(!allow_attr)dom_remove_attr(n,"style");
        if(tag(n,"iframe"))continue; /* depth one, no nested document loader */
        if(tag(n,"style") || (tag(n,"link")&&ci_has(dom_attr(n,"rel"),"stylesheet"))){
            if(f->nsheet>=PF_SHEETS)return 0;
            struct pf_sheet *s=&f->sheets[f->nsheet++];s->node=n;
            if(tag(n,"link")){const char *href=dom_attr(n,"href");if(!resolve_url(f->base,href,s->url,sizeof s->url))s->url[0]=0;}
        }
    }
    f->layout=layout_context_create();return f->layout!=0&&frame_runtime_open(f);
}
static int redirect_allowed(void *owner,const char *base,const char *ref,char *out,int cap)
{
    struct pframe *f=owner;if(!resolve_url(base,ref,out,cap))return 0;
    const char *initiator=f->request_kind?f->url:parent_url;
    if(!strncmp(initiator,"https:",6)&&!strncmp(out,"http:",5))return 0;
    return f->request_kind?iframe_policy_resource(f->url,out,f->csp,f->request_kind):
        iframe_policy_frame(parent_url,out,parent_csp,"","");
}
static int request(struct pframe *f,const char *url,int kind)
{
    if(f->bytes>=PF_BYTES)return -1;
    f->request_kind=kind;char checked[PF_URL];
    if(!redirect_allowed(f,kind?f->url:parent_url,url,checked,sizeof checked))return -1;
    return bfetch_start_embedded(checked,kind?f->url:parent_url,parent_url,redirect_allowed,f);
}
static int start_frame(struct pframe *f,struct node *host,const char *src)
{
    memset(f,0,sizeof *f);f->id=f->script_id=-1;f->host=host;f->serial=host->serial;f->seen=1;f->state=PF_FETCH;
    if(!src||strlen(src)>=sizeof f->src)return 0;strcpy(f->src,src);
    if(!parent_known||dom_attr(host,"sandbox")||dom_attr(host,"srcdoc")||dom_attr(host,"csp")||dom_attr(host,"credentialless"))return 0;
    if(!resolve_url(parent_base,src,f->url,sizeof f->url)||!iframe_policy_frame(parent_url,f->url,parent_csp,"",""))return 0;
    /* A hidden/unstyled frame is not a policy failure. Wait without a network
     * wake until real layout provides a viewport; identical src must remain
     * eligible when a later stylesheet or resize makes it visible. */
    if(!host_size(host,&f->width,&f->height)){f->state=PF_WAIT_GEOMETRY;return 1;}
    f->id=request(f,f->url,0);return f->id>=0;
}
static int finish_response(struct pframe *f,int limit,unsigned char **out)
{
    if(bfetch_state(f->id)!=BF_DONE||bfetch_status(f->id)/100!=2)return -1;
    int len=0;bfetch_body(f->id,&len);if(len<0||len>limit||f->bytes>PF_BYTES-len){f->bytes=PF_BYTES;return -1;}
    len=bfetch_take(f->id,out);f->id=-1;if(len>=0)f->bytes+=len;return len;
}
static int frame_inline_handlers(void)
{return active_frame&&!active_frame->policy_failed&&iframe_policy_script_attribute(active_frame->csp);}
static int frame_connect(void *owner,const char *url)
{struct pframe *f=owner;return !f->policy_failed&&iframe_policy_resource(f->url,url,f->csp,IF_POLICY_CONNECT);}
static int frame_connected(struct pframe *f,struct node *n,uint32_t serial)
{if(!n||n->serial!=serial)return 0;while(n&&n!=f->root)n=n->parent;return n==f->root;}
#include "embedded_window.inc"
#include "embedded_focus.inc"
static void frame_policy_mutation(void *owner,const struct dom_mutation *m)
{
    struct pframe *f=owner;
    if(m->kind!=DOM_MUT_INSERT&&m->kind!=DOM_MUT_ATTRIBUTE)return;
    if(m->kind==DOM_MUT_ATTRIBUTE&&!tag(m->node,"meta"))return;
    struct node *n=m->node;while(n&&n!=f->root)n=n->parent;if(n!=f->root)return;
    int has_meta=0;
    for(n=m->node;n;n=next_node(n,m->node))if(tag(n,"meta")){has_meta=1;break;}
    if(!has_meta)return;
    char meta[4096]={0};
    if(!meta_policy(f->root,meta,sizeof meta)||!add_new_policies(f->csp,sizeof f->csp,meta)||
       ci_has(f->csp,"require-trusted-types-for"))f->policy_failed=1;
    /* This is a synchronous native mutation notification, not another JS
     * entry: a following eval/fetch in the same author callback sees it. */
    JS_SetStringCodeGenerationAllowed(f->ctx,!f->policy_failed&&iframe_policy_eval(f->csp));
}
static void frame_script_offer(struct node *n)
{
    struct pframe *f=active_frame;
    if(!f||dom_script_is_done(n))return;
    if(f->nscript==PF_SCRIPTS){dom_script_mark_done(n);f->scripts_disabled=1;return;}
    /* Preparation is once per native node, even if author code reinserts it. */
    dom_script_mark_done(n);
    f->scripts[f->nscript++]=(struct pf_script){n,n->serial,0};f->runtime_pending=1;
}
static int frame_worker_allow(void *owner,int op,const char *url)
{
    struct pframe *f=owner;if(f->policy_failed||f->retire)return 0;
    if(op==JSW_CREATE)return url&&!strncmp(url,"blob:",5)?iframe_policy_blob_worker(f->csp):
        iframe_policy_network_worker(f->url,url,f->csp);
    if(op==JSW_EVAL)return iframe_policy_eval(f->csp);
    if(op==JSW_CONNECT)return iframe_policy_resource(f->url,url,f->csp,IF_POLICY_CONNECT);
    return op==JSW_IMPORT&&iframe_policy_worker_import(f->url,url,f->csp);
}
static int frame_worker_redirect(void *owner,const char *base,const char *ref,char *out,int cap)
{return resolve_url(base,ref,out,cap)&&frame_worker_allow(owner,JSW_IMPORT,out);}
static int frame_worker_load(void *owner,const char *url,unsigned char **bytes,int *length)
{
    struct pframe *f=owner;char final[PF_URL];
    if(!frame_worker_redirect(f,f->url,url,final,sizeof final)||f->bytes>=PF_BYTES)return -1;
    int id=bfetch_start_embedded(final,f->url,parent_url,frame_worker_redirect,f);
    if(id<0)return -1;
    /* importScripts is synchronous, but uses the bounded embedded transport
     * and its per-hop policy/Cookie ancestry rather than top-page bfetch_sync.
     * No page callback may re-enter while this worker owns the native stack. */
    bfetch_wait(id,NULL);int len=0;bfetch_body(id,&len);
    const char *ct=bfetch_response_header(id,"content-type");
    int mime=media_type(ct,"text/javascript")||media_type(ct,"application/javascript")||
        media_type(ct,"text/ecmascript")||media_type(ct,"application/ecmascript");
    if(bfetch_state(id)!=BF_DONE||bfetch_status(id)/100!=2||!mime||len<0||len>PF_SCRIPT||
       f->bytes>PF_BYTES-len||!frame_worker_redirect(f,f->url,bfetch_url(id),final,sizeof final)){
        bfetch_release(id);return -1;
    }
    len=bfetch_take(id,bytes);if(len<0)return -1;
    f->bytes+=len;*length=len;return 0;
}
/* A network worker is not governed by its creator's script-src after entry.
 * Keep the final response's policy in separate native storage until its
 * runtime and fetch realm have closed. The frame outlives all its workers. */
struct frame_worker_response {struct pframe *frame;char url[PF_URL],csp[4096];};
static int frame_worker_response_allow(void *owner,int op,const char *url)
{
    struct frame_worker_response *r=owner;
    if(r->frame->policy_failed||r->frame->retire)return 0;
    if(op==JSW_EVAL)return iframe_policy_eval(r->csp);
    if(op==JSW_CONNECT)return iframe_policy_resource(r->url,url,r->csp,IF_POLICY_CONNECT);
    return op==JSW_IMPORT&&iframe_policy_worker_import(r->url,url,r->csp);
}
static int frame_worker_response_redirect(void *owner,const char *base,const char *ref,char *out,int cap)
{return resolve_url(base,ref,out,cap)&&frame_worker_response_allow(owner,JSW_IMPORT,out);}
static int frame_worker_entry_redirect(void *owner,const char *base,const char *ref,char *out,int cap)
{return resolve_url(base,ref,out,cap)&&frame_worker_allow(owner,JSW_CREATE,out);}
static int frame_worker_response_load(void *owner,const char *url,unsigned char **bytes,int *length)
{
    struct frame_worker_response *r=owner;struct pframe *f=r->frame;char final[PF_URL];
    if(!frame_worker_response_redirect(r,r->url,url,final,sizeof final)||f->bytes>=PF_BYTES)return -1;
    int id=bfetch_start_embedded(final,r->url,parent_url,frame_worker_response_redirect,r);
    if(id<0)return -1;
    bfetch_wait(id,NULL);int len=0;bfetch_body(id,&len);
    const char *ct=bfetch_response_header(id,"content-type");
    int mime=media_type(ct,"text/javascript")||media_type(ct,"application/javascript")||
        media_type(ct,"text/ecmascript")||media_type(ct,"application/ecmascript");
    if(bfetch_state(id)!=BF_DONE||bfetch_status(id)/100!=2||!mime||len<0||len>PF_SCRIPT||
       f->bytes>PF_BYTES-len||!frame_worker_response_redirect(r,r->url,bfetch_url(id),final,sizeof final)){
        bfetch_release(id);return -1;
    }
    len=bfetch_take(id,bytes);if(len<0)return -1;
    f->bytes+=len;*length=len;return 0;
}
static int frame_worker_entry(void *owner,char *url,int cap,unsigned char **bytes,int *length,
                              struct js_worker_policy *policy)
{
    struct pframe *f=owner;char final[PF_URL];
    if(!frame_worker_entry_redirect(f,f->url,url,final,sizeof final)||f->bytes>=PF_BYTES)return -1;
    int id=bfetch_start_embedded(final,f->url,parent_url,frame_worker_entry_redirect,f);
    if(id<0)return -1;
    bfetch_wait(id,NULL);int len=0;bfetch_body(id,&len);
    struct frame_worker_response *r=calloc(1,sizeof *r);
    const char *ct=bfetch_response_header(id,"content-type");
    int mime=media_type(ct,"text/javascript")||media_type(ct,"application/javascript")||
        media_type(ct,"text/ecmascript")||media_type(ct,"application/ecmascript");
    const char *csp=bfetch_response_header(id,"content-security-policy");
    int ok=r&&bfetch_state(id)==BF_DONE&&bfetch_status(id)/100==2&&mime&&
        bfetch_response_policy_known(id)&&len>=0&&len<=PF_SCRIPT&&f->bytes<=PF_BYTES-len&&
        frame_worker_entry_redirect(f,f->url,bfetch_url(id),final,sizeof final)&&
        (int)strlen(final)<cap&&add_policy(r->csp,sizeof r->csp,csp);
    /* These policies need separate enforcement machinery, not a successful
     * Worker with silently ignored response constraints. */
    if(ok&&(ci_has(r->csp,"sandbox")||ci_has(r->csp,"require-trusted-types-for")))ok=0;
    if(!ok){free(r);bfetch_release(id);printf("[iframe] network Worker response refused: policy/mime/status/budget\n");return -1;}
    len=bfetch_take(id,bytes);if(len<0){free(r);return -1;}
    strcpy(url,final);strcpy(r->url,final);r->frame=f;f->bytes+=len;*length=len;
    *policy=(struct js_worker_policy){r,frame_worker_response_allow,frame_worker_response_load,0,0,free};
    printf("[iframe] network Worker response: bytes=%d policy-owned=1\n",len);return 0;
}
static int frame_runtime_open(struct pframe *f)
{
    /* Trusted Types enforcement and sandbox execution are not implemented.
     * Retain the inert preview for those policies, never weaken the policy. */
    if(ci_has(f->csp,"require-trusted-types-for")){
        printf("[iframe] execution unavailable: Trusted Types enforcement not implemented\n");return 1;
    }
    if(!frame_window_bind(f))return 0;
    struct url child,ancestor;const char *site=0;
    if(!url_parse(f->url,&child)&&!url_parse(parent_url,&ancestor)){
        struct cookie_ctx dst={child.host,child.path,child.https,1};
        struct cookie_request req={ancestor.host,ancestor.https,0,1,0};
        if(cookie_request_kind(&dst,&req)==CK_REQ_SAME_SITE)site=parent_url;
    }
    f->page=js_page_context_create();
    if(!f->page||!js_page_context_enable_webapi(f->page,site)||!js_page_context_enable_platform(f->page))return 0;
    if(LOGIT_HAVE(js_worker_context_create)){
        struct js_worker_policy p={f,frame_worker_allow,frame_worker_load,site,frame_worker_entry,0};
        if(!js_page_context_enable_workers(f->page,&p))return 0;
    }
    struct js_page_context *old=0;if(!js_page_context_activate(f->page,&old))return 0;
    struct layout_context *layout_old=layout_context_activate(f->layout);
    active_frame=f;js_page_set_location(f->url);
    int ok=js_page_open(f->root);
    if(ok){
        f->ctx=js_page_ctx();dom_subscribe(f->root->doc,&f->policy_subscription,frame_policy_mutation,f);
        JS_SetStringCodeGenerationAllowed(js_page_ctx(),iframe_policy_eval(f->csp));
        ok=js_webapi_set_connect_policy(js_page_ctx(),frame_connect,f)&&frame_window_open_child(f)&&frame_focus_install(f);
        if(ok)css_context_set_interactive(1);
        js_dom_set_inline_handler_policy(frame_inline_handlers);
        js_dom_set_script_sink(frame_script_offer);
        js_webapi_set_viewport(f->width,f->height);js_platform_set_viewport(f->width,f->height);
        f->scripts_disabled=0;
        for(struct node *n=f->root;n;n=next_node(n,f->root))if(tag(n,"script")){
            int before=f->nscript;frame_script_offer(n);
            if(f->nscript>before)f->scripts[before].parser=1;
        }
        f->mutation=js_dom_mutation_generation();
    }
    active_frame=0;layout_context_activate(layout_old);js_page_context_activate(old,0);return ok;
}
static int frame_script_redirect(void *owner,const char *base,const char *ref,char *out,int cap)
{
    struct pframe *f=owner;if(f->script>=f->nscript||!resolve_url(base,ref,out,cap))return 0;
    struct pf_script *s=&f->scripts[f->script];
    if(!frame_connected(f,s->node,s->serial))return 0;
    return !f->policy_failed&&iframe_policy_script(f->url,out,f->csp,dom_attr(s->node,"nonce"),s->parser);
}
static void frame_script_event(struct pframe *f,struct pf_script *s,const char *type)
{if(frame_connected(f,s->node,s->serial)){struct js_event_init ev={0};js_dom_dispatch(s->node,type,&ev);}}
static int frame_run_script(struct pframe *f)
{
    if(f->script>=f->nscript)return 0;
    struct pf_script *s=&f->scripts[f->script];struct node *n=s->node;
    if(!frame_connected(f,n,s->serial)){
        if(f->script_id>=0)bfetch_release(f->script_id);f->script_id=-1;f->script++;return 1;
    }
    const char *type=dom_attr(n,"type"),*src=dom_attr(n,"src");
    if(!js_module_is_classic_type(type)||dom_attr(n,"integrity")){
        /* Module graphs/integrity must not silently run as classic scripts. */
        if(js_module_is_module_type(type)||dom_attr(n,"integrity")){
            f->scripts_disabled=1;printf("[iframe] executable type/integrity support unavailable\n");
        }
        f->script++;return 1;
    }
    unsigned char *raw=0;int len=0,ok=0;char final[PF_URL];strcpy(final,f->url);
    if(src){
        if(f->script_id<0){
            if(f->bytes>=PF_BYTES||!resolve_url(f->base,src,final,sizeof final)||
                !frame_script_redirect(f,f->base,src,final,sizeof final)){
                f->scripts_disabled=1;printf("[iframe] external script refused before request: policy/url/budget\n");frame_script_event(f,s,"error");f->script++;return 1;
            }
            f->script_id=bfetch_start_embedded(final,f->url,parent_url,frame_script_redirect,f);
            if(f->script_id<0){f->scripts_disabled=1;frame_script_event(f,s,"error");f->script++;}
            return 1;
        }
        int id=f->script_id;if(bfetch_state(id)==BF_PENDING)return 0;
        const char *ct=bfetch_response_header(id,"content-type");
        int mime=media_type(ct,"text/javascript")||media_type(ct,"application/javascript")||
            media_type(ct,"application/ecmascript")||media_type(ct,"text/ecmascript");
        bfetch_body(id,&len);
        if(bfetch_state(id)==BF_DONE&&bfetch_status(id)/100==2&&mime&&len>=0&&len<=PF_SCRIPT&&
            f->bytes<=PF_BYTES-len&&frame_script_redirect(f,f->url,bfetch_url(id),final,sizeof final)){
            len=bfetch_take(id,&raw);if(len>=0){f->bytes+=len;ok=1;}
        }else {printf("[iframe] external response refused: state=%d status=%d mime=%d bytes=%d\n",bfetch_state(id),bfetch_status(id),mime,len);bfetch_release(id);}
        f->script_id=-1;
    }else if(!f->policy_failed&&iframe_policy_inline_script(f->csp,dom_attr(n,"nonce"))){
        for(struct node *t=n->first_child;t;t=t->next)if(t->type==N_TEXT){
            if(t->textlen>PF_SCRIPT-len){len=-1;break;}len+=t->textlen;
        }
        if(len>=0&&(raw=malloc((size_t)len+1))){int at=0;
            for(struct node *t=n->first_child;t;t=t->next)if(t->type==N_TEXT){memcpy(raw+at,t->text,t->textlen);at+=t->textlen;}
            raw[len]=0;ok=1;
        }
    }
    if(ok){
        int ran=js_page_eval((char *)raw,len,final,n);
        if(!ran)f->scripts_disabled=1;
        printf("[iframe] classic script: bytes=%d external=%d evaluated=%d\n",len,src!=0,ran);
        if(src)frame_script_event(f,s,"load");
    }
    else {f->scripts_disabled=1;printf("[iframe] script refused: external=%d bytes=%d policy-or-response\n",src!=0,len);if(src)frame_script_event(f,s,"error");}
    free(raw);f->script++;return 1;
}
static int frame_runtime_pump(struct pframe *f)
{
    int notify_load=0;
    struct js_page_context *old=0;if(!js_page_context_activate(f->page,&old))return 0;
    struct layout_context *layout_old=layout_context_activate(f->layout);active_frame=f;
    js_webapi_set_viewport(f->width,f->height);js_platform_set_viewport(f->width,f->height);
    js_dom_set_scroll(f->scroll_x,f->scroll_y);
    /* A bounded scheduling turn keeps parent chrome responsive. External
     * classic scripts are ordered here; async/module scheduling is pending. */
    for(int i=0;i<8&&f->script<f->nscript;i++)if(!frame_run_script(f)||f->script_id>=0)break;
    if(!f->loaded&&f->script==f->nscript){
        struct js_event_init ev={0};f->loaded=1;
        js_platform_document_parsed(js_page_ctx());ev.bubbles=1;js_dom_dispatch(f->root,"DOMContentLoaded",&ev);
    }
    js_page_run_due();
    unsigned long long mutation=js_dom_mutation_generation();int changed=mutation!=f->mutation;
    if(changed){f->mutation=mutation;f->dirty=1;}
    if(f->dirty){if(!style_layout(f))changed=1;else changed=1;}
    if(f->loaded==1&&f->state==PF_READY&&f->script==f->nscript){
        struct js_event_init ev={0};f->loaded=2;js_dom_dispatch(f->root,"load",&ev);changed=1;notify_load=1;
    }
    f->runtime_pending=js_page_pending()||f->script<f->nscript;
    active_frame=0;layout_context_activate(layout_old);js_page_context_activate(old,0);
    if(notify_load&&f->window&&frame_window_connected(f->window)){
        struct js_event_init ev={0};js_dom_dispatch(f->host,"load",&ev);
    }
    return changed;
}
void passive_frames_blur(void){focused_frame=0;}
int passive_frame_pointer(struct node *host,const char *type,struct js_event_init *event)
{
#ifdef PF_TEST_NO_INPUT
    return 0; /* Host negative: same live child, disconnected native input. */
#endif
    struct pframe *f=0;for(int i=0;i<PF_MAX;i++)if(frames[i].host==host&&frames[i].page&&
        frames[i].state>=PF_PIXELS&&frames[i].state<=PF_READY){f=&frames[i];break;}
    if(!f)return 0;
    struct js_page_context *old=0;if(!js_page_context_activate(f->page,&old))return 0;
    struct layout_context *layout_old=layout_context_activate(f->layout);active_frame=f;
    struct node *n=0;browser_hittest_node_scroll(event->client_x,event->client_y,f->scroll_x,f->scroll_y,&n,0,0);
    if(!strcmp(type,"mousedown")){
        focused_frame=f;f->press=n;f->press_serial=n?n->serial:0;
        css_interaction_active(n);css_interaction_hover(n);
    }
    if(!strcmp(type,"mousemove"))css_interaction_hover(n);
    int allow=js_dom_dispatch(n?n:f->root,type,event);
    if(!strcmp(type,"mousedown")&&allow){
        if(js_dom_mutation_generation()!=f->mutation)style_layout(f);
        if(n&&!frame_connected(f,n,f->press_serial))n=NULL;
        struct node *candidate=n;while(candidate&&!focus_is_focusable(candidate))candidate=candidate->parent;
        focus_set(candidate);f->want_focus=1;
    }
    if(!strcmp(type,"mouseup")){
        css_interaction_active(0);
        if(allow&&n==f->press&&frame_connected(f,n,f->press_serial))js_dom_dispatch(n,"click",event);
        f->press=0;
    }
    if(!strcmp(type,"wheel")&&allow){
        int max=layout_height()-f->height;if(max<0)max=0;
        f->scroll_y+=(int)event->delta_y;if(f->scroll_y<0)f->scroll_y=0;if(f->scroll_y>max)f->scroll_y=max;
        js_dom_set_scroll(f->scroll_x,f->scroll_y);
    }
    f->dirty=1;f->generation=next_generation++;
    active_frame=0;layout_context_activate(layout_old);js_page_context_activate(old,0);frame_focus_commit_parent(f);return 1;
}
int passive_frame_key(struct js_event_init *event)
{
    struct pframe *f=focused_frame;if(!f||!f->page)return 0;
    struct js_page_context *old=0;if(!js_page_context_activate(f->page,&old))return 0;
    struct layout_context *layout_old=layout_context_activate(f->layout);active_frame=f;
    struct node *n=focus_current();if(!n)n=f->root;
    int allow=js_dom_dispatch(n,"keydown",event);
    if(allow&&event->key&&!strcmp(event->key,"Tab")){focus_advance(f->root,event->shift);f->want_focus=1;}
    f->dirty=1;f->generation=next_generation++;
    active_frame=0;layout_context_activate(layout_old);js_page_context_activate(old,0);return 1;
}
static int advance_frame(struct pframe *f)
{
    if(f->state==PF_FETCH){
        if(bfetch_state(f->id)==BF_PENDING)return 0;
        printf("[iframe] document response: state=%d status=%d policy-known=%d\n",
               bfetch_state(f->id),bfetch_status(f->id),bfetch_response_policy_known(f->id));
        const char *csp=bfetch_response_header(f->id,"content-security-policy"),*xfo=bfetch_response_header(f->id,"x-frame-options");
        const char *ct=bfetch_response_header(f->id,"content-type");
        if(!bfetch_response_policy_known(f->id)||!media_type(ct,"text/html")||
           !resolve_url(0,bfetch_url(f->id),f->url,sizeof f->url)||!add_policy(f->csp,sizeof f->csp,csp)||!add_policy(f->xfo,sizeof f->xfo,xfo)||
           !iframe_policy_frame(parent_url,f->url,parent_csp,f->csp,f->xfo)||(f->rawlen=finish_response(f,PF_HTML,&f->raw))<0||!prepare_document(f)){
            fail_frame(f,"document, response policy, or budget");return 1;}
        f->state=PF_STYLES;
    }
    if(f->state==PF_STYLES){
        while(f->sheet<f->nsheet){struct pf_sheet *s=&f->sheets[f->sheet];
            if(tag(s->node,"style")){
                if(iframe_policy_inline_style(f->csp,dom_attr(s->node,"nonce"))){
                    int len=0;for(struct node *n=s->node->first_child;n;n=n->next)if(n->type==N_TEXT)len+=n->textlen;
                    if(len>PF_CSS){fail_frame(f,"inline stylesheet budget");return 1;}
                    char *raw=malloc((size_t)len+1);if(!raw){fail_frame(f,"stylesheet allocation");return 1;}int at=0;
                    for(struct node *n=s->node->first_child;n;n=n->next)if(n->type==N_TEXT){memcpy(raw+at,n->text,n->textlen);at+=n->textlen;}raw[at]=0;
                    int ok=append_sheet(f,raw,len,f->base);free(raw);if(!ok){fail_frame(f,"stylesheet budget");return 1;}
                }f->sheet++;continue;
            }
            if(!s->url[0]||!iframe_policy_resource(f->url,s->url,f->csp,IF_POLICY_STYLE)){f->sheet++;continue;}
            if(f->id<0){f->id=request(f,s->url,IF_POLICY_STYLE);if(f->id<0){f->sheet++;continue;}return 0;}
            if(bfetch_state(f->id)==BF_PENDING)return 0;
            char final[PF_URL];unsigned char *raw=0;int len=-1;
            const char *ct=bfetch_response_header(f->id,"content-type");
            if(media_type(ct,"text/css")&&resolve_url(0,bfetch_url(f->id),final,sizeof final)&&iframe_policy_resource(f->url,final,f->csp,IF_POLICY_STYLE))len=finish_response(f,PF_CSS,&raw);
            if(f->id>=0){bfetch_release(f->id);f->id=-1;}
            if(len>=0&&!append_sheet(f,(char *)raw,len,final)){free(raw);fail_frame(f,"stylesheet budget");return 1;}free(raw);f->sheet++;
        }
        if(!style_layout(f)){fail_frame(f,"style/layout allocation");return 1;}f->state=PF_PIXELS;return 1;
    }
    if(f->state==PF_PIXELS||f->state==PF_READY){
        if(f->dirty&&!style_layout(f)){fail_frame(f,"resize layout");return 1;}
        if(f->id>=0){
            if(bfetch_state(f->id)==BF_PENDING)return 0;
            unsigned char *raw=0;int len=-1;char final[PF_URL];
            if(resolve_url(0,bfetch_url(f->id),final,sizeof final)&&iframe_policy_resource(f->url,final,f->csp,IF_POLICY_IMAGE))len=finish_response(f,PF_IMAGE,&raw);
            if(f->id>=0){bfetch_release(f->id);f->id=-1;}
            struct layout_context *old=layout_context_activate(f->layout);
            layout_img_store(f->image,len>=0?raw:0,len>=0?len:0);layout_page(f->root,f->width);
            layout_context_activate(old);free(raw);f->generation=next_generation++;return 1;
        }
        struct layout_context *old=layout_context_activate(f->layout);const struct item *it=layout_items();
        char image[PF_URL]={0};
        for(int i=0;i<layout_count();i++)if(it[i].type==IT_IMAGE&&it[i].imgsrc&&!it[i].img&&!layout_img_dimensions(it[i].imgsrc,0,0)){
            if(strlen(it[i].imgsrc)<sizeof image)strcpy(image,it[i].imgsrc);break;}
        layout_context_activate(old);
        if(!image[0]||f->images>=PF_IMAGES){if(f->state!=PF_READY)printf("[iframe] passive document ready: images=%d runtime=%s scripts-pending=%d depth=1\n",f->images,f->page?"active":"inert",f->nscript-f->script);f->state=PF_READY;return 0;}
        f->images++;strcpy(f->image,image);char dest[PF_URL];
        if(resolve_url(f->base,image,dest,sizeof dest)&&iframe_policy_resource(f->url,dest,f->csp,IF_POLICY_IMAGE))f->id=request(f,dest,IF_POLICY_IMAGE);
        if(f->id<0){old=layout_context_activate(f->layout);layout_img_store(image,0,0);layout_context_activate(old);}
        f->state=PF_PIXELS;
    }
    return 0;
}
int passive_frames_update(struct node *root,unsigned long long mutation)
{
    int changed=0;if(!root)return 0;
    /* Message callbacks can mutate either document. Deliver the snapshot
     * BEFORE their layout passes, not after the last child's reflow: a
     * drained queue otherwise lets the guest sleep with stale pixels until
     * an unrelated mouse movement. Include parent removals/policy changes
     * made by these callbacks in this same ownership scan. */
    changed=frame_messages_pump();
    if(changed)mutation=js_dom_mutation_generation();
    if(root!=parent_root||mutation!=parent_mutation){
        parent_root=root;parent_mutation=mutation;
        /* Meta policies are additive over the lifetime of a document. Keep
         * prior restrictions when the author removes a meta element. */
        char meta[4096]={0};if(!meta_policy(root,meta,sizeof meta))parent_known=0;
        if(!add_new_policies(parent_csp,sizeof parent_csp,meta))parent_known=0;
        strcpy(parent_base,parent_url);
        for(struct node *n=root;n;n=next_node(n,root))if(tag(n,"base")&&dom_attr(n,"href")){
            char candidate[PF_URL];
            if(resolve_url(parent_url,dom_attr(n,"href"),candidate,sizeof candidate)&&
               iframe_policy_base(parent_url,candidate,parent_csp))strcpy(parent_base,candidate);
            break;
        }
        for(int i=0;i<PF_MAX;i++)frames[i].seen=0;
        for(struct node *n=root;n;n=next_node(n,root))if(tag(n,"iframe")){
            const char *src=dom_attr(n,"src");if(!src||!*src)continue;
            struct pframe *f=0;for(int i=0;i<PF_MAX;i++)if(frames[i].state&&frames[i].host==n&&frames[i].serial==n->serial){f=&frames[i];break;}
            if(f&&(!strcmp(f->src,src))&&f->state!=PF_FAILED&&
               (!parent_known||!iframe_policy_frame(parent_url,f->url,parent_csp,f->csp,f->xfo))){
                fail_frame(f,"updated parent policy");changed=1;
            }
            if(f&&!f->retire&&(!strcmp(f->src,src))&&!dom_attr(n,"sandbox")&&!dom_attr(n,"srcdoc")&&!dom_attr(n,"csp")&&!dom_attr(n,"credentialless")){f->seen=1;continue;}
            if(f){drop_content(f);memset(f,0,sizeof *f);f->id=-1;changed=1;}
            if(!f)for(int i=0;i<PF_MAX;i++)if(!frames[i].state){f=&frames[i];break;}
            if(f&&!start_frame(f,n,src)){fail_frame(f,"source, parent policy, or viewport");changed=1;}
        }
        for(int i=0;i<PF_MAX;i++)if(frames[i].state&&!frames[i].seen){drop_content(&frames[i]);memset(&frames[i],0,sizeof frames[i]);frames[i].id=-1;changed=1;}
    }
    if(passive_frames_pending())bfetch_pump();
    for(int i=0;i<PF_MAX;i++){struct pframe *f=&frames[i];if(f->state&&f->state!=PF_FAILED){
        if(f->state==PF_WAIT_GEOMETRY){int w,h;if(host_size(f->host,&w,&h)){struct node *host=f->host;char src[PF_URL];strcpy(src,f->src);if(!start_frame(f,host,src))fail_frame(f,"source after geometry became available");changed=1;}continue;}
        int w,h;if(host_size(f->host,&w,&h)&&(w!=f->width||h!=f->height)){f->width=w;f->height=h;f->dirty=1;changed=1;}
        changed|=advance_frame(f);
        if(f->page&&f->state>=PF_PIXELS&&f->state<=PF_READY)changed|=frame_runtime_pump(f);
        frame_focus_commit_parent(f);
    }}
    return changed;
}
