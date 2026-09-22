/* SPDX-License-Identifier: MIT
 * The initial loader USED TO be the only consumer of link stylesheets.
 * A script-created link neither fetched nor fired load/error. The SMS widget
 * specimen fetched both JS bundles, then waited forever with empty innerHTML
 * after inserting two CSS links. The ordinary dynamic-styles fixture exercises
 * that continuation without a login, SDK or hostname in the implementation.
 *
 * Keep sheet bytes by DOM occurrence, not appended to an immutable CSS tail:
 * <link><style> order, href replacement and removal must affect the cascade.
 * Requests survive frame boundaries; events run only on the outer frame, after
 * application. CSSOM's synchronous geometry flush never does network IO or
 * invokes these callbacks. Imported-sheet expansion still uses the existing
 * bounded synchronous import loader; this does not add parallel @import or
 * expose a fabricated link.sheet CSSOM object.
 */
struct live_sheet {
    struct live_sheet *next;
    struct node *node;             /* compare only; never dereference a saved node */
    uint32_t serial;
    char *href, *base, *raw, *expanded;
    int len, exlen, excap, fd, state, event, seen, apply_failed;
};
static struct live_sheet *g_sheets_live;
static unsigned long long g_sheet_generation = (unsigned long long)-1;
static int g_sheet_rebuild, g_sheet_count;
static uint64_t g_sheet_signature, g_sheet_scan_signature;
#define SHEET_SIGNATURE_INIT UINT64_C(14695981039346656037)
static int g_sheet_rebuild_count;
int browser_stylesheet_rebuilds(void) { return g_sheet_rebuild_count; }
static unsigned g_restyle_calls;
unsigned browser_dom_restyles(void) { return g_restyle_calls; }
/* One author buffer was 4 MiB; its current ceiling is BROWSER_CSS_BYTES.
 * Keep at most twice that in per-link data, plus
 * one temporary expansion buffer; 512 small links must not permit 512 huge
 * retained expansions to consume the process arena. Refusal fires error. */
#define LIVE_SHEET_BYTES (2u * BROWSER_CSS_BYTES)
static size_t g_sheet_bytes;
static int sheet_reserve(size_t bytes)
{
    if(bytes>LIVE_SHEET_BYTES-g_sheet_bytes){printf("[stylesheet] retained CSS exceeds %u bytes\n",(unsigned)LIVE_SHEET_BYTES);return 0;}
    return 1;
}

