/* SPDX-License-Identifier: MIT */
#include "internal.h"

static int list_modules(void *context,char names[][48],int max)
{
    StEngine *e=context;int n=0;const char *dirs[]={e->state.project,e->modules};
    for(unsigned k=0;k<2;k++){DIR *dir=opendir(dirs[k]);if(!dir)continue;struct dirent *entry;
        while(n<max&&(entry=readdir(dir))){size_t len=strlen(entry->d_name);
            if(len<=3||len>=51||strcmp(entry->d_name+len-3,".as"))continue;
            int duplicate=0;for(int i=0;i<n;i++)if(strlen(names[i])==len-3&&!memcmp(names[i],entry->d_name,len-3))duplicate=1;
            if(!duplicate){memcpy(names[n],entry->d_name,len-3);names[n++][len-3]=0;}
        }closedir(dir);
    }return n;
}
static int read_module(void *context,const char *name,char *out,int max)
{
    StEngine *e=context;const char *dirs[]={e->state.project,e->modules};
    for(unsigned k=0;k<2;k++){char path[512];snprintf(path,sizeof path,"%s/%s.as",dirs[k],name);
        for(int i=0;i<e->state.count;i++)if(!strcmp(e->state.docs[i].path,path)){
            if(e->state.docs[i].length>max)return -1;memcpy(out,e->state.docs[i].text,(size_t)e->state.docs[i].length);return e->state.docs[i].length;
        }
        char *text;int bytes,exists;if(st_read_file(path,&text,&bytes,&exists)<0)continue;
        if(exists&&bytes<=max){memcpy(out,text,(size_t)bytes);free(text);return bytes;}free(text);
    }return -1;
}
void st_engine_complete(StEngine *e)
{
    StDocument *d=st_current(e);if(!d)return;
    CmpProviders providers={e,list_modules,read_module};
    e->state.completion_count=as_complete_with(d->text,d->length,d->caret,e->state.completions,64,&providers);
    e->state.completion_start=d->caret;
    while(e->state.completion_start>0){char c=d->text[e->state.completion_start-1];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))break;e->state.completion_start--;}
}
void st_engine_dismiss_completion(StEngine *e){e->state.completion_count=0;}
int st_engine_accept_completion(StEngine *e,int index)
{
    StDocument *d=st_current(e);if(!d||index<0||index>=e->state.completion_count)return -1;
    int old_anchor=d->anchor;d->anchor=e->state.completion_start;
    int r=st_engine_insert(e,e->state.completions[index].insert,(int)strlen(e->state.completions[index].insert));
    if(r<0)d->anchor=old_anchor;return r;
}
int st_engine_select_problem(StEngine *e,int index)
{
    int tab=e->state.problem_tab;
    if(tab<0||tab>=e->state.count||e->state.docs[tab].revision!=e->state.problem_revision){
        st_engine_notice(e,"Diagnostics belong to an older version. Check again.");return -1;
    }
    if(index<0||index>=e->state.problem_count)return -1;
    st_engine_activate(e,tab);StProblem *p=&e->state.problems[index];return st_engine_select(e,p->start,p->end);
}
