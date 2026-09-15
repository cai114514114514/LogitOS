/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "review.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int directory(const char *dir,char *out)
{int n=snprintf(out,AG_PATH,"%s/review",dir);return n>0&&n<AG_PATH?0:AG_E_LIMIT;}
void ag_review_free(struct ag_review *r)
{ag_document_free(&r->candidate);memset(r,0,sizeof *r);}
int ag_review_load(const char *dir,struct ag_review *r)
{
    char p[AG_PATH];int rc=directory(dir,p);if(rc<0)return rc;
    struct ag_review loaded={0};rc=ag_store_load(p,&loaded.record,&loaded.candidate);
    if(rc<0)return rc;loaded.enabled=1;ag_review_free(r);*r=loaded;return 0;
}
static int save(const char *dir,struct ag_review *r,struct ag_task *next,const char *text,uint32_t n)
{
    char p[AG_PATH];int rc=directory(dir,p);if(rc<0)return rc;
    if(mkdir(p,0700)<0){struct stat st;if(stat(p,&st)<0||!S_ISDIR(st.st_mode))return AG_E_IO;}
    struct ag_document doc={0};rc=ag_document_set(&doc,text,n,next->revision);
    if(rc<0)return rc;next->document_bytes=n;next->document_checksum=ag_checksum(text,n);
    rc=ag_store_save(p,next,&doc);
    /* Install only after the complete snapshot has reached stable storage;
     * a failed write must leave the displayed proposal identity untouched. */
    if(!rc){ag_review_free(r);r->enabled=1;r->record=*next;r->candidate=doc;}
    else ag_document_free(&doc);return rc;
}
int ag_review_enable(const char *dir,const struct ag_task *t,struct ag_review *r,const char *binding)
{
    if(r->enabled)return 0;
    struct ag_task next;ag_task_init(&next,t->id,"TextEdit review journal",t->output);
    next.phase=AG_CONFLICT;next.workflow=t->workflow;next.revision=t->revision;
    if(binding){if(strlen(binding)>=sizeof next.artifact)return AG_E_LIMIT;strcpy(next.artifact,binding);}
    return save(dir,r,&next,"",0);
}
int ag_review_propose(const char *dir,const struct ag_task *t,struct ag_review *r,
                     uint64_t op,uint64_t rev,const char *text,uint32_t bytes)
{
    if(!r->enabled||!bytes||!op||!rev)return AG_E_ARGUMENT;
    struct ag_task next=r->record;next.workflow=t->workflow;next.revision=rev;
    next.next_operation=op;return save(dir,r,&next,text,bytes);
}
int ag_review_pending(const struct ag_task *t,const struct ag_review *r)
{
    if(!r->enabled||!r->candidate.length||r->record.id!=t->id||r->record.workflow!=t->workflow)return 0;
    for(unsigned i=0;i<t->receipt_count;i++)if(t->receipts[i].operation==r->record.next_operation)return 0;
    return 1;
}
int ag_review_validate(const struct ag_task *t,const struct ag_review *r,uint64_t rev,uint64_t proposal)
{
    if(t->phase!=AG_CONFLICT||!ag_review_pending(t,r))return AG_E_STATE;
#ifndef AG_NEG_REVIEW_VERSION
    if(rev!=t->revision||proposal!=r->record.generation)return AG_E_CONFLICT;
#endif
    return 0;
}
