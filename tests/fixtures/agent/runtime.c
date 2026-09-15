/* SPDX-License-Identifier: MIT */
#include "../../../c/lib/agent/sdk.h"
#include "../../../c/apps/agent/userfs.h"
#include "../../../c/apps/logit.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#define CHECK(x,name) do{if(!(x)){printf("AGENT_RUNTIME_FAIL %s\n",name);return 1;}checks++;}while(0)
static unsigned checks;
int main(int argc,char **argv)
{
    struct aex_agent_identity self;
    if(ag_self(&self)<0)return 2;
    if(self.mode==AEX_ACT_WORKER){
        struct aex_agent_identity peer;unsigned ok=ag_peer(3,&peer)==0&&peer.pid==self.parent_pid;
        ok&=_sys(SYS_OPEN,(long)"/docs/source.md",0,0)<0;
        ok&=_sys(SYS_GET_TIME,0,0,0)<0;
        char c;ok&=read(4,&c,1)<0;
        if(write(3,&ok,sizeof ok)!=sizeof ok)return 3;
        /* A marked worker must be reaped even if every next call is denied.
         * This uses an ordinary clock query, with no crafted user frame. */
        for(;;)(void)_sys(SYS_GET_TIME,0,0,0);
    }
    if(argc==3&&!strcmp(argv[1],"stop")){
        int pid=atoi(argv[2]);if(pid<=0)return 2;
        int r=(int)_sys(SYS_KILL,pid,0,0);printf("AGENT_RUNTIME_STOP pid=%d rc=%d\n",pid,r);return r<0;
    }
    if(argc==3&&!strcmp(argv[1],"open-path")){sys_open_path(argv[2]);return 0;}
    if(argc==2&&!strcmp(argv[1],"open-document")){
        sys_open_path("/textedit.aex");return 0;
    }
    if(argc==2&&!strcmp(argv[1],"install-model-fixture")){
        /* Only a private test gateway capability is packed under /fixture.
         * Install it through the guest filesystem after the service is ready,
         * so recovery cannot accidentally rely on a preconfigured boot disk. */
        const char *names[]={"agent.conf","agent.key"};
        for(unsigned i=0;i<2;i++){
            char from[64],to[64],buf[4096];
            snprintf(from,sizeof from,"/fixture/%s",names[i]);
            snprintf(to,sizeof to,"/etc/%s",names[i]);
            int in=open(from,O_RDONLY),out=open(to,O_CREAT|O_TRUNC|O_WRONLY,0600);
            CHECK(in>=0&&out>=0,"configuration fixture opens");
            long n;while((n=read(in,buf,sizeof buf))>0){
                long off=0;while(off<n){long w=write(out,buf+off,(size_t)(n-off));
                    CHECK(w>0,"configuration fixture write");off+=w;}}
            CHECK(n==0&&!fsync(out)&&!close(out)&&!close(in),"configuration fixture durable");
        }
        puts("AGENT_MODEL_FIXTURE_INSTALLED");return 0;
    }
    if(argc==4&&!strcmp(argv[1],"caps")){
        char text[8192],expected[128];int fd=open(argv[2],O_RDONLY);if(fd<0)return 1;
        long n=read(fd,text,sizeof text-1);int cr=close(fd);if(n<0||cr<0)return 1;text[n]=0;
        int z=snprintf(expected,sizeof expected,"%s version=1 state=1\n",argv[3]);
        if(z<0||z>=(int)sizeof expected||!strstr(text,expected))return 1;
        puts("AGENT_CAPABILITY_VERIFIED");return 0;
    }
    struct aex_agent_identity installed;CHECK(!ag_registry("/bin/agent-runtime-test",&installed,0),"registered image");
    int extra=open("/docs/source.md",O_RDONLY);CHECK(extra>=3,"ambient parent descriptor");
    int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair),"private channel");
    int alias=dup(pair[1]);CHECK(alias>=0,"descriptor alias");
    CHECK(ag_spawn_worker("/bin/agent-runtime-test",&installed,pair[1])<0,"aliased channel refused");close(alias);
    struct aex_agent_identity wrong=installed;wrong.image_hash[0]^=1;
    CHECK(ag_spawn_worker("/bin/agent-runtime-test",&wrong,pair[1])<0,"unregistered version refused");
    int pid=ag_spawn_worker("/bin/agent-runtime-test",&installed,pair[1]);CHECK(pid>0,"restricted child starts");
    struct aex_agent_identity peer;CHECK(!ag_peer(pair[0],&peer)&&peer.pid==pid&&peer.mode==AEX_ACT_WORKER,"kernel peer identity");
    unsigned ok=0;CHECK(read(pair[0],&ok,sizeof ok)==sizeof ok&&ok,"worker isolation");
    CHECK(_sys(SYS_KILL,pid,0,0)==0,"request child stop");
    unsigned long long deadline=monotonic_ms()+3000;int status=0,result=0;
    while(monotonic_ms()<deadline){result=waitpid(pid,&status,WNOHANG);if(result==pid)break;_sys(SYS_YIELD,0,0,0);}
    CHECK(result==pid,"denied syscall still observes stop");close(pair[0]);close(extra);

    int fd=open("/docs/private.md",O_CREAT|O_TRUNC|O_WRONLY,0600);CHECK(fd>=0,"private fixture");
    CHECK(write(fd,"private",7)==7&&!fsync(fd)&&!close(fd)&&!chmod("/docs/private.md",0600),"private fixture durable");
    void *bytes=0;size_t length=0;
    CHECK(ag_userfs(1042,1042,AG_U_READ,"/etc/agent.key",0,0,&bytes,&length,0)<0&&!bytes,"model credential is not user-readable");
    CHECK(ag_userfs(1042,1042,AG_U_READ,"/docs/private.md",0,0,&bytes,&length,0)<0&&!bytes,"grant cannot bypass user permissions");
    CHECK(!mkdir("/docs/user-output",0777)&&!chmod("/docs/user-output",0777),"writable output directory");
    CHECK(!ag_userfs(1042,1042,AG_U_CREATE,"/docs/user-output/new.md","one",3,0,0,0),"create as granting user");
    struct stat st;CHECK(!stat("/docs/user-output/new.md",&st)&&st.st_uid==1042,"artifact owner");
    CHECK(ag_userfs(1042,1042,AG_U_CREATE,"/docs/user-output/new.md","two",3,0,0,0)<0,"existing artifact refused");
    CHECK(!ag_userfs(1042,1042,AG_U_READ,"/docs/user-output/new.md",0,0,&bytes,&length,0)&&length==3&&!memcmp(bytes,"one",3),"existing contents retained");free(bytes);
    printf("AGENT_RUNTIME_PASS checks=%u\n",checks);return 0;
}
