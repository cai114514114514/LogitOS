/* Real guest PTY acceptance. The parent is a terminal owner, not a synthetic
 * byte-loop: it allocates one native pair, forks, gives a new-session child the
 * slave as stdio, execs this same image, and observes the child's reply through
 * the master. `pipe-control` changes only child stdin, so isatty must catch the
 * wiring error after exec rather than merely inspecting the parent's fd. */
#include "clib.h"
#include <pty.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

static int failures,checks;
static void check(int ok,const char *name)
{
    checks++;
    if(!ok){failures++;outs("PTY_FAIL ");outs(name);outc('\n');}
    else {outs("PTY_PASS ");outs(name);outc('\n');}
}

static int has(const char *hay,int n,const char *needle)
{
    int m=c_strlen(needle);
    for(int i=0;i+m<=n;i++){int same=1;for(int j=0;j<m;j++)if(hay[i+j]!=needle[j]){same=0;break;}if(same)return 1;}
    return 0;
}

struct capture { char b[1024];int n; };
static int wait_text(int fd,struct capture *c,const char *needle)
{
    /* 200000 yields is a guest-scheduler budget, not host wall time. The test
     * stays insensitive to another agent's QEMU load, while a dead child still
     * terminates the gate instead of blocking forever on the master. */
    for(int turn=0;turn<200000;turn++){
        int room=(int)sizeof c->b-c->n;
        int n=room>0?(int)read(fd,c->b+c->n,(size_t)room):-1;
        if(n>0){c->n+=n;if(has(c->b,c->n,needle))return 1;}
        else if(n==0)return has(c->b,c->n,needle);
        else if(errno!=EAGAIN)return 0;
        sys_yield();
    }
    return has(c->b,c->n,needle);
}

static int child_image(void)
{
    if(!isatty(0)){outs("PTY_FAIL child stdin is terminal\n");return 1;}
    pid_t pg=getpgrp(),fg=tcgetpgrp(0);
    if(pg<=0||fg!=pg){outs("PTY_FAIL child owns foreground process group\n");return 1;}
    outs("PTY_CHILD_READY pgid=");outn(pg);outc('\n');
    char line[32];int n=(int)read(0,line,sizeof line);
    if(n!=6||line[0]!='z'||line[1]!='e'||line[2]!='b'||line[3]!='r'||line[4]!='a'||line[5]!='\n'){
        outs("PTY_FAIL child read parent line\n");return 1;
    }
    outs("PTY_CHILD_REPLY zebra\n");
    return 0;
}

int main(int argc,char **argv)
{
    if(argc>1&&c_streq(argv[1],"child"))return child_image();
    int pipe_control=argc>1&&c_streq(argv[1],"pipe-control");
    int master=-1,slave=-1,pipes[2]={-1,-1};
    check(openpty(&master,&slave,0,0,0)==0,"openpty returns one pair");
    if(master<0||slave<0)goto done;
    check(isatty(slave)==1,"slave isatty true");
    check(pipe(pipes)==0,"control pipe opens");
    if(pipes[0]<0||pipes[1]<0)goto done;
    check(isatty(pipes[0])==0,"pipe isatty false");
    sys_set_nonblock(master);

    pid_t pid=fork();
    if(pid<0){check(0,"fork child");goto done;}
    if(pid==0){
        close(master);close(pipes[1]);
        /* Put diagnostics on the PTY before setup so a failed setsid/TIOCSCTTY
         * remains visible to the parent instead of escaping on serial fd 1. */
        dup2(slave,1);dup2(slave,2);
        pid_t sid=setsid();
        if(sid!=getpid()){outs("PTY_FAIL child creates session\n");_exit(1);}
        if(ioctl(slave,TIOCSCTTY,(void *)0)<0){outs("PTY_FAIL child acquires controlling terminal\n");_exit(1);}
        pid_t pg=getpgrp();
        if(tcsetpgrp(slave,pg)<0||tcgetpgrp(slave)!=pg){outs("PTY_FAIL child sets foreground process group\n");_exit(1);}
        dup2(pipe_control?pipes[0]:slave,0);
        if(slave>2)close(slave);if(pipes[0]>2)close(pipes[0]);
        char *av[]={"pty-check","child",0};
        execv("/bin/pty-check",av);
        outs("PTY_FAIL child exec\n");_exit(1);
    }
    check(1,"fork child");
    close(pipes[0]);pipes[0]=-1;close(pipes[1]);pipes[1]=-1;
    struct capture cap={{0},0};
    int ready=wait_text(master,&cap,"PTY_CHILD_READY");
    check(ready,"child runs on PTY slave after exec");
    if(!ready){
        if(cap.n>0)write(1,cap.b,(size_t)cap.n);
        int st=0;if(pid>0)waitpid(pid,&st,0);
        goto done;
    }

    struct termios t;
    check(tcgetattr(slave,&t)==0,"read slave termios");
    errno=0;
    check(tcflow(slave,TCOOFF)<0&&errno==ENOTTY,"unsupported flow control stays ENOTTY");
    t.c_lflag&=~(tcflag_t)ECHO;
    check(tcsetattr(slave,TCSANOW,&t)==0,"disable echo request succeeds");
    tcflush(slave,TCOFLUSH);
    cap.n=0;
    check(write(master,"z",1)==1,"write first input byte");
    char echoed=0;errno=0;
    int echo_n=(int)read(master,&echoed,1);
    check(echo_n<0&&errno==EAGAIN,"echo disabled at line discipline");
    check(write(master,"ebra\n",5)==5,"write complete input line");
    check(wait_text(master,&cap,"PTY_CHILD_REPLY zebra"),"parent reads child reply from master");
    if(cap.n>0)write(1,cap.b,(size_t)cap.n);
    int status=-1;
    check(waitpid(pid,&status,0)==pid&&status==0,"child exits cleanly");

done:
    if(slave>=0)close(slave);if(master>=0)close(master);
    if(pipes[0]>=0)close(pipes[0]);if(pipes[1]>=0)close(pipes[1]);
    outs("PTY_CHECKS=");outn(checks);outs(" failures=");outn(failures);outc('\n');
    outs(failures?"PTY_RESULT FAIL\n":"PTY_RESULT PASS\n");
    return failures?1:0;
}
