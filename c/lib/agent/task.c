/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "task.h"
#include <stdlib.h>
#include <string.h>
#include "../../drivers/block/crc32.h"

uint32_t ag_checksum(const void *p, size_t n) { return crc32(p, n); }
int ag_manual_preserved(const char *base,const char *edited,const char *candidate)
{
    size_t a=strlen(base),b=strlen(edited),prefix=0,suffix=0;
    while(prefix<a&&prefix<b&&base[prefix]==edited[prefix])prefix++;
    while(suffix<a-prefix&&suffix<b-prefix&&base[a-1-suffix]==edited[b-1-suffix])suffix++;
    size_t changed=b-prefix-suffix;
    if(!changed){
        if(a==b)return 1;
        /* Deletions also deserve review if the candidate restores the exact
         * pre-edit document. More complex rewrites are compared in the UI. */
        return strcmp(base,candidate)!=0;
    }
    size_t n=strlen(candidate);
    if(changed>n)return 0;
    size_t *failure=malloc(changed*sizeof *failure);if(!failure)return 0;
    const char *needle=edited+prefix;failure[0]=0;
    for(size_t i=1,j=0;i<changed;i++){while(j&&needle[i]!=needle[j])j=failure[j-1];
        if(needle[i]==needle[j])j++;failure[i]=j;}
    int found=0;
    for(size_t i=0,j=0;i<n;i++){while(j&&candidate[i]!=needle[j])j=failure[j-1];
        if(candidate[i]==needle[j])j++;if(j==changed){found=1;break;}}
    free(failure);return found;
}
static int citation_number(const char **at,uint64_t *value)
{
    const char *p=*at;uint64_t n=0;if(*p<'0'||*p>'9')return 0;
    while(*p>='0'&&*p<='9'){unsigned d=(unsigned)(*p++-'0');if(n>(UINT64_MAX-d)/10)return 0;n=n*10+d;}
    *at=p;*value=n;return 1;
}
int ag_citations_valid(const struct ag_task *t,const char *text)
{
    unsigned count=0;const char *p=text;
    /* A model may group several spans from the same source in one bracket.
     * Validate every range independently; a well-formed group is not an
     * invented source, while one bad member must reject the whole candidate. */
    while((p=strstr(p,"[S"))){p+=2;uint64_t id,a,b;
        if(!citation_number(&p,&id)||*p++!=':')return 0;
        const struct ag_object *o=0;for(unsigned i=0;i<t->object_count;i++)if(t->objects[i].id==id)o=&t->objects[i];
        if(!o)return 0;
        for(;;){while(*p==' ')p++;
            if(!citation_number(&p,&a)||*p++!='-'||!citation_number(&p,&b)||a>=b||b>o->bytes)return 0;
            count++;while(*p==' ')p++;
            if(*p==']'){p++;break;}if(*p++!=',')return 0;
        }
    }
    if(count)return 1;for(unsigned i=0;i<t->object_count;i++)if(t->objects[i].bytes)return 0;return 1;
}
static void copy(char *d, size_t cap, const char *s)
{ size_t n = strlen(s); if (n >= cap) n = cap-1; memcpy(d,s,n); d[n]=0; }
const char *ag_phase_name(unsigned p)
{
    static const char *names[]={"invalid","queued","running","waiting for model",
        "paused","cancelled","done","document conflict","reconciliation required","budget exhausted"};
    return p < sizeof names/sizeof *names ? names[p] : "invalid";
}
void ag_task_init(struct ag_task *t, uint64_t id, const char *goal, const char *out)
{
    memset(t,0,sizeof *t); t->id=id; t->version=AG_TASK_VERSION;
    t->phase=AG_QUEUED; t->next_operation=1; t->revision=1;
    t->finder_state=t->editor_state=1; t->call_limit=AG_CALLS_DEFAULT;
    t->workflow=1;
    copy(t->goal,sizeof t->goal,goal); copy(t->output,sizeof t->output,out);
}
int ag_task_valid(const struct ag_task *t)
{
    if (t->version != AG_TASK_VERSION || t->finder_state != 1 || t->editor_state != 1)
        return AG_E_VERSION;
    if (!t->id || !t->revision || !t->next_operation || !t->phase || t->phase>AG_WAIT_BUDGET ||
        t->active>AG_ACTIVE_MAX || t->object_count>AG_OBJECTS || t->receipt_count>AG_RECEIPTS ||
        t->document_bytes>AEX_AGENT_DOCUMENT_MAX || !memchr(t->goal,0,sizeof t->goal) ||
        !t->workflow || t->stage>2 || !memchr(t->app_id,0,sizeof t->app_id) ||
        !memchr(t->artifact,0,sizeof t->artifact) || !memchr(t->output,0,sizeof t->output) || !memchr(t->reason,0,sizeof t->reason)) return AG_E_ARGUMENT;
    for(unsigned i=0;i<t->object_count;i++) {
        const struct ag_object *o=&t->objects[i];
        if(!o->id || !memchr(o->path,0,sizeof o->path) || o->path[0]!='/' ||
           !o->rights || (o->rights&~3u)) return AG_E_ARGUMENT;
        for(unsigned j=0;j<i;j++) if(t->objects[j].id==o->id) return AG_E_ARGUMENT;
    }
    return 0;
}
int ag_task_control(struct ag_task *t,unsigned phase,unsigned extend)
{
    if(t->phase==AG_CANCELLED) return AG_E_STATE;
    if(phase!=AG_PAUSED && phase!=AG_CANCELLED && phase!=AG_QUEUED) return AG_E_ARGUMENT;
    if(phase==AG_QUEUED && (t->phase==AG_RECONCILE || t->phase==AG_CONFLICT)) return AG_E_STATE;
    if(t->phase==AG_DONE&&phase!=AG_CANCELLED)return AG_E_STATE;
    if(extend>1024 || t->call_limit>0xffffffffu-extend) return AG_E_LIMIT;
    t->call_limit+=extend; t->phase=phase==AG_QUEUED&&t->stage==2?AG_DONE:phase; return 0;
}
int ag_task_enter(struct ag_task *t)
{
    if(t->phase!=AG_QUEUED && t->phase!=AG_RUNNING) return AG_E_STATE;
    if(t->active>=AG_ACTIVE_MAX) return AG_E_LIMIT;
    ++t->active; t->phase=AG_RUNNING; return 0;
}
void ag_task_leave(struct ag_task *t) { if(t->active) --t->active; }
int ag_task_call(struct ag_task *t)
{
    if(t->phase!=AG_RUNNING) return AG_E_STATE;
    if(t->calls>=t->call_limit) {t->phase=AG_WAIT_BUDGET; return AG_E_BUDGET;}
    ++t->calls; return 0;
}
int ag_task_grant(struct ag_task *t,const struct ag_object *o)
{
    if(t->object_count>=AG_OBJECTS || !o->id || !o->rights || (o->rights&~3u) ||
       !memchr(o->path,0,sizeof o->path) || o->path[0]!='/') return AG_E_ARGUMENT;
    for(unsigned i=0;i<t->object_count;i++) if(t->objects[i].id==o->id) return AG_E_CONFLICT;
    t->objects[t->object_count++]=*o; return 0;
}
int ag_task_authorize(const struct ag_task *t,unsigned role,uint64_t id,unsigned rights)
{
#ifdef AG_NEG_PERMISSION
    return 0;
#endif
    if((role!=AG_FINDER && role!=AG_EDITOR && role!=AG_SPECIALIST) || !rights || (rights&~3u)) return AG_E_SCOPE;
    if(role==AG_FINDER && (rights&AG_RIGHT_EDIT)) return AG_E_SCOPE;
    for(unsigned i=0;i<t->object_count;i++) if(t->objects[i].id==id)
        return (t->objects[i].rights&rights)==rights ? 0 : AG_E_SCOPE;
    return AG_E_SCOPE;
}
int ag_utf8(const char *s,uint32_t n)
{
    for(uint32_t i=0;i<n;) {
        unsigned c=(unsigned char)s[i++], k, cp;
        if(c<128) {if(!c) return 0; continue;}
        if(c>=0xc2 && c<=0xdf) {k=1;cp=c&31;}
        else if(c>=0xe0 && c<=0xef) {k=2;cp=c&15;}
        else if(c>=0xf0 && c<=0xf4) {k=3;cp=c&7;}
        else return 0;
        unsigned width=k;
        if(k>n-i) return 0;
        while(k--) {c=(unsigned char)s[i++];if((c&0xc0)!=0x80)return 0;cp=(cp<<6)|(c&63);}
        if((width==1&&cp<128)||(width==2&&cp<2048)||(width==3&&cp<65536)||
           cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return 0;
    }
    return 1;
}
int ag_document_set(struct ag_document *d,const char *s,uint32_t n,uint64_t revision)
{
    if(n>AEX_AGENT_DOCUMENT_MAX) return AG_E_LIMIT;
    if((n&&!s)||!revision||!ag_utf8(s,n)) return AG_E_ARGUMENT;
    char *p=malloc((size_t)n+1); if(!p) return AG_E_IO;
    if(n)memcpy(p,s,n);p[n]=0; free(d->bytes);d->bytes=p;d->length=n;d->revision=revision;return 0;
}
void ag_document_free(struct ag_document *d) {free(d->bytes);memset(d,0,sizeof *d);}
int ag_task_commit(struct ag_task *t,struct ag_document *d,uint64_t op,uint64_t rev,
                   const char *s,uint32_t n)
{
    if(!op || n>AEX_AGENT_DOCUMENT_MAX || (n&&!s)) return AG_E_ARGUMENT;
    uint32_t sum=ag_checksum(s,n);
    /* Receipts bind the payload as well as its id. Reusing an operation id
     * with different content is an error, never a successful deduplication. */
#ifndef AG_NEG_DEDUP
    for(unsigned i=0;i<t->receipt_count;i++) if(t->receipts[i].operation==op)
        return t->receipts[i].bytes==n && t->receipts[i].checksum==sum ? 1 : AG_E_CONFLICT;
#endif
    if(t->phase!=AG_RUNNING) return AG_E_STATE;
#ifndef AG_NEG_REVISION
    if(rev!=t->revision || d->revision!=rev) return AG_E_CONFLICT;
#endif
    if(t->receipt_count==AG_RECEIPTS || rev==UINT64_MAX || op==UINT64_MAX) return AG_E_LIMIT;
    int r=ag_document_set(d,s,n,rev+1);if(r<0)return r;
    t->revision=rev+1; t->document_bytes=n;t->document_checksum=sum;
    t->receipts[t->receipt_count++]=(struct ag_receipt){op,rev+1,sum,n};
    if(op>=t->next_operation)t->next_operation=op+1;
    return 0;
}
