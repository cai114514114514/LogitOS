/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "memory.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>

#define MEMORY_WORKFLOW 0x4d454d31u
#define MEMORY_ID UINT64_C(0x4d454d4f525931)
static const char memory_kind[]="LogitOS domain memory v1";

static int valid_id(const char *id)
{
    if(!id||!*id)return 0;
    size_t n=0;
    for(;id[n];n++) {
        unsigned c=(unsigned char)id[n];
        if(n>=AEX_AGENT_ID_MAX-1||!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
            (c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return 0;
    }
    return 1;
}
static int checkpoint_presence(const char *directory,int *present)
{
    if(!directory||directory[0]!='/')return AG_E_ARGUMENT;
    size_t n=strlen(directory);if(n>AG_PATH-7)return AG_E_LIMIT;
    struct stat st;
    if(stat(directory,&st)<0||!S_ISDIR(st.st_mode))return AG_E_IO;
    *present=0;
    for(unsigned i=0;i<2;i++) {
        char path[AG_PATH];snprintf(path,sizeof path,"%s/slot%u",directory,i);
        if(lstat(path,&st)==0) {
            /* Private checkpoints must be regular files. In particular a
             * FIFO cannot turn a bounded memory load into an indefinite read. */
            if(!S_ISREG(st.st_mode))return AG_E_IO;
            *present=1;
        } else if(errno!=ENOENT)return AG_E_IO;
    }
    return 0;
}
static int memory_state(const struct ag_task *t,const struct ag_document *d,const char *id)
{
    if(strcmp(t->app_id,id))return AG_E_SCOPE;
    if(t->version!=AG_MEMORY_VERSION||t->workflow!=MEMORY_WORKFLOW||
       t->id!=MEMORY_ID||strcmp(t->goal,memory_kind))return AG_E_VERSION;
    if(d->length>AG_MEMORY_MAX)return AG_E_LIMIT;
    /* A memory checkpoint shares the receipt/document commit machinery, not
     * task execution state. Refuse a task or model cache dressed as memory. */
    if(t->phase!=AG_DONE||t->object_count||t->active||t->calls||t->stage||
       t->pending_operation||t->model_operation||t->model_bytes||t->notes_bytes||
       t->revision!=(uint64_t)t->receipt_count+1)return AG_E_IO;
    for(unsigned i=0;i<t->receipt_count;i++) {
        const struct ag_receipt *receipt=&t->receipts[i];
        if(!receipt->operation||receipt->operation==UINT64_MAX||
           receipt->revision!=(uint64_t)i+2||receipt->bytes>AG_MEMORY_MAX)return AG_E_IO;
        for(unsigned j=0;j<i;j++)if(receipt->operation==t->receipts[j].operation)return AG_E_IO;
    }
    return 0;
}
static int current(const char *directory,const char *id,struct ag_task *task,
                    struct ag_document *document)
{
    if(!valid_id(id))return AG_E_ARGUMENT;
    int present=0,result=checkpoint_presence(directory,&present);if(result)return result;
    result=ag_store_load(directory,task,document);
    if(result==AG_E_NOTFOUND) {
        /* ag_store's open failure is NOTFOUND, even for an unreadable file.
         * Only truly absent slots represent initial empty memory. */
        if(present)return AG_E_IO;
        ag_task_init(task,MEMORY_ID,memory_kind,"/memory");
        task->workflow=MEMORY_WORKFLOW;task->phase=AG_DONE;
        memcpy(task->app_id,id,strlen(id)+1);
        return ag_document_set(document,"",0,1);
    }
    if(result)return result;
    return memory_state(task,document,id);
}
int ag_memory_load(const char *directory,const char *id,struct ag_document *out)
{
    if(!out)return AG_E_ARGUMENT;
    struct ag_task task;struct ag_document document={0};
    int result=current(directory,id,&task,&document);
    if(!result){ag_document_free(out);*out=document;}
    else ag_document_free(&document);
    return result;
}
int ag_memory_commit(const char *directory,const char *id,uint64_t operation,
                      uint64_t expected_revision,const char *utf8,uint32_t length,
                      uint64_t *revision)
{
    if(revision)*revision=0;
    if(!revision||!operation||!expected_revision||(length&&!utf8))return AG_E_ARGUMENT;
    if(length>AG_MEMORY_MAX)return AG_E_LIMIT;
    if(!ag_utf8(utf8,length))return AG_E_ARGUMENT;
    struct ag_task task;struct ag_document document={0};
    /* Every explicit write reloads receipts and revision. A long-lived caller
     * cannot overwrite a newer broker commit using its stale local snapshot. */
    int result=current(directory,id,&task,&document);if(result)goto done;
    task.phase=AG_RUNNING;
    result=ag_task_commit(&task,&document,operation,expected_revision,utf8,length);
    if(result==0) {
        task.phase=AG_DONE;
        result=ag_store_save(directory,&task,&document);
    }
    if(result>=0)*revision=task.revision;
done:
    ag_document_free(&document);return result;
}
