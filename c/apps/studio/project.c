/* SPDX-License-Identifier: MIT */
#include "internal.h"

/* Only enumerate visible directories. The old eager recursion read every
 * descendant on open/save, and clicking a folder replaced the project root.
 * Expansion is keyed by path so refresh and sorting cannot move it to a
 * different folder. A collapsed subtree keeps its descendants' expansion. */
int st_list_directory(const char *path,StListing *out)
{
    if(!path||strlen(path)>=sizeof out->path){errno=ENAMETOOLONG;return -1;}
    DIR *dir=opendir(path);if(!dir)return -1;
    memset(out,0,sizeof *out);strcpy(out->path,path);struct dirent *entry;
    while((entry=readdir(dir))){
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")||!strcmp(entry->d_name,".studio"))continue;
        if(out->count==ST_FILES){out->limited=1;break;}
        StFile f={0};int n=snprintf(f.path,sizeof f.path,"%s%s%s",path,strcmp(path,"/")?"/":"",entry->d_name);
        if(n<0||n>=(int)sizeof f.path||strlen(entry->d_name)>=sizeof f.name){out->limited=1;continue;}
        struct stat st;if(stat(f.path,&st)<0)continue;
        strcpy(f.name,entry->d_name);f.directory=S_ISDIR(st.st_mode);
        int at=out->count;
        while(at>0){StFile *prev=&out->files[at-1];
            if(prev->directory>f.directory||(prev->directory==f.directory&&strcmp(prev->name,f.name)<=0))break;
            out->files[at]=*prev;at--;
        }out->files[at]=f;out->count++;
    }closedir(dir);return 0;
}
static int expanded_at(StEngine *e,const char *path)
{for(int i=0;i<e->expanded_count;i++)if(!strcmp(e->expanded[i],path))return i;return -1;}
void st_engine_refresh(StEngine *e)
{
    StListing list;e->state.tree_count=e->state.tree_limited=0;
    if(st_list_directory(e->state.project,&list)<0){st_engine_notice(e,"Could not read the project folder");return;}
    memcpy(e->state.files,list.files,(size_t)list.count*sizeof(StFile));
    e->state.tree_count=list.count;e->state.tree_limited=list.limited;
    for(int i=0;i<e->state.tree_count;i++){
        StFile *f=&e->state.files[i];f->expanded=f->directory&&expanded_at(e,f->path)>=0;
        if(!f->expanded)continue;
        if(f->depth>=32||st_list_directory(f->path,&list)<0){e->state.tree_limited=1;continue;}
        int n=list.count,room=ST_FILES-e->state.tree_count;
        if(n>room){n=room;e->state.tree_limited=1;}if(list.limited)e->state.tree_limited=1;
        memmove(f+1+n,f+1,(size_t)(e->state.tree_count-i-1)*sizeof *f);
        for(int j=0;j<n;j++){list.files[j].depth=f->depth+1;f[j+1]=list.files[j];}
        e->state.tree_count+=n;
    }
}
int st_engine_toggle_folder(StEngine *e,int index)
{
    if(index<0||index>=e->state.tree_count||!e->state.files[index].directory)return -1;
    int at=expanded_at(e,e->state.files[index].path);
    if(at>=0){memmove(e->expanded[at],e->expanded[at+1],(size_t)(e->expanded_count-at-1)*sizeof e->expanded[0]);e->expanded_count--;}
    else {if(e->expanded_count==ST_FILES){st_engine_notice(e,"Close another folder before expanding more");return -1;}
        strcpy(e->expanded[e->expanded_count++],e->state.files[index].path);}
    st_engine_refresh(e);return 0;
}
int st_engine_create_entry(StEngine *e,const char *parent,const char *name,int directory)
{
    int n=name?(int)strlen(name):0;
    if(!n||n>=64||!st_utf8(name,n)||strchr(name,'/')||strchr(name,'\\')||!strcmp(name,".")||!strcmp(name,"..")){
        st_engine_notice(e,"Enter a file or folder name without path separators");return -1;}
    for(int i=0;i<n;i++)if((unsigned char)name[i]<32){st_engine_notice(e,"Names cannot contain control characters");return -1;}
    char path[128];int len=snprintf(path,sizeof path,"%s%s%s",parent,strcmp(parent,"/")?"/":"",name);
    if(len<0||len>=(int)sizeof path){st_engine_notice(e,"Path is too long");return -1;}
    if(!directory&&e->state.count==ST_TABS){st_engine_notice(e,"Close a tab before creating another file");return -1;}
    int r;
    if(directory)r=mkdir(path,0755);
    else {int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0644);r=fd<0?-1:close(fd);}
    if(r<0){char message[256];snprintf(message,sizeof message,"Could not create %s: %s",name,strerror(errno));st_engine_notice(e,message);return -1;}
    if(strcmp(parent,e->state.project)&&expanded_at(e,parent)<0&&e->expanded_count<ST_FILES)strcpy(e->expanded[e->expanded_count++],parent);
    st_engine_refresh(e);
    if(!directory)return st_engine_open(e,path);
    st_engine_notice(e,"Folder created");return 0;
}

