/* SPDX-License-Identifier: MIT */
#include "internal.h"

void st_engine_start(StEngine *e,int check)
{
    StDocument *d=st_current(e);if(!d){e->check_pending=0;return;}if(st_runner_busy(&e->state.runner)){if(check)e->check_pending=1;else st_engine_notice(e,"Stop the running process before starting another.");return;}
    if(!check)for(int i=0;i<e->state.count;i++)if(st_dirty(&e->state.docs[i])){int r=st_save_document(&e->state.docs[i]);if(r<0){e->state.active=i;st_engine_notice(e,r==-2?"Run stopped: a file changed on disk.":"Run stopped: a document could not be saved.");return;}}
    if(st_runner_start(&e->state.runner,d,e->state.active,check,e->compiler,e->state.project)<0){st_engine_notice(e,"Could not start the compiler process.");e->check_pending=0;return;}
    e->runner_seen=0;e->state.completion_count=0;e->check_pending=0;
    st_engine_notice(e,check?"Checking the captured unsaved source...":"Running /bin/as...");
}
