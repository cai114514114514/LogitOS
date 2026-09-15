/* SPDX-License-Identifier: MIT */
/* Directory identity is the Project identity. This pane asks the service for
 * work owned by that directory; merely opening a Project creates no task. */
static struct ag_project_view project_view;
static char project_goal[AG_GOAL],project_path[AG_PATH];
static uint64_t project_polled;
static int project_rc=AG_E_NOTFOUND,project_selected;
static int project_refresh(int force)
{
    uint64_t now=monotonic_ms();
    if(!force&&!strcmp(project_path,cwd)&&now-project_polled<2000)return 0;
    project_polled=now;int changed=strcmp(project_path,cwd)!=0;
    if(changed){project_selected=0;memset(&project_view,0,sizeof project_view);}
    if(!changed&&project_view.id.object){char moved[AG_PATH];
        if(!ag_file_path(&project_view.id,moved,sizeof moved)&&strcmp(moved,cwd)){scpy(cwd,moved,sizeof cwd);changed=1;}
    }
    scpy(project_path,cwd,sizeof project_path);
    struct ag_message m={.type=AG_PROJECT_VIEW,.bytes=(unsigned)strlen(cwd)};void *p=0;
    int rc=ag_call(&m,cwd,&p);
    if(!rc&&(m.bytes!=sizeof project_view||((struct ag_project_view *)p)->count>AG_TASKS))rc=AG_E_VERSION;
    if(!rc){changed|=memcmp(&project_view,p,sizeof project_view)!=0;memcpy(&project_view,p,sizeof project_view);}
    changed|=rc!=project_rc;project_rc=rc;free(p);
    if(changed&&!rc)printf("PROJECT_VIEW path=%s tasks=%u first=%llu phase=%u\n",cwd,project_view.count,
        project_view.count?(unsigned long long)project_view.tasks[0].id:0,project_view.count?project_view.tasks[0].phase:0);
    return changed;
}
static void project_create(void)
{
    if(!project_goal[0])return;
    char names[AG_OBJECTS][AG_PATH];const char *paths[AG_OBJECTS];unsigned n=0;
    if(sel_count>AG_OBJECTS){scpy(agent_notice,"最多选择 32 份资料",sizeof agent_notice);return;}
    for(int i=0;i<sel_count;i++){int dir;if(row_path(sel[i],names[n],AG_PATH,&dir)<0)return;paths[n]=names[n];n++;}
    if(!n){paths[0]=cwd;n=1;}
    uint64_t context;int rc=ag_publish_selection(paths,n,cwd,&context);
    if(!rc){struct ag_create req={.context=context};scpy(req.goal,project_goal,sizeof req.goal);scpy(req.output,cwd,sizeof req.output);
        struct ag_message m={.type=AG_CREATE,.bytes=sizeof req};void *p=0;rc=ag_call(&m,&req,&p);free(p);}
    if(!rc){project_goal[0]=0;scpy(agent_notice,"已开始，报告保存在当前 Project",sizeof agent_notice);project_refresh(1);}
    else snprintf(agent_notice,sizeof agent_notice,"未能开始（%d），资料和要求已保留",rc);
}
static void project_control(unsigned phase)
{
    if(project_selected>=(int)project_view.count)return;
    struct ag_message m={.type=AG_CONTROL,.task=project_view.tasks[project_selected].id,.bytes=sizeof(struct ag_control)};
    struct ag_control q={.phase=phase};void *p=0;int rc=ag_call(&m,&q,&p);free(p);
    if(rc)snprintf(agent_notice,sizeof agent_notice,"操作未完成（%d）",rc);
    project_refresh(1);
}
static void project_line(int x,int y,const char *text,unsigned color)
{char label[256];fit_label(text,PROJECT_W-40,label,sizeof label);gui_text_run(x,y,13,0,color,label,slen(label));}
static void draw_project(void)
{
    int x=CONTENT_RIGHT+20,w=PROJECT_W-40;
    gui_rect(CONTENT_RIGHT,0,PROJECT_W,WINH,AUI_SURFACE);
    gui_rect(CONTENT_RIGHT,0,1,WINH,AUI_BORDER);
    aui_heading(x,22,"这个 Project 的工作",AUI_TEXT);
    project_line(x,54,cwd,AUI_MUTED);
    aui_label(x,88,sel_count?"使用选中的资料":"使用当前 Project 的资料",AUI_TEXT);
    if(aui_textfield_ex(x,114,w,project_goal,sizeof project_goal,"让应用做什么…",1))project_create();
    if(aui_button(x,150,w,28,"整理资料，生成报告"))project_create();
    if(project_rc){aui_label(x,204,"任务服务暂不可用",AUI_MUTED);return;}
    if(!project_view.count){aui_label(x,204,"还没有工作",AUI_TEXT);aui_label(x,232,"这里的文件可以照常打开、编辑",AUI_MUTED);return;}
    aui_label(x,200,"已有工作",AUI_MUTED);
    unsigned first=(unsigned)project_selected/4*4,last=first+4;if(last>project_view.count)last=project_view.count;
    if(first&&aui_button(x+w-60,196,26,24,"<")){project_selected-=4;return;}
    if(last<project_view.count&&aui_button(x+w-30,196,26,24,">")){project_selected=(int)last;return;}
    for(unsigned i=first;i<last;i++){
        char label[96];snprintf(label,sizeof label,"%llu · %s",(unsigned long long)project_view.tasks[i].id,ag_phase_name(project_view.tasks[i].phase));
        if(aui_button(x,228+(int)(i-first)*30,w,26,label))project_selected=(int)i;
    }
    if(project_selected>=(int)project_view.count)project_selected=0;
    struct ag_task *t=&project_view.tasks[project_selected];int y=238+(int)(last-first)*30;
    project_line(x,y,t->goal,AUI_TEXT);project_line(x,y+27,t->reason,AUI_MUTED);
    if(t->artifact[0]&&aui_button(x,y+58,w,28,"在 TextEdit 中打开报告"))sys_open_path(t->artifact);
    if(aui_button(x,y+98,w/2-4,26,t->phase==AG_PAUSED?"继续":"暂停"))project_control(t->phase==AG_PAUSED?AG_QUEUED:AG_PAUSED);
    if(aui_button(x+w/2+4,y+98,w/2-4,26,"取消工作"))project_control(AG_CANCELLED);
}