static char *sheet_copy(const char *s, int len)
{
    char *p=malloc((size_t)len+1);
    if(p){if(len)memcpy(p,s,(size_t)len);p[len]=0;}return p;
}
static void sheet_free(struct live_sheet *s)
{
    if(s->fd>=0)bfetch_release(s->fd);
    if(s->raw)g_sheet_bytes-=(size_t)s->len+1;
    if(s->expanded)g_sheet_bytes-=(size_t)s->excap;
    free(s->href);free(s->base);free(s->raw);free(s->expanded);free(s);
    g_sheet_count--;
}
static void stylesheet_reset(void)
{
    while(g_sheets_live){struct live_sheet *s=g_sheets_live;g_sheets_live=s->next;sheet_free(s);}
    g_sheet_generation=(unsigned long long)-1;g_sheet_rebuild=0;g_sheet_signature=0;
    g_sheet_rebuild_count=0;
}
static struct live_sheet *sheet_find(struct node *n)
{
    const char *href=dom_attr(n,"href");
    if(!href)return 0;
    for(struct live_sheet *s=g_sheets_live;s;s=s->next)
        if(s->node==n && s->serial==n->serial && str_eq(s->href,href))return s;
    return 0;
}
static struct live_sheet *sheet_new(struct node *n,const char *base)
{
    const char *href=dom_attr(n,"href");
    if(!href || !href[0])return 0;
    if(g_sheet_count>=512){printf("[stylesheet] document link limit 512 reached\n");return 0;}
    struct live_sheet *s=calloc(1,sizeof *s);
    if(!s)return 0;
    s->fd=-1;s->href=sheet_copy(href,(int)strlen(href));
    s->base=sheet_copy(base,(int)strlen(base));
    if(!s->href||!s->base){free(s->href);free(s->base);free(s);return 0;}
    s->node=n;s->serial=n->serial;s->next=g_sheets_live;g_sheets_live=s;g_sheet_count++;
    return s;
}
static void stylesheet_remember(struct node *n,const struct resent *e)
{
#ifndef BROWSER_NO_DYNAMIC_STYLESHEETS
    if(sheet_find(n))return;
    struct live_sheet *s=sheet_new(n,e->url?e->url:e->ref);
    if(!s)return;
    s->len=e->len;
    if(e->len>=0 && sheet_reserve((size_t)e->len+1))s->raw=sheet_copy((const char *)e->data,e->len);
    if(s->raw)g_sheet_bytes+=(size_t)s->len+1;
    s->state=s->raw?2:3; /* parser resource already consumed, no duplicate event */
    /* The old tab model kept only concatenated CSS, since parser links were
     * never revisited. A live per-link rebuild needs these original bytes too;
     * otherwise switching tabs silently redials every stylesheet origin. */
    if(s->raw&&e->url)tab_keep_res(tab_cur(),e->url,(unsigned char *)s->raw,s->len);
#else
    (void)n;(void)e;
#endif
}
static int stylesheet_emit(struct node *n,char **out,int used,int *cap,
                           struct css_import_budget *budget)
{
    struct live_sheet *s=sheet_find(n);
    if(!s||s->state!=2)return used;
    s->apply_failed=0;
    if(!s->expanded){
        struct css_import_budget one={0};
        s->exlen=browser_css_emit(s->raw,s->len,s->base,0,&s->expanded,0,&s->excap,&one);
        /* Retain expansion, so unrelated DOM mutations don't refetch imports.
         * A failed dependency is not advertised to a waiting loader as success. */
        if(one.failed||one.limited){free(s->expanded);s->expanded=0;s->state=3;return used;}
        char *small=realloc(s->expanded,(size_t)s->exlen+1);
        if(small)s->expanded=small;
        s->excap=small?s->exlen+1:s->excap;
        free(s->raw);s->raw=0;g_sheet_bytes-=(size_t)s->len+1;
        if(!sheet_reserve((size_t)s->excap)){free(s->expanded);s->expanded=0;s->excap=0;s->state=3;return used;}
        g_sheet_bytes+=(size_t)s->excap;
    }
    int before=used,wrap=media_needs_wrap(dom_attr(n,"media"));
    int extra=wrap?(int)strlen(dom_attr(n,"media"))+13:0;
    if(s->exlen>BROWSER_CSS_BYTES-used-extra-1||
       !css_text_reserve(out,cap,used+s->exlen+extra+1,BROWSER_CSS_BYTES)){
        /* Keep the decoded sheet: removing an earlier occurrence can free
         * room on the next rebuild. But its pending event must be error, not
         * a fabricated successful application of bytes we just omitted. */
        s->apply_failed=1;
        budget->limited++;printf("[stylesheet] whole occurrence refused: required=%d allowed=%d\n",s->exlen+extra,BROWSER_CSS_BYTES-used-1);return before;}
    if(wrap)used=append_media_open(*out,used,*cap,dom_attr(n,"media"));
    memcpy(*out+used,s->expanded,(size_t)s->exlen);used+=s->exlen;
    if(wrap)used=append_media_close(*out,used,*cap);(*out)[used]=0;
    return used;
}
static void sheet_hash(const char *text)
{
    if(text)while(*text){g_sheet_scan_signature^=(unsigned char)*text++;g_sheet_scan_signature*=1099511628211ULL;}
    g_sheet_scan_signature^=255;g_sheet_scan_signature*=1099511628211ULL;
}
static void sheet_scan(struct node *n)
{
    if(!n)return;
    if(n->type==N_ELEM && tag_is(n->tag,"link") && link_sheet_active(n)){
        struct live_sheet *s=sheet_find(n);
        if(!s)s=sheet_new(n,committed_page_url());
        if(s)s->seen=1;
        sheet_hash("link");sheet_hash(dom_attr(n,"href"));sheet_hash(dom_attr(n,"media"));
    }else if(n->type==N_ELEM && tag_is(n->tag,"style")){
        sheet_hash("style");sheet_hash(dom_attr(n,"media"));
        for(struct node *c=n->first_child;c;c=c->next)if(c->type==N_TEXT)sheet_hash(c->text);
    }
    for(struct node *c=n->first_child;c;c=c->next)sheet_scan(c);
}
static void stylesheet_commit_initial(void)
{
#ifndef BROWSER_REBUILD_INITIAL_SHEETS
    /* The loader just assembled/applied these DOM occurrences. Starting the
     * live signature at zero made its first frame re-expand, cascade and lay
     * out the identical sheet: 2.84s in the retained Wikipedia guest workload
     * (2026-09-11). Seed only the signature BEFORE scripts run; the first live
     * scan must still detect script edits and discover pending link events. */
    g_sheet_scan_signature=SHEET_SIGNATURE_INIT;
    sheet_scan(g_root);
    g_sheet_signature=g_sheet_scan_signature;
#endif
}
static int stylesheet_pending(void)
{
    for(struct live_sheet *s=g_sheets_live;s;s=s->next)if(s->state<2||s->event)return 1;
    return 0;
}
static struct node *sheet_live_node(struct node *n,struct live_sheet *s)
{
    if(!n)return 0;
    if(n==s->node && n->serial==s->serial && tag_is(n->tag,"link") &&
       link_sheet_active(n) && dom_attr(n,"href") && str_eq(dom_attr(n,"href"),s->href))return n;
    for(struct node *c=n->first_child;c;c=c->next){struct node *r=sheet_live_node(c,s);if(r)return r;}
    return 0;
}
static int stylesheet_frame(void)
{
#ifdef BROWSER_NO_DYNAMIC_STYLESHEETS
    return 0;
#else
    if(!g_root||!js_page_ctx())return 0;
    unsigned long long generation=js_dom_mutation_generation();
    if(generation!=g_sheet_generation){
        g_sheet_generation=generation;
        for(struct live_sheet *s=g_sheets_live;s;s=s->next)s->seen=0;
        /* Generation is an O(1) idle guard. Hash only stylesheet producers on
         * mutation: an input value or animation tick must not rebuild 4 MiB
         * of author CSS. DOM order and media are part of the signature. */
        g_sheet_scan_signature=SHEET_SIGNATURE_INIT;
        sheet_scan(g_root);
        struct live_sheet **at=&g_sheets_live;
        while(*at){struct live_sheet *s=*at;
            if(!s->seen){*at=s->next;sheet_free(s);}else at=&s->next;}
        if(g_sheet_signature!=g_sheet_scan_signature)g_sheet_rebuild=1;
        g_sheet_signature=g_sheet_scan_signature;
    }
    if(stylesheet_pending())bfetch_pump();
    for(struct live_sheet *s=g_sheets_live;s;s=s->next){
        if(s->state==0){
            char absolute[2048];
            if(bfetch_resolve(s->base,s->href,absolute,sizeof absolute)<0){s->state=3;s->event=1;continue;}
            const struct tabres *saved=tab_res_find(tab_cur(),absolute);
            if(saved){s->len=saved->len;
                if(sheet_reserve((size_t)saved->len+1))s->raw=sheet_copy((const char *)saved->data,saved->len);
                if(s->raw)g_sheet_bytes+=(size_t)s->len+1;
                char *base=sheet_copy(absolute,(int)strlen(absolute));if(base){free(s->base);s->base=base;}
                s->state=s->raw?2:3;s->event=1;g_sheet_rebuild=1;continue;}
            s->fd=bfetch_start_from(s->base,s->href);
            if(s->fd<0)break; /* slots are shared: retry after earlier IO retires */
            s->state=1;
        }
        if(s->state!=1||bfetch_state(s->fd)==BF_PENDING)continue;
        int fd=s->fd;s->fd=-1;
        if(bfetch_state(fd)==BF_DONE && bfetch_status(fd)/100==2){
            const char *final=bfetch_url(fd);char *base=final?sheet_copy(final,(int)strlen(final)):0;
            if(base){free(s->base);s->base=base;}
            s->len=bfetch_take(fd,(unsigned char **)&s->raw);
            s->state=s->len>=0?2:3;
            if(s->raw){
                if(s->len<0||!sheet_reserve((size_t)s->len+1)){free(s->raw);s->raw=0;s->state=3;}
                else g_sheet_bytes+=(size_t)s->len+1;
            }
            if(s->state==2)tab_keep_res(tab_cur(),s->base,(unsigned char *)s->raw,s->len);
        }else{printf("[stylesheet] fetch failed HTTP %d %s\n",bfetch_status(fd),s->href);
            bfetch_release(fd);s->state=3;}
        s->event=1;g_sheet_rebuild=1;
    }
    int changed=g_sheet_rebuild;g_sheet_rebuild=0;
    if(changed){
        g_sheet_rebuild_count++;
        struct css_import_budget budget={0};
        int len=browser_css_collect(g_root,committed_page_url(),&author_css,0,&author_cap,&budget);
        css_exlen=browser_expand_author(len);
        tab_keep_css(tab_cur(),author_css,len);
        if(LOGIT_HAVE(css_anim_snapshot))css_anim_snapshot(g_root);
        css_apply(g_root,css_expanded,css_exlen);css_extra_apply(g_root,css_expanded,css_exlen);
        layout_page(g_root,win_w);ph=layout_height();
        if(LOGIT_HAVE(js_cssom_reconcile_element_scroll))js_cssom_reconcile_element_scroll();
        g_page_width=browser_content_width(g_root,win_w);g_hbar=max_scroll_x()>0?12:0;
#ifndef BROWSER_RESTYLE_APPLIED_SHEET
        /* This complete cascade/layout consumed the existing DOM invalidation.
         * Leaving it dirty made settle_dom immediately do a second pass: the
         * initial-stylesheet gate observed two layouts for one author edit.
         * Clear BEFORE load/error callbacks below, whose new mutations must
         * survive and cause their own settle pass. Mirror restyle's animation
         * snapshot and scroll reconciliation before consuming that record. */
        js_dom_clear_dirty();
        (void)css_interaction_take_change();
#endif
    }
    /* Clear the event before dispatch. A handler can remove this or another
     * link, or replace href; re-find through the live tree for every dispatch.
     * We never inspect a freed/recycled node through the saved pointer. */
    for(struct live_sheet *s=g_sheets_live;s;s=s->next)if(s->event){
        s->event=0;struct node *n=sheet_live_node(g_root,s);
        if(n){struct js_event_init ev={0};int loaded=s->state==2;
#ifndef BROWSER_OMITTED_SHEET_LOAD
            loaded=loaded&&!s->apply_failed;
#endif
            js_dom_dispatch(n,loaded?"load":"error",&ev);}
    }
    return changed;
#endif
}