int st_engine_remove_entry(StEngine *e,const char *path)
{
    char target[128];int index=-1;
    for(int i=0;i<e->state.tree_count;i++)if(!strcmp(e->state.files[i].path,path)){index=i;break;}
    if(index<0){st_engine_notice(e,"Select a file or folder in the project tree");return -1;}
    strcpy(target,e->state.files[index].path);
    if(st_runner_busy(&e->state.runner)){st_engine_notice(e,"Stop the running process before deleting a file");return -1;}
    struct stat st;int present=stat(target,&st)==0;
    if(!present&&errno!=ENOENT)goto failed;
    if(present&&S_ISDIR(st.st_mode)){
        if(rmdir(target)<0)goto failed;
    }else {
        /* Remove recovery slots before the source: reversing this order lets
         * a leftover dirty slot resurrect a deliberately deleted document.
         * On failure the live buffer remains open and can checkpoint again. */
        if(st_forget_drafts(target)<0)goto failed;
        if(unlink(target)<0&&errno!=ENOENT)goto failed;
        for(int i=0;i<e->state.count;i++)if(!strcmp(e->state.docs[i].path,target)){
            StDocument *d=&e->state.docs[i];st_dispose(d);
            memmove(d,d+1,(size_t)(e->state.count-i-1)*sizeof *d);e->state.count--;
            memset(&e->state.docs[e->state.count],0,sizeof *d);
            if(e->state.active>i)e->state.active--;
            if(e->state.active>=e->state.count)e->state.active=e->state.count?e->state.count-1:0;
            break;
        }
    }
    e->state.problem_tab=-1;e->state.problem_count=0;e->state.completion_count=0;
    e->check_pending=0;st_engine_refresh(e);st_session_save(e);st_engine_notice(e,"Deleted");return 0;
failed:
    for(int i=0;i<e->state.count;i++)if(!strcmp(e->state.docs[i].path,target))e->state.docs[i].checkpoint=0;
    e->checkpoint_due=st_now(e)+500;
    {char message[256];snprintf(message,sizeof message,"Could not delete: %s (folders must be empty)",strerror(errno));st_engine_notice(e,message);}
    return -1;
}

int st_engine_open(StEngine *e,const char *path)
{
    if(strlen(path)>=sizeof e->state.docs[0].path){st_engine_notice(e,"Path is too long");return -1;}
    for(int i=0;i<e->state.count;i++)if(!strcmp(e->state.docs[i].path,path)){e->state.active=i;e->state.completion_count=0;return 0;}
    struct stat st;if(stat(path,&st)==0&&S_ISDIR(st.st_mode)){return st_engine_project(e,path);}
    if(e->state.count==ST_TABS){st_engine_notice(e,"Eight documents are open. Close a tab before opening another.");return -1;}
    int r=st_open_document(&e->state.docs[e->state.count],path);if(r<0){st_engine_notice(e,r==-2?"File no longer exists and has no unsaved recovery draft.":"Could not open complete UTF-8 text (limit 1 MiB). No file was changed.");return -1;}
    e->state.active=e->state.count++;e->state.completion_count=0;e->check_due=st_now(e)+850;e->check_pending=1;
    st_engine_notice(e,r?"Recovered unsaved draft. Save checks the original file for conflicts.":"Document opened");st_session_save(e);return 0;
}

int st_engine_save(StEngine *e)
{StDocument *d=st_current(e);if(!d)return 0;int r=st_save_document(d);st_engine_notice(e,r==-2?"File changed outside Studio. Your draft is retained.":r<0?"Save failed. Keep this tab open; the draft is retained.":"Saved");if(!r)st_engine_refresh(e);return r;}
