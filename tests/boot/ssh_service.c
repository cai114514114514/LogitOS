/* Test-owned secondary daemon. Cleanup signals only the PID this launcher
 * created, then reaps it; no process-name matching or unrelated listener kill. */
#include "clib.h"
#include "logit_stat.h"
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    int pid=sys_fork();if(pid<0)return 3;
    if(!pid){char *args[]={"sshd","8081",argv[1],argv[2],0};char *env[]={"PATH=/bin",0};
        sys_execve("/bin/sshd",args,env);return 4;}
    struct logit_stat s;
    while(st_lstat("/state/ssh-service.stop",&s)<0)sys_sleep_ms(20);
    _sys(SYS_KILL,pid,LOGIT_SIGKILL,LOGIT_KILL_SIGNAL);
    int status;while(_sys(SYS_WAITPID,pid,(long)&status,0)==SIG_E_INTR){}
    outs("SSH_SERVICE_STOPPED\n");return 0;
}
