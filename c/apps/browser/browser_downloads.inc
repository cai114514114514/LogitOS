/* SPDX-License-Identifier: MIT */
/* Resolve the response BEFORE retiring the old DOM/JS realm. The previous
 * "stay on the page" branch ran after js_page_close/dom_free, leaving a dead
 * page behind after every download. A normal document is staged once and
 * handed to the existing loader; an attachment never enters its teardown. */
extern const char *bfetch_response_header(int,const char *) LOGIT_WEAK;
LOGIT_WEAK_STUB(bfetch_response_header);
static unsigned char *download_document;
static int download_document_len,download_document_status;
static char download_document_csp[TAB_DOCUMENT_CSP_MAX];
static int download_document_policy_known;
extern int bfetch_response_policy_known(int) LOGIT_WEAK;
LOGIT_WEAK_STUB(bfetch_response_policy_known);
static char download_document_url[600];
static char download_hint[128];
static int download_forced;
extern void js_download_set_handler(int (*)(const char *,const char *,const unsigned char *,int)) LOGIT_WEAK;
LOGIT_WEAK_STUB(js_download_set_handler);
struct download_job {char url[600],name[128],initiator[600];unsigned char *body;int len;};
static struct download_job download_jobs[4];
static int download_jobs_count;
static int download_queue(const char *target,const char *name,const unsigned char *body,int len)
{
    if(download_jobs_count==4||strlen(target)>=600||strlen(name)>=128||len>64*1024*1024)return -1;
    struct download_job *j=&download_jobs[download_jobs_count];memset(j,0,sizeof *j);
    memcpy(j->url,target,strlen(target)+1);memcpy(j->name,name,strlen(name)+1);
    const char *origin=committed_page_url();size_t n=strlen(origin);if(n>=sizeof j->initiator)return -1;
    memcpy(j->initiator,origin,n+1);j->len=len;
    if(len>=0){j->body=malloc((size_t)len+1);if(!j->body)return -1;if(len)memcpy(j->body,body,(size_t)len);}
    download_jobs_count++;return 0;
}
static int download_same_origin(const char *a,const char *b)
{
    struct url x,y;if(url_parse(a,&x)||url_parse(b,&y))return 0;
    if(x.port!=y.port||x.https!=y.https)return 0;
    int i=0;for(;x.host[i]&&y.host[i];i++)if(dl_lower(x.host[i])!=dl_lower(y.host[i]))return 0;
    return x.host[i]==y.host[i];
}
static void download_show_record(int id)
{
    const struct download *d=download_at(id);char st[96];int p=0;
    const char *prefix=d&&d->ok?"Downloaded: ":"Download failed: ";
    while(*prefix)st[p++]=*prefix++;
    for(const char *s=d?d->path:DOWNLOAD_DIR;*s&&p<(int)sizeof st-1;s++)st[p++]=*s;
    st[p]=0;set_status(st);
    printf("[download] %s bytes=%d\n",st,d?d->len:-1);
}
/* 0: continue with staged document; 1: download or failed fetch, preserve page. */
static int download_stage_navigation(const char *u,const char *initiator)
{
    int forced=download_forced;char hint[sizeof download_hint];
    memcpy(hint,download_hint,sizeof hint);download_forced=0;download_hint[0]=0;
    free(download_document);download_document=NULL;
    g_prog_what="fetching";g_prog_total=0;g_prog_done=0;g_prog_last=0;
    bfetch_reset_stats();
    int id=bfetch_start_nav_from(u,initiator);
    if(id<0){set_status("load failed: invalid URL");return 1;}
    bfetch_wait(id,load_tick);
    if(bfetch_state(id)!=BF_DONE){
        printf("[browser] page fetch failed: %s (%s)\n",bfetch_error(id),bfetch_url(id));
        char st[96];int p=0;const char *pre="load failed: ";while(*pre)st[p++]=*pre++;
        const char *why=bfetch_error(id);while(*why&&p<95)st[p++]=*why++;st[p]=0;set_status(st);
        bfetch_release(id);return 1;
    }
    const char *final=bfetch_url(id),*cd=NULL,*type=NULL;
    if(LOGIT_HAVE(bfetch_response_header)){cd=bfetch_response_header(id,"content-disposition");type=bfetch_response_header(id,"content-type");}
    /* A cross-origin redirect cannot inherit a page's filename suggestion.
     * Its own attachment header may still request an ordinary download. */
    if(forced&&initiator&&!download_same_origin(initiator,final)){forced=0;hint[0]=0;}
    int status=bfetch_status(id);
    if(download_should_save(final,cd,type,forced)){
        int len=0;const unsigned char *body=bfetch_body(id,&len);
        if(status>=200&&status<300&&status!=206&&len>=0)
            download_show_record(download_record_as(final,cd,hint,body,len));
        else set_status("Download failed: incomplete or unsuccessful HTTP response");
        bfetch_release(id);
        const char *back=committed_page_url();if(back&&*back){
            int i=0;while(back[i]&&i<599){url[i]=back[i];i++;}url[i]=0;ulen=i;addr_sync();
        }
        return 1;
    }
    size_t n=strlen(final);if(n>=sizeof download_document_url)n=sizeof download_document_url-1;
    memcpy(download_document_url,final,n);download_document_url[n]=0;
    download_document_csp[0]=0;
    download_document_policy_known=LOGIT_HAVE(bfetch_response_policy_known)&&bfetch_response_policy_known(id);
    const char *csp=LOGIT_HAVE(bfetch_response_header)?bfetch_response_header(id,"content-security-policy"):0;
    if(csp&&strlen(csp)<sizeof download_document_csp)strcpy(download_document_csp,csp);
    else if(csp)download_document_policy_known=0;
    download_document_status=status;download_document_len=bfetch_take(id,&download_document);
    if(download_document_len<0){set_status("load failed: response unavailable");return 1;}
    if(download_document_len>BROWSER_BUFFERED_BODY_MAX){free(download_document);download_document=NULL;set_status("load failed: document exceeds size limit");return 1;}
    return 0;
}
static int download_pump_jobs(void)
{
    if(!download_jobs_count)return 0;
    struct download_job j=download_jobs[0];download_jobs_count--;
    memmove(download_jobs,download_jobs+1,(size_t)download_jobs_count*sizeof download_jobs[0]);
    if(j.len>=0){download_show_record(download_record_as(j.url,NULL,j.name,j.body,j.len));free(j.body);}
    else {
        download_forced=1;memcpy(download_hint,j.name,sizeof download_hint);
        if(!download_stage_navigation(j.url,j.initiator)){
            free(download_document);download_document=NULL;
            set_status("Download refused: redirected response is a page");
        }
    }
    return 1;
}
static int download_activate_anchor(struct node *node)
{
    while(node&&!(node->type==N_ELEM&&(tag_is(node->tag,"a")||tag_is(node->tag,"area"))))node=node->parent;
    if(!node||!dom_attr(node,"download"))return 0;
    JSContext *ctx=js_page_ctx();
    if(ctx){
        JSValue global=JS_GetGlobalObject(ctx),fn=JS_GetPropertyStr(ctx,global,"__logitDownloadAnchor");
        if(JS_IsFunction(ctx,fn)){
            JSValue arg=js_dom_node_value(ctx,node),result=JS_Call(ctx,fn,global,1,&arg);
            int handled=!JS_IsException(result)&&JS_ToBool(ctx,result)>0;
            if(JS_IsException(result)){JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);set_status("Download could not be queued");handled=1;}
            JS_FreeValue(ctx,result);JS_FreeValue(ctx,arg);JS_FreeValue(ctx,fn);JS_FreeValue(ctx,global);return handled;
        }
        JS_FreeValue(ctx,fn);JS_FreeValue(ctx,global);
    }
    const char *href=dom_attr(node,"href");char abs[600];
    if(!href||bfetch_resolve(navigation_base(),href,abs,sizeof abs)||!download_same_origin(navigation_base(),abs))return 0;
    if(download_queue(abs,dom_attr(node,"download"),NULL,-1)<0)set_status("Download queue is full");
    return 1;
}
