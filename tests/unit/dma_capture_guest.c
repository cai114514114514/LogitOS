/* SPDX-License-Identifier: MIT
 * A silent QEMU input backend must overwrite the driver's 0xa5 poison through
 * DMA. Guard bytes also detect a capture syscall writing outside its request.
 * Run with the dedicated DMA_CAPTURE_VERIFY image, never infer DMA from zeros
 * in an ordinary initially-zero ring. Two opens exercise normal close/restart. */
#include "clib.h"
static struct { unsigned char before[32], data[32768], after[32]; } capture;
static int check(int yes,const char *why) {
    if(!yes){outs("DMA_CAPTURE_FAIL ");outs(why);outs("\n");return 0;}return 1;
}
int main(void) {
    for(int run=0;run<2;run++) {
        struct logit_sndfmt fmt={0};
        for(unsigned i=0;i<sizeof capture;i++)((unsigned char *)&capture)[i]=0x5a;
        int h=snd_cap_open(&fmt);
        if(!check(h>=0,"open"))return 1;
        if(!check(fmt.rate==48000 && fmt.channels==2 && fmt.format==SND_FMT_S16,"format"))return 1;
        int got=snd_cap_read_all(h,capture.data,sizeof capture.data);
        if(!check(got==sizeof capture.data,"full capture read"))return 1;
        for(unsigned i=0;i<sizeof capture.data;i++)
            if(!check(capture.data[i]==0,"DMA silent input overwrote poison"))return 1;
        for(unsigned i=0;i<32;i++)
            if(!check(capture.before[i]==0x5a && capture.after[i]==0x5a,"user canaries"))return 1;
        struct logit_sndstate st={0};
        if(!check(snd_cap_state(h,&st)==0 && st.frames_played>=8192,"captured frames advance"))return 1;
        if(!check(snd_cap_close(h)==0,"close"))return 1;
        sys_sleep_ms(300); /* verification-only stop observer finishes in 120ms */
        outs("DMA_CAPTURE_ROUND_PASS ");outn(run);outs("\n");
    }
    struct logit_sndfmt removed={0};
    if(!check(snd_cap_open(&removed)==SND_E_NODEV,"unbound capture rejects reopen"))return 1;
    outs("DMA_CAPTURE_PASS\n");return 0;
}
