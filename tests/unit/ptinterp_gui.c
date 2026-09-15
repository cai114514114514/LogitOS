/* SPDX-License-Identifier: MIT */
#include "clib.h"
#include "elf.h"
extern char **environ;
__thread long gui_tls=82;
static long value=53;
long *volatile gui_pointer=&value;
int main(int argc,char **argv)
{
    int tiny=argc==1&&c_streq(argv[0],"/apps/ptinterp-tiny.aex");
    int bad=argc!=1||(!tiny&&!c_streq(argv[0],"/apps/ptinterp.aex"))||gui_tls!=82||gui_pointer!=&value;
    uint64_t *a=(void*)environ;while(*a)a++;a++;uint64_t base=0;
    for(;*a;a+=2)if(*a==AT_BASE)base=a[1];
    if(!base)bad++;
    if(_sys(SYS_GUI_CREATE,(long)"PT_INTERP",(320ul<<16)|120,0)<0)bad++;
    _sys(SYS_GUI_CLEAR,0x183143,0,0);
    _sys(SYS_GUI_TEXT,(20ul<<16)|30,0xffffff,(long)"Interpreter -> GUI: TLS OK");
    _sys(SYS_GUI_FLUSH,0,0,0);
    outs(bad?"PTINTERP_GUI_FAIL\n":tiny?"PTINTERP_GUI_TINY_PASS\n":"PTINTERP_GUI_PASS\n");
    sys_sleep_ms(500);
    return bad;
}
