/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define AG_STORE_MAGIC 0x33545341u
struct header {uint32_t magic,version,state_bytes,doc_bytes;uint64_t generation;
    uint32_t state_crc,doc_crc,header_crc,reserved;};
static int exact(int fd,void *buf,size_t n,int writing)
{
    char *p=buf;
    while(n) {long r=writing?write(fd,p,n):read(fd,p,n);if(r<=0)return AG_E_IO;p+=r;n-=(size_t)r;}
    return 0;
}
static int slot_name(char *p,size_t cap,const char *dir,unsigned slot)
{int n=snprintf(p,cap,"%s/slot%u",dir,slot);return n>0&&(size_t)n<cap?0:AG_E_LIMIT;}
int ag_store_save(const char *dir,struct ag_task *t,const struct ag_document *d)
{
    if(ag_task_valid(t)<0 || t->generation==UINT64_MAX || t->revision!=d->revision ||
       t->document_bytes!=d->length || ag_checksum(d->bytes,d->length)!=t->document_checksum)
        return AG_E_ARGUMENT;
#ifdef AG_NEG_CHECKPOINT
    return 0;
#endif
    struct ag_task next=*t; next.generation++;
    struct header h={AG_STORE_MAGIC,AG_TASK_VERSION,sizeof next,d->length,next.generation,
        ag_checksum(&next,sizeof next),ag_checksum(d->bytes,d->length),0,0};
    h.header_crc=ag_checksum(&h,sizeof h);
    char path[AG_PATH];if(slot_name(path,sizeof path,dir,(unsigned)(next.generation&1))<0)return AG_E_LIMIT;
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return AG_E_IO;
    int r=exact(fd,&h,sizeof h,1);
    if(!r)r=exact(fd,&next,sizeof next,1);
    if(!r)r=exact(fd,d->bytes,d->length,1);
    if(!r && fsync(fd)<0)r=AG_E_IO;
    if(close(fd)<0)r=AG_E_IO;
    if(!r)t->generation=next.generation;
    return r;
}
static int load_slot(const char *dir,unsigned slot,struct ag_task *t,struct ag_document *d)
{
    char path[AG_PATH];if(slot_name(path,sizeof path,dir,slot)<0)return AG_E_LIMIT;
    int fd=open(path,O_RDONLY);if(fd<0)return AG_E_NOTFOUND;
    struct header h={0};int r=exact(fd,&h,sizeof h,0);uint32_t crc=h.header_crc;h.header_crc=0;
    if(r<0 || h.magic!=AG_STORE_MAGIC || ag_checksum(&h,sizeof h)!=crc || h.reserved ||
       h.doc_bytes>AEX_AGENT_DOCUMENT_MAX)r=AG_E_IO;
    else if(h.version!=AG_TASK_VERSION || h.state_bytes!=sizeof *t)r=AG_E_VERSION;
    if(!r)r=exact(fd,t,sizeof *t,0);
    if(!r && (ag_checksum(t,sizeof *t)!=h.state_crc || t->generation!=h.generation))r=AG_E_IO;
    if(!r)r=ag_task_valid(t);
    char *buf=0;
    if(!r){buf=malloc((size_t)h.doc_bytes+1);if(!buf)r=AG_E_IO;}
    if(!r)r=exact(fd,buf,h.doc_bytes,0);
    if(!r){char extra;if(read(fd,&extra,1)!=0)r=AG_E_IO;}
    if(!r && (ag_checksum(buf,h.doc_bytes)!=h.doc_crc || t->document_bytes!=h.doc_bytes ||
       t->document_checksum!=h.doc_crc || !ag_utf8(buf,h.doc_bytes)))r=AG_E_IO;
    if(close(fd)<0)r=AG_E_IO;
    if(!r){buf[h.doc_bytes]=0;d->bytes=buf;d->length=h.doc_bytes;d->revision=t->revision;}
    else free(buf);
    return r;
}
int ag_store_load(const char *dir,struct ag_task *t,struct ag_document *d)
{
    struct ag_task a,b;struct ag_document da={0},db={0};
    int ra=load_slot(dir,0,&a,&da),rb=load_slot(dir,1,&b,&db);
    if(ra==AG_E_VERSION || rb==AG_E_VERSION){ag_document_free(&da);ag_document_free(&db);return AG_E_VERSION;}
    if(ra<0&&rb<0)return ra==AG_E_NOTFOUND&&rb==AG_E_NOTFOUND?AG_E_NOTFOUND:AG_E_IO;
    if(ra==0&&(rb<0||a.generation>b.generation)){*t=a;*d=da;ag_document_free(&db);}
    else {*t=b;*d=db;ag_document_free(&da);}
    /* A PID/active count is not durable work. RUNNING means an interrupted
     * step; pending side effects need reconciliation before resubmission. */
    t->active=0;
    if(t->phase==AG_RUNNING)t->phase=t->pending_operation?AG_RECONCILE:AG_QUEUED;
    return 0;
}
