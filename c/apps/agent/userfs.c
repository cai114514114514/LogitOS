/* SPDX-License-Identifier: MIT */
#include "userfs.h"
#include "../../lib/agent/task.h"
#include "../../lib/agent/files.h"
#include "../logit.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <grp.h>
#include <sys/wait.h>

struct answer {int status;unsigned bytes;struct stat st;};
static int exact(int fd,void *p,size_t n,int writing)
{while(n){long z=writing?write(fd,p,n):read(fd,p,n);if(z<=0)return -1;p=(char *)p+z;n-=z;}return 0;}
static int plain_path(const char *p,int parent)
{
    char name[AG_PATH];size_t n=strlen(p);
    if(!n||n>=sizeof name||p[0]!='/')return 0;memcpy(name,p,n+1);
    if(parent){char *q=strrchr(name,'/');if(q==name)q[1]=0;else *q=0;}
    for(char *q=name+1;;q++)if(*q=='/'||!*q){char c=*q;*q=0;struct stat st;
        if(lstat(name,&st)<0||S_ISLNK(st.st_mode))return 0;*q=c;if(!c)break;}
    return 1;
}
int ag_userfs(unsigned uid,unsigned gid,unsigned op,const char *path,
              const void *input,size_t size,void **output,size_t *length,struct stat *st)
{
    if(output)*output=0;if(length)*length=0;
    if(size>AEX_AGENT_DOCUMENT_MAX)return AG_E_LIMIT;
    int channel[2];if(pipe(channel)<0)return AG_E_IO;
    int pid=fork();if(pid<0){close(channel[0]);close(channel[1]);return AG_E_IO;}
    if(!pid){
        /* Close inherited service/model/client handles before dropping uid.
         * Pipe ownership does not depend on the authenticated socket identity
         * that fork deliberately clears. No application code runs here. */
        for(int fd=0;fd<32;fd++)if(fd!=channel[1])close(fd);
        struct answer a={.status=AG_E_SCOPE};char *data=0;
        if(setgroups(0,0)<0||setgid(gid)<0||setuid(uid)<0)goto done;
        if(op==AG_U_RESOLVE){
            if(size!=sizeof(struct logit_file_id))goto done;
            data=malloc(AG_PATH);if(!data){a.status=AG_E_IO;goto done;}
            a.status=ag_file_path(input,data,AG_PATH);
            if(!a.status)a.bytes=(unsigned)strlen(data)+1;goto done;
        }
        if(!plain_path(path,op==AG_U_CREATE||op==AG_U_REF))goto done;
        if(op==AG_U_REF){
            data=malloc(sizeof(struct ag_file_info));if(!data){a.status=AG_E_IO;goto done;}
            struct ag_file_info *info=(void *)data;
            a.status=ag_file_ref(path,&info->id,&info->revision);
            if(!a.status)a.bytes=sizeof(struct ag_file_info);goto done;
        }
        if(op==AG_U_CREATE){
            long r=_sys(SYS_CREATE_FILE,(long)path,(long)input,size);
            a.status=r==(long)size?0:AG_E_IO;
            if(!a.status){int fd=open(path,O_RDONLY);
                if(fd<0)a.status=AG_E_IO;
                else {if(fsync(fd)<0)a.status=AG_E_IO;if(close(fd)<0)a.status=AG_E_IO;}}
            goto done;
        }
        if(lstat(path,&a.st)<0)goto done;
        if(op==AG_U_STAT){a.status=0;goto done;}
        if(op==AG_U_DIRECTORY){
            /* access() currently checks existence only. Primary uid/gid mode
             * checks here are conservative; the eventual create runs under
             * the same real VFS credentials and rechecks the directory. */
            unsigned bits=uid==a.st.st_uid?(a.st.st_mode>>6):gid==a.st.st_gid?(a.st.st_mode>>3):a.st.st_mode;
            if(S_ISDIR(a.st.st_mode)&&(!uid||(bits&3)==3))a.status=0;goto done;
        }
        if(op==AG_U_READ&&S_ISREG(a.st.st_mode)&&a.st.st_size>=0&&(uint64_t)a.st.st_size<=AEX_AGENT_DOCUMENT_MAX){
            int fd=open(path,O_RDONLY);if(fd<0)goto done;
            /* fstat the opened object, not the earlier pathname; a rename
             * cannot smuggle a larger or special file into this snapshot. */
            if(fstat(fd,&a.st)<0||!S_ISREG(a.st.st_mode)||a.st.st_size<0||(uint64_t)a.st.st_size>AEX_AGENT_DOCUMENT_MAX){close(fd);goto done;}
            a.bytes=(unsigned)a.st.st_size;data=malloc((size_t)a.bytes+1);
            if(data&&!exact(fd,data,a.bytes,0)){char extra;if(read(fd,&extra,1)==0)a.status=0;}
            if(close(fd)<0)a.status=AG_E_IO;
        }else if(op==AG_U_LIST&&S_ISDIR(a.st.st_mode)){
            DIR *dir=opendir(path);if(!dir)goto done;
            data=malloc(AG_OBJECTS*64+1);struct dirent *e;a.status=data?0:AG_E_IO;
            while(!a.status&&(e=readdir(dir))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
                size_t n=strlen(e->d_name)+1;
                if(n>AG_OBJECTS*64-a.bytes){a.status=AG_E_LIMIT;break;}
                memcpy(data+a.bytes,e->d_name,n);a.bytes+=(unsigned)n;}
            if(closedir(dir)<0)a.status=AG_E_IO;
        }
done:
        if(a.status)a.bytes=0;
        int ok=exact(channel[1],&a,sizeof a,1);
        if(!ok&&a.bytes)ok=exact(channel[1],data,a.bytes,1);
        free(data);close(channel[1]);_exit(ok?1:0);
    }
    close(channel[1]);struct answer a={.status=AG_E_IO};int rc=exact(channel[0],&a,sizeof a,0);void *data=0;
    if(!rc&&a.bytes>AEX_AGENT_DOCUMENT_MAX)rc=-1;
    if(!rc&&(a.bytes||output)){data=malloc((size_t)a.bytes+1);if(!data||exact(channel[0],data,a.bytes,0)<0)rc=-1;else ((char *)data)[a.bytes]=0;}
    close(channel[0]);int status=0;if(waitpid(pid,&status,0)!=pid||status)rc=-1;
    if(rc||a.status){free(data);return rc?AG_E_IO:a.status;}
    if(output)*output=data;else free(data);if(length)*length=a.bytes;if(st)*st=a.st;return 0;
}
