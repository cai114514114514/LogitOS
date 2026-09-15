/* SPDX-License-Identifier: MIT */
#include "../../lib/agent/sdk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
static int call(struct ag_message *m,const void *in,void **out)
{int r=ag_call(m,in,out);if(r<0)fprintf(stderr,"Task service refused operation (%d).\n",r);return r;}
int main(int argc,char **argv)
{
    if(argc<2){puts("agentctl status | document/verify ID | pause/resume/cancel/budget ID | revise ID GOAL | create GOAL OUTPUT SOURCE...");return 2;}
    if(!strcmp(argv[1],"ref")&&argc==3){
        struct logit_file_id id;uint64_t revision;char path[AG_PATH];int rc=ag_file_ref(argv[2],&id,&revision);
        if(!rc)rc=ag_file_path(&id,path,sizeof path);
        if(rc)return 1;
        printf("FILE_REF volume=%016llx%016llx object=%llu revision=%llu path=%s\n",id.volume[0],id.volume[1],id.object,(unsigned long long)revision,path);return 0;
    }
    if(!strcmp(argv[1],"project")&&argc==3){
        struct ag_message m={.type=AG_PROJECT_VIEW,.bytes=(unsigned)strlen(argv[2])};void *p=0;int rc=call(&m,argv[2],&p);
        if(!rc&&m.bytes==sizeof(struct ag_project_view)){struct ag_project_view *v=p;
            printf("PROJECT object=%llu tasks=%u path=%s\n",v->id.object,v->count,argv[2]);
            for(unsigned i=0;i<v->count;i++)printf("PROJECT_TASK id=%llu artifact=%s\n",(unsigned long long)v->tasks[i].id,v->tasks[i].artifact);
        }else rc=-1;free(p);return rc<0;
    }
    if(!strcmp(argv[1],"lookup")&&argc==3){
        struct ag_message m={.type=AG_DOCUMENT,.bytes=(unsigned)strlen(argv[2])};void *p=0;int rc=call(&m,argv[2],&p);
        if(!rc)printf("DOCUMENT_TASK task=%llu revision=%llu bytes=%u\n",(unsigned long long)m.task,(unsigned long long)m.revision,m.bytes);
        free(p);return rc<0;
    }
    if(!strcmp(argv[1],"memory")){
        if(argc!=3&&argc!=5){puts("agentctl memory APPID [EXPECTED_REVISION TEXT]");return 2;}
        char *text=0;uint32_t bytes=0;uint64_t revision=0;int rc;
        if(argc==5){revision=strtoull(argv[3],0,10);rc=ag_memory_write(argv[2],argv[4],(uint32_t)strlen(argv[4]),&revision,revision);}
        else rc=ag_memory_read(argv[2],&text,&bytes,&revision);
        if(rc<0)printf("Memory operation refused (%d)\n",rc);
        else {printf("MEMORY app=%s revision=%llu bytes=%u\n",argv[2],(unsigned long long)revision,bytes);if(bytes)fwrite(text,1,bytes,stdout);}
        free(text);return rc<0;}
    if(!strcmp(argv[1],"create")){
        if(argc<5||argc>AG_OBJECTS+4)return 2;
        char *args[AG_OBJECTS+5];args[0]="/files.aex";args[1]="--task-from";
        for(int i=2;i<argc;i++)args[i]=argv[i];args[argc]=0;
        execv(args[0],args);return 1;
    }
    struct ag_message m={0};void *out=0;int r;
    if(!strcmp(argv[1],"status")){
        m.type=AG_STATUS;r=call(&m,0,&out);
        if(!r&&m.bytes==sizeof(struct ag_status)){struct ag_status *s=out;
            printf("SERVICE elapsed_ms=%llu idle_wakes=%llu\n",(unsigned long long)s->service_ms,(unsigned long long)s->idle_wakes);
            printf("SERVICE_COUNTERS cpu_ns=%llu poll_returns=%llu poll_errors=%llu\n",
                (unsigned long long)s->service_cpu_ns,(unsigned long long)s->poll_returns,(unsigned long long)s->poll_errors);
            for(unsigned i=0;i<s->count;i++){struct ag_task *t=&s->tasks[i];
                printf("TASK id=%llu phase=%s revision=%llu calls=%u active=%u generation=%llu objects=%u output=%s\nReason: %s\n",
                    (unsigned long long)t->id,ag_phase_name(t->phase),(unsigned long long)t->revision,t->calls,t->active,(unsigned long long)t->generation,t->object_count,t->artifact[0]?t->artifact:t->output,t->reason);
                printf("METRICS task=%llu sampled_peak_rss_bytes=%llu worker_cpu_ns=%llu recovery_ms=%llu\n",
                    (unsigned long long)t->id,(unsigned long long)t->peak_worker_rss_frames*4096,
                    (unsigned long long)t->worker_cpu_ns,(unsigned long long)t->recovery_ms);}}
    }else if(argc>=3){m.task=strtoull(argv[2],0,10);if(!m.task)return 2;
        if(!strcmp(argv[1],"verify")){
            uint64_t id=m.task;struct ag_message status={.type=AG_STATUS};void *data=0;
            r=call(&status,0,&data);char artifact[AG_PATH]={0};
            if(!r&&status.bytes==sizeof(struct ag_status)){
                struct ag_status *s=data;for(unsigned i=0;i<s->count;i++)if(s->tasks[i].id==id)memcpy(artifact,s->tasks[i].artifact,sizeof artifact);}
            free(data);if(r||!artifact[0])return 1;
            m.type=AG_DOCUMENT;r=call(&m,0,&out);if(r)return 1;
            int fd=open(artifact,O_RDONLY);size_t got=0;char buf[4096];int good=fd>=0;
            while(good&&got<m.bytes){size_t want=m.bytes-got;if(want>sizeof buf)want=sizeof buf;
                long n=read(fd,buf,want);if(n<=0||memcmp(buf,(char *)out+got,(size_t)n)){good=0;break;}got+=(size_t)n;}
            if(good&&read(fd,buf,1)!=0)good=0;if(fd>=0&&close(fd)<0)good=0;
            if(good)printf("DOCUMENT_VERIFIED task=%llu revision=%llu bytes=%u crc=%u artifact=%s\n",
                (unsigned long long)id,(unsigned long long)m.revision,m.bytes,ag_checksum(out,m.bytes),artifact);
            else puts("DOCUMENT_VERIFY_FAILED: artifact differs from current document (it may have unsaved edits)");r=good?0:-1;
        }
        else if(!strcmp(argv[1],"work")){
            m.type=AG_WORK_VIEW;m.object=argc==4&&!strcmp(argv[3],"review");r=call(&m,0,&out);
            if(!r&&m.bytes==sizeof(struct ag_work_view)){struct ag_work_view *v=out;
                printf("WORK task=%llu revision=%llu proposal=%llu base=%llu bytes=%u review=%u phase=%s\n",
                    (unsigned long long)v->task.id,(unsigned long long)v->task.revision,
                    (unsigned long long)v->proposal,(unsigned long long)v->base_revision,v->candidate_bytes,v->review_enabled,ag_phase_name(v->task.phase));}}
        else if(!strcmp(argv[1],"draft")){m.type=AG_DOCUMENT;m.object=2;r=call(&m,0,&out);if(!r&&m.bytes)fwrite(out,1,m.bytes,stdout);}
        else if(!strcmp(argv[1],"candidate")){m.type=AG_DOCUMENT;m.object=1;r=call(&m,0,&out);if(!r&&m.bytes)fwrite(out,1,m.bytes,stdout);}
        else if(!strcmp(argv[1],"decide")&&argc==6){
            m.type=AG_WORK_DECIDE;m.revision=strtoull(argv[3],0,10);m.operation=strtoull(argv[4],0,10);
            if(strcmp(argv[5],"apply")&&strcmp(argv[5],"keep"))return 2;
            m.object=!strcmp(argv[5],"apply");r=call(&m,0,&out);}
        else if(!strcmp(argv[1],"source")&&argc==6){
            m.type=AG_WORK_SOURCE;m.object=strtoull(argv[3],0,10);m.revision=strtoull(argv[4],0,10);
            struct ag_slice slice={(unsigned)strtoul(argv[5],0,10),16384};m.bytes=sizeof slice;
            r=call(&m,&slice,&out);if(!r&&m.bytes)fwrite(out,1,m.bytes,stdout);}
        else if(!strcmp(argv[1],"document")){m.type=AG_DOCUMENT;r=call(&m,0,&out);if(!r&&m.bytes)fwrite(out,1,m.bytes,stdout);}
        else if(!strcmp(argv[1],"revise")&&argc==4){m.type=AG_REVISE;m.bytes=strlen(argv[3]);r=call(&m,argv[3],&out);}
        else {struct ag_control c={0};m.type=AG_CONTROL;m.bytes=sizeof c;
            if(!strcmp(argv[1],"pause"))c.phase=AG_PAUSED;
            else if(!strcmp(argv[1],"cancel"))c.phase=AG_CANCELLED;
            else if(!strcmp(argv[1],"resume"))c.phase=AG_QUEUED;
            else if(!strcmp(argv[1],"budget")){c.phase=AG_QUEUED;c.extend=32;}
            else return 2;r=call(&m,&c,&out);}
    }else return 2;
    free(out);return r<0?1:0;
}
