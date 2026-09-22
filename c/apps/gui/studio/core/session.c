/* SPDX-License-Identifier: MIT */
#include "internal.h"

void st_session_save(StEngine *e)
{
    if(!e->host.set)return;
    void *ctx=e->host.context;
    e->host.set(ctx,"app.studio.project",e->state.project);
    for(int i=0;i<ST_TABS;i++){char key[40];snprintf(key,sizeof key,"app.studio.tab.%d",i);
        e->host.set(ctx,key,i<e->state.count?e->state.docs[i].path:"");}
    char value[16];snprintf(value,sizeof value,"%d",e->state.active);e->host.set(ctx,"app.studio.active",value);
    if(e->host.commit)e->host.commit(ctx);
}
int st_engine_project(StEngine *e,const char *path)
{
    struct stat st;
    if(!path||strlen(path)>=sizeof e->state.project||stat(path,&st)<0||!S_ISDIR(st.st_mode)){
        st_engine_notice(e,"Project directory is not available");return -1;
    }
    if(strcmp(e->state.project,path))e->expanded_count=0;
    snprintf(e->state.project,sizeof e->state.project,"%s",path);st_engine_refresh(e);st_session_save(e);return 0;
}
void st_engine_restore(StEngine *e,const char *launch_path)
{
    if(launch_path&&*launch_path){
        char path[128];int n=snprintf(path,sizeof path,"%s%s",launch_path[0]=='/'?"":"/",launch_path);
        if(n<0||n>=(int)sizeof path){st_engine_notice(e,"Path is too long");return;}
        struct stat st;
        if(stat(path,&st)==0&&S_ISDIR(st.st_mode)){st_engine_project(e,path);return;}
        char parent[128];strcpy(parent,path);char *slash=strrchr(parent,'/');
        if(slash){if(slash==parent)slash[1]=0;else *slash=0;st_engine_project(e,parent);}
        st_engine_open(e,path);return;
    }
    if(e->host.get){
        char project[128]={0},paths[ST_TABS][128]={{0}},active[16]={0};void *ctx=e->host.context;
        /* Read the entire session before open() commits any updated settings. */
        e->host.get(ctx,"app.studio.project",project,sizeof project);
        e->host.get(ctx,"app.studio.active",active,sizeof active);
        for(int i=0;i<ST_TABS;i++){char key[40];snprintf(key,sizeof key,"app.studio.tab.%d",i);e->host.get(ctx,key,paths[i],sizeof paths[i]);}
        if(project[0])st_engine_project(e,project);
        int selected=0,old_active=atoi(active),missing=0;
        for(int i=0;i<ST_TABS;i++)if(paths[i][0]){
            if(st_engine_open(e,paths[i])<0){missing++;continue;}
            if(i==old_active)selected=e->state.active;
        }
        st_engine_activate(e,selected);
        /* Also commit when every saved path disappeared: activate() is a no-op
         * for an empty session, which otherwise retries those ghosts forever. */
        st_session_save(e);
        if(missing)st_engine_notice(e,"Unavailable files were removed from the saved tabs.");
    }
    st_engine_refresh(e);
}
