/* Product-kernel terminal behavior, using native descriptors plus the actual
 * libc termios/ioctl consumers. No host pseudo-terminal stands in for it. */
#include "clib.h"
#include "../../../include/abi/pty.h"
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
static int failures, checks;
static void check(int ok,const char *name)
{ checks++;if(!ok){failures++;outs("PTY_FAIL ");outs(name);outc('\n');} }
int main(int argc,char **argv)
{
    if(argc>1&&c_streq(argv[1],"attached")){
        struct termios t;struct winsize w;
        if(!isatty(0)||tcgetattr(0,&t)<0||ioctl(0,TIOCGWINSZ,&w)<0)return 1;
        outs("PTY_ATTACHED rows=");outn(w.ws_row);outs(" cols=");outn(w.ws_col);outc('\n');return 0;
    }
    int f[2];if(_sys(SYS_PTY_OPEN,(long)f,0,0)<0)return 1;
    sys_set_nonblock(f[0]);sys_set_nonblock(f[1]);
    struct termios t;check(isatty(f[1]),"slave is a terminal above fd 2");
    check(tcgetattr(f[1],&t)==0,"libc reads terminal attributes");
    struct termios original=t;
    if(argc>1){t.c_lflag&=~ECHO;tcsetattr(f[1],TCSANOW,&t);}
    char b[64];sys_write(f[0],"abc",3);
    check(sys_read(f[1],b,sizeof b)==LSK_E_AGAIN,"canonical input waits for newline");
    int n=sys_read(f[0],b,sizeof b);
    check(n==3&&b[0]=='a'&&b[1]=='b'&&b[2]=='c',"canonical echo");
    sys_write(f[0],"\n",1);n=sys_read(f[1],b,sizeof b);
    check(n==4&&b[3]=='\n',"completed line reaches slave");
    tcflush(f[1],TCIOFLUSH);
    cfmakeraw(&t);check(tcsetattr(f[1],TCSANOW,&t)==0,"libc sets raw mode");
    unsigned char raw[]={0,3,4,10,13,127};sys_write(f[0],raw,sizeof raw);n=sys_read(f[1],b,sizeof b);
    int same=n==(int)sizeof raw;for(int i=0;i<n&&i<(int)sizeof raw;i++)same&=(unsigned char)b[i]==raw[i];
    check(same,"raw input bytes unchanged");check(sys_read(f[0],b,sizeof b)==LSK_E_AGAIN,"raw mode disables echo");
    sys_write(f[1],raw,sizeof raw);n=sys_read(f[0],b,sizeof b);same=n==(int)sizeof raw;
    for(int i=0;i<n&&i<(int)sizeof raw;i++)same&=(unsigned char)b[i]==raw[i];check(same,"raw output bytes unchanged");
    struct winsize w={41,119,0,0},got;
    check(ioctl(f[0],TIOCSWINSZ,&w)==0&&ioctl(f[1],TIOCGWINSZ,&got)==0&&got.ws_row==41&&got.ws_col==119,"window size crosses endpoints");
    check(_sys(SYS_PTY_CTL,f[0],LPTY_SIGNAL,0)==LOGIT_SIGWINCH,"window change announces signal");
    tcsetattr(f[1],TCSANOW,&original);unsigned char intr=3;sys_write(f[0],&intr,1);
    check(_sys(SYS_PTY_CTL,f[0],LPTY_SIGNAL,0)==LOGIT_SIGINT,"line discipline emits interrupt event");
    sys_close(f[1]);check(sys_read(f[0],b,sizeof b)==0,"last slave close reaches master EOF");sys_close(f[0]);
    outs("PTY_CHECKS=");outn(checks);outs(" failures=");outn(failures);outc('\n');return failures?1:0;
}
