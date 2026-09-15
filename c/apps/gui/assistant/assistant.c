/* SPDX-License-Identifier: MIT */
#include "aui.h"
#include "../../../lib/agent/sdk.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static struct ag_status status;
static char goal[AG_GOAL],output[AG_PATH]="/docs",message[192];
static uint64_t selected;
static uint64_t displayed_proposal,displayed_revision,displayed_task;
static uint64_t memory_revision;
static char memory_app[AEX_AGENT_ID_MAX];
static int refresh(void)
{
    struct ag_message m={.type=AG_STATUS};void *p=0;int r=ag_call(&m,0,&p);
    if(!r&&m.bytes==sizeof status){memcpy(&status,p,sizeof status);if(!selected&&status.count)selected=status.tasks[0].id;}
    else snprintf(message,sizeof message,"Task service unavailable (%d)",r);
    free(p);return r;
}
static struct ag_task *current(void)
{for(unsigned i=0;i<status.count;i++)if(status.tasks[i].id==selected)return &status.tasks[i];return 0;}
static void command(unsigned type,const void *p,unsigned len,uint64_t object)
{
    struct ag_task *t=current();struct ag_message m={.type=type,.task=selected,.bytes=len,.object=object,.revision=t?t->revision:0};void *out=0;
    /* Reviewed TextEdit proposals bind the actual comparison the user saw.
     * Legacy conflicts retain their old protocol and remain readable. */
    if(type==AG_ACCEPT_CANDIDATE&&displayed_task==selected&&displayed_proposal){
        m.type=AG_WORK_DECIDE;m.operation=displayed_proposal;m.revision=displayed_revision;}
    int r=ag_call(&m,p,&out);snprintf(message,sizeof message,r<0?"Action failed (%d); your task is retained.":"Action accepted (%d)",r);free(out);refresh();
}
static void diff_preview(struct ag_task *t)
{
    struct ag_message m={.type=AG_DOCUMENT,.task=t->id};void *before=0,*after=0;
    if(ag_call(&m,0,&before)<0)return;
    struct ag_message view={.type=AG_WORK_VIEW,.task=t->id};void *data=0;uint64_t proposal=0;
    if(!ag_call(&view,0,&data)&&view.bytes==sizeof(struct ag_work_view))proposal=((struct ag_work_view *)data)->proposal;
    free(data);m=(struct ag_message){.type=AG_DOCUMENT,.task=t->id,.object=1,.operation=proposal};
    if(ag_call(&m,0,&after)<0){free(before);return;}
    displayed_task=t->id;displayed_proposal=m.operation;displayed_revision=m.revision;
    size_t offset=0;char *a=before,*b=after;while(a[offset]&&b[offset]&&a[offset]==b[offset])offset++;
    while(offset&&((unsigned char)a[offset]&0xc0)==0x80)offset--;
    char line[160];snprintf(line,sizeof line,"Difference begins at byte %lu. Current / candidate:",(unsigned long)offset);
    aui_label(20,440,line,AUI_MUTED);
    char left[160],right[160];snprintf(left,sizeof left,"%.150s",a+offset);snprintf(right,sizeof right,"%.150s",b+offset);
    aui_label(20,462,left,AUI_TEXT);aui_label(20,484,right,AUI_ACCENT);free(before);free(after);
}
static void draw(void)
{
    aui_begin(AUI_BG);aui_label(20,16,"Logit Assistant",AUI_TEXT);
    struct ag_task *shown=current();char scope[160];
    if(status.context)snprintf(scope,sizeof scope,"Selected: %s",status.selection);
    else if(shown&&shown->object_count)snprintf(scope,sizeof scope,"%u source(s): %s",shown->object_count,shown->objects[0].path);
    else snprintf(scope,sizeof scope,"Select materials in Finder, or use Ask Logit in TextEdit.");
    aui_label(20,43,scope,AUI_MUTED);
    aui_label(20,73,"What should the applications do?",AUI_TEXT);
    int submit=aui_textfield(20,94,680,goal,sizeof goal);
    aui_label(20,126,"Save new reports in",AUI_MUTED);aui_textfield(170,120,350,output,sizeof output);
    if(aui_button(20,157,160,28,"Create report")||submit){
        struct ag_create req={0};req.context=status.context;snprintf(req.goal,sizeof req.goal,"%s",goal);snprintf(req.output,sizeof req.output,"%s",output);
        struct ag_message m={.type=AG_CREATE,.bytes=sizeof req};void *p=0;int r=ag_call(&m,&req,&p);
        if(!r){selected=m.task;goal[0]=0;snprintf(message,sizeof message,"Task %llu started.",(unsigned long long)selected);}
        else snprintf(message,sizeof message,"Cannot create task (%d): %.140s",r,p?(char *)p:"select supported materials first");free(p);refresh();}
    if(aui_button(194,157,150,28,"Revise selected")&&goal[0])command(AG_REVISE,goal,(unsigned)strlen(goal),0);
    if(aui_button(354,157,160,28,"Read app memory")){
        struct ag_task *t=current();const char *id=t&&t->app_id[0]?t->app_id:"os.logit.textedit";
        char *text=0;uint32_t bytes=0;uint64_t revision=0;int rc=ag_memory_read(id,&text,&bytes,&revision);
        if(!rc&&bytes<sizeof goal){if(bytes)memcpy(goal,text,bytes);goal[bytes]=0;memory_revision=revision;
            snprintf(memory_app,sizeof memory_app,"%s",id);snprintf(message,sizeof message,"Memory for %s. Edit above, then Remember text.",id);}
        else {memory_revision=0;snprintf(message,sizeof message,"Memory unavailable or too large for this field (%d). Use agentctl memory.",rc);}
        free(text);}
    if(aui_button(524,157,176,28,"Remember text")){
        if(!memory_revision)snprintf(message,sizeof message,"Read app memory before replacing it.");
        else {int rc=ag_memory_write(memory_app,goal,(unsigned)strlen(goal),&memory_revision,memory_revision);
            snprintf(message,sizeof message,rc<0?"Memory changed or could not be saved (%d). Read it again.":"App memory saved (%d). Other applications do not share it.",rc);}}
    aui_label(20,198,message,AUI_MUTED);
    for(unsigned i=0;i<status.count;i++){struct ag_task *t=&status.tasks[i];char label[120];
        snprintf(label,sizeof label,"%llu  %s  | model %u/%u",(unsigned long long)t->id,ag_phase_name(t->phase),t->calls,t->call_limit);
        if(aui_button(20,220+(int)i*23,680,21,label))selected=t->id;}
    struct ag_task *t=current();if(t){
        aui_label(20,418,t->reason,AUI_MUTED);
        if(t->phase==AG_CONFLICT)diff_preview(t);
        struct ag_control ctl;
        if(aui_button(20,510,95,26,"Pause")){ctl=(struct ag_control){AG_PAUSED,0};command(AG_CONTROL,&ctl,sizeof ctl,0);}
        if(aui_button(123,510,95,26,"Resume")){ctl=(struct ag_control){AG_QUEUED,0};command(AG_CONTROL,&ctl,sizeof ctl,0);}
        if(aui_button(226,510,115,26,"+32 calls")){ctl=(struct ag_control){AG_QUEUED,32};command(AG_CONTROL,&ctl,sizeof ctl,0);}
        if(aui_button(349,510,95,26,"Cancel")){ctl=(struct ag_control){AG_CANCELLED,0};command(AG_CONTROL,&ctl,sizeof ctl,0);}
        if(aui_button(452,510,120,26,"Open report")&&t->artifact[0])sys_open_path(t->artifact);
        if(t->phase==AG_CONFLICT){
            if(aui_button(20,548,200,26,"Keep current version"))command(AG_ACCEPT_CANDIDATE,0,0,0);
            if(aui_button(234,548,210,26,"Use candidate version"))command(AG_ACCEPT_CANDIDATE,0,0,1);}
    }
    aui_end();
}
int main(void)
{
    struct aex_agent_identity self;if(ag_self(&self)<0||self.mode!=AEX_ACT_UI)return 1;
    /* Keep the task controls above the dock on the 1280x800 baseline. */
    gui_create("Logit Assistant",720,580);aui_set_size(720,580);refresh();draw();
    unsigned long long next=monotonic_ms()+1500;
    for(;;){struct logit_event e;int changed=0;
        while(poll_event(&e)){if(e.type==EV_CLOSE)app_exit(0);aui_feed(&e);draw();aui_feed_done();changed=1;}
        if(monotonic_ms()>=next){refresh();changed=1;next=monotonic_ms()+1500;}
        if(changed)draw();wait_idle(250);
    }
}
