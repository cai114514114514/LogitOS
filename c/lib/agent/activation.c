/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sdk.h"
#include "catalog.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
/* This path is shared by all OWNED applications. Normal invocation returns to
 * the original CRT; workers are selected by kernel identity, never argv. */
int ag_activation(int argc,char **argv)
{
    struct aex_agent_identity id;
    if(ag_self(&id)<0)return 125;
    const struct ag_app *app=ag_app_find(id.app_id);if(!app)return 125;
    if(id.mode==AEX_ACT_WORKER)return ag_worker(!strcmp(app->name,"files")?AG_FINDER:!strcmp(app->name,"textedit")?AG_EDITOR:AG_SPECIALIST);
    if(argc>=2&&!strcmp(argv[1],"--aex-capabilities")){
        printf("%s version=%u state=%u\n%s\nAgent actions: analyze supplied context; draft text. External changes require separately granted actions.\n",id.app_id,id.capability_version,id.state_version,app->domain);return 0;}
    if(argc==2&&!strcmp(argv[1],"--aex-memory")){
        char *text=0;uint32_t bytes=0;uint64_t revision=0;int rc=ag_memory_read(0,&text,&bytes,&revision);
        if(!rc){printf("MEMORY app=%s revision=%llu bytes=%u\n",id.app_id,(unsigned long long)revision,bytes);if(bytes)fwrite(text,1,bytes,stdout);}
        free(text);return rc<0;}
    if(argc>=5&&!strcmp(argv[1],"--task-from")&&!strcmp(app->name,"files")){
        if(argc-4>AG_OBJECTS||strlen(argv[2])>=AG_GOAL)return 2;
        uint64_t context;int r=ag_publish_selection((const char *const *)(argv+4),(unsigned)(argc-4),argv[3],&context);
        if(r<0)return 1;struct ag_create req={0};req.context=context;
        strcpy(req.goal,argv[2]);if(strlen(argv[3])>=AG_PATH)return 2;strcpy(req.output,argv[3]);
        struct ag_message m={.type=AG_CREATE,.bytes=sizeof req};void *out=0;r=ag_call(&m,&req,&out);free(out);
        if(!r)printf("TASK_CREATED id=%llu\n",(unsigned long long)m.task);else printf("Task refused (%d)\n",r);return r<0?1:0;
    }
    if(argc>=2&&!strcmp(argv[1],"--ask")){
        if(argc!=4){printf("usage: %s --ask 'goal' 'explicit context text'\n",argv[0]);return 2;}
        uint64_t context;int r=ag_publish_state(app->name,argv[3],strlen(argv[3]),1,&context);
        if(r<0){printf("Task context refused (%d).\n",r);return 1;}
        struct ag_create req={0};req.context=context;
        if(strlen(argv[2])>=sizeof req.goal)return 2;
        strcpy(req.goal,argv[2]);strcpy(req.output,"/docs");
        struct ag_message m={.type=AG_CREATE,.bytes=sizeof req};void *out=0;r=ag_call(&m,&req,&out);free(out);
        if(r<0){printf("Task creation refused (%d).\n",r);return 1;}
        printf("Task %llu saved; follow it in Tasks.\n",(unsigned long long)m.task);return 0;
    }
    return -1;
}
