/* Ordinary POSIX IPv4 client/server using the shipped libc. The control
 * omits htons on the client port and must fail the actual connect check. */
#include "clib.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
static int failures;
static int check(int ok,const char *name)
{if(!ok){failures++;outs("INET_FAIL ");outs(name);outc('\n');}return ok;}
static int exact(int fd,char *p,int size)
{int n=0;while(n<size){int r=recv(fd,p+n,size-n,0);if(r<=0)return -1;n+=r;}return n;}
int main(int argc,char **argv)
{
    (void)argv;
    struct sockaddr_in a;memset(&a,0,sizeof a);a.sin_family=AF_INET;
    a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons(19081);
    int listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP),one=1;
    if(!check(listener>=0,"server socket"))return 1;
    check(setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one)==0,"server option");
    if(!check(bind(listener,(struct sockaddr *)&a,sizeof a)==0&&listen(listener,2)==0,"loopback bind and listen")){close(listener);return 1;}
    struct sockaddr_in name;socklen_t len=sizeof name;
    check(getsockname(listener,(struct sockaddr *)&name,&len)==0&&name.sin_port==a.sin_port&&name.sin_addr.s_addr==a.sin_addr.s_addr,"listener name and byte order");
    int child=fork();
    if(child==0){
        struct sockaddr_in peer;len=sizeof peer;int fd=accept(listener,(struct sockaddr *)&peer,&len);close(listener);
        if(!check(fd>=0,"accept"))return 1;
        check(peer.sin_family==AF_INET&&peer.sin_addr.s_addr==a.sin_addr.s_addr&&ntohs(peer.sin_port)>=49152,"accepted peer and byte order");
        len=sizeof name;check(getsockname(fd,(struct sockaddr *)&name,&len)==0&&name.sin_port==a.sin_port&&name.sin_addr.s_addr==a.sin_addr.s_addr,"accepted local endpoint");
        char b[8];check(exact(fd,b,8)==8&&memcmp(b,"hello\0IP",8)==0,"server receives binary bytes");
        char end;check(recv(fd,&end,1,0)==0,"server observes half close");
        check(send(fd,b,8,0)==8,"server response after EOF");close(fd);return failures?1:0;
    }
    if(!check(child>0,"fork server")){close(listener);return 1;}
    close(listener);int fd=socket(AF_INET,SOCK_STREAM,0);
    if(argc>1)a.sin_port=19081; /* missing conversion, private runtime control */
    if(!check(fd>=0&&connect(fd,(struct sockaddr *)&a,sizeof a)==0,"client connect")){
        if(fd>=0)close(fd);_sys(SYS_KILL,child,LOGIT_SIGKILL,LOGIT_KILL_SIGNAL);waitpid(child,0,0);return 1;
    }
    struct timeval timeout={5,0};check(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout)==0,"client receive timeout");
    len=sizeof name;check(getsockname(fd,(struct sockaddr *)&name,&len)==0&&ntohs(name.sin_port)>=49152&&name.sin_addr.s_addr==htonl(INADDR_LOOPBACK),"client ephemeral local endpoint");
    check(send(fd,"hello\0IP",8,0)==8&&shutdown(fd,SHUT_WR)==0,"client write and half close");
    char b[8];check(exact(fd,b,8)==8&&memcmp(b,"hello\0IP",8)==0,"client receives complete response");close(fd);
    int status=255;check(waitpid(child,&status,0)==child&&status==0,"server completes all checks");
    outs("INET_FAILURES=");outn(failures);outc('\n');return failures?1:0;
}
