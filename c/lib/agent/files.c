/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "files.h"
#include "../../apps/logit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int ag_file_same(const struct logit_file_id *a,const struct logit_file_id *b)
{return a->object&&a->object==b->object&&a->volume[0]==b->volume[0]&&a->volume[1]==b->volume[1];}
int ag_file_ref(const char *p,struct logit_file_id *id,uint64_t *rev)
{
    struct logit_stat st={0};memset(id,0,sizeof *id);
    if(_sys(SYS_LSTAT,(long)p,(long)&st,sizeof st)<0)return AG_E_NOTFOUND;
    if(st.len<sizeof st||!(st.attr&LSTA_ID)||!st.object_id)return AG_E_VERSION;
    if((st.mode&LST_IFMT)!=LST_IFDIR&&(st.mode&LST_IFMT)!=LST_IFREG)return AG_E_ARGUMENT;
    id->volume[0]=st.volume[0];id->volume[1]=st.volume[1];id->object=st.object_id;
    if(rev)*rev=st.revision;return 0;
}
int ag_file_path(const struct logit_file_id *id,char *p,unsigned cap)
{
    struct logit_fsref q={.version=LOGIT_FSREF_VERSION,.id=*id};
    int rc=(int)_sys(SYS_FSREF,(long)&q,sizeof q,0);if(rc<0)return AG_E_NOTFOUND;
    size_t n=strnlen(q.path,sizeof q.path);if(n==sizeof q.path||n+1>cap)return AG_E_LIMIT;
    memcpy(p,q.path,n+1);return 0;
}
int ag_parent_path(const char *p,char *out,unsigned cap)
{
    if(!p||p[0]!='/')return AG_E_ARGUMENT;
    const char *last=strrchr(p,'/');size_t n=(size_t)(last-p);if(!n)n=1;
    if(n+1>cap)return AG_E_LIMIT;memcpy(out,p,n);out[n]=0;return 0;
}
static int directory(const char *dir,char *out)
{int n=snprintf(out,AG_PATH,"%s/bindings",dir);return n>0&&n<AG_PATH?0:AG_E_LIMIT;}
int ag_binding_save(const char *dir,uint64_t id,struct ag_binding *b)
{
    char name[AG_PATH],text[320];if(directory(dir,name)<0)return AG_E_LIMIT;
    if(mkdir(name,0700)<0){struct stat st;if(stat(name,&st)<0||!S_ISDIR(st.st_mode))return AG_E_IO;}
    if(!b->journal.id){
        /* A rejected first task can leave a binding slot behind. Preserve its
         * generation before retrying, so reboot cannot select an older slot. */
        struct ag_task previous;struct ag_document stored={0};int rc=ag_store_load(name,&previous,&stored);
        ag_document_free(&stored);
        if(!rc){if(previous.id!=id)return AG_E_CONFLICT;b->journal=previous;}
        else if(rc!=AG_E_NOTFOUND)return rc;
        else {ag_task_init(&b->journal,id,"Persistent file references",name);b->journal.phase=AG_DONE;}
    }
    unsigned off=0;
    for(unsigned i=0;i<AG_BIND_COUNT;i++){
        int n=snprintf(text+off,sizeof text-off,"%016llx %016llx %016llx %016llx\n",b->refs[i].volume[0],b->refs[i].volume[1],b->refs[i].object,(unsigned long long)b->revisions[i]);
        if(n<0||(unsigned)n>=sizeof text-off)return AG_E_LIMIT;off+=(unsigned)n;
    }
    struct ag_document d={text,off,b->journal.revision};
    b->journal.document_bytes=off;b->journal.document_checksum=ag_checksum(text,off);
    return ag_store_save(name,&b->journal,&d);
}
int ag_binding_load(const char *dir,struct ag_binding *b)
{
    char name[AG_PATH];if(directory(dir,name)<0)return AG_E_LIMIT;
    memset(b,0,sizeof *b);struct ag_document d={0};int rc=ag_store_load(name,&b->journal,&d);
    if(rc<0)return rc;
    /* Fixed-width ASCII lets bindings share the checked dual-slot serializer
     * without pretending arbitrary binary data is a UTF-8 document. */
    if(d.length!=AG_BIND_COUNT*68u)rc=AG_E_IO;
    for(unsigned i=0;!rc&&i<AG_BIND_COUNT;i++)for(unsigned j=0;j<4;j++){
        char *p=d.bytes+i*68+j*17;char expect=j==3?'\n':' ';
        for(unsigned k=0;k<16;k++)if(!((p[k]>='0'&&p[k]<='9')||(p[k]>='a'&&p[k]<='f')))rc=AG_E_IO;
        if(p[16]!=expect)rc=AG_E_IO;
        unsigned long long value=strtoull(p,0,16);
        if(j<2)b->refs[i].volume[j]=value;else if(j==2)b->refs[i].object=value;else b->revisions[i]=value;
    }
    ag_document_free(&d);return rc;
}
