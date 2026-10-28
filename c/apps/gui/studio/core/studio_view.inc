static void draw(void)
{
    uint64_t paint_start=trace_paint?monotonic_ns():0;paint_runs=paint_measures=0;
    int w=aui_width(),h=aui_height();sync_tree_selection();aui_begin(AUI_BG);
    if(dialog==1||dialog==4||dialog==5||dialog==6){draw_project_dialog();aui_end_rect(0,0,w,h);if(!dialog)draw();return;}
    if(w<680||h<420){aui_label(20,30,"放大 Code Studio 窗口以继续编辑",AUI_TEXT);aui_end_rect(0,0,w,h);return;}
    edit_x=196;edit_y=76;out_y=h-180;work_x=work_open&&w>=940?w-248:w;edit_w=work_x-edit_x;edit_h=out_y-edit_y;
    gui_rect(0,0,w,40,AUI_SURFACE);
    /* Aims (P13, ui_aim.h): every control here is hand-drawn or an aui_button (which publishes
     * none), so this app is the single emitter. Window-local points at each centre. */
    ui_aim_rect("open",8,6,58,28);ui_aim_rect("save",72,6,58,28);ui_aim_rect("check",136,6,64,28);
    ui_aim_rect("run",206,6,58,28);ui_aim_rect("find",334,6,58,28);
    ui_aim_rect("new-file",8,76,84,26);ui_aim_rect("new-folder",98,76,90,26);
    ui_aim_rect("editor",edit_x,edit_y,edit_w,ST_ROW);
    if(aui_button(8,6,58,28,"打开"))open_picker();
    if(aui_button(72,6,58,28,"存储"))save_current();
    if(aui_button(136,6,64,28,"检查"))start_job(1);
    if(aui_button(206,6,58,28,"运行"))start_job(0);
    if(aui_button(270,6,58,28,"停止"))st_engine_stop(engine);
    if(aui_button(334,6,58,28,"查找")){dialog=2;focus=1;}
    if(aui_button(398,6,68,28,"替换")){dialog=3;focus=1;}
    if(aui_button(w-80,6,72,28,"工作"))work_open=!work_open;
    aui_text_ellipsis(480,12,w-580,state->project,AUI_MUTED,14);
    gui_rect(0,41,edit_x-1,h-69,AUI_SURFACE);
    aui_text_sz(12,51,"项目",AUI_MUTED,12);
    if(aui_button(8,76,84,26,"新建文件"))new_entry(0);
    if(aui_button(98,76,90,26,"新建文件夹"))new_entry(1);
    for(int i=tree_top,y=ST_TREE_Y;i<state->tree_count&&y<h-48;i++,y+=ST_ROW){
        if(i==tree_selected||(document()&&!strcmp(state->files[i].path,document()->path)))gui_rect(4,y,edit_x-8,ST_ROW,AUI_SELECTION);
        ui_aim_i("tree-row",i,4+(edit_x-8)/2,y+ST_ROW/2);
        char label[80];snprintf(label,sizeof label,"%s%s",state->files[i].directory?(state->files[i].expanded?"v ":"> "):"",state->files[i].name);
        aui_text_ellipsis(12+state->files[i].depth*10,y+3,edit_x-22-state->files[i].depth*10,label,state->files[i].directory?AUI_MUTED:AUI_TEXT,14);
    }
    if(state->tree_limited)aui_text_sz(8,h-45,"已达到目录树上限，请使用“打开”",AUI_WARNING,11);
    gui_rect(edit_x,42,edit_w,30,AUI_SURFACE);
    int tabw=state->count?edit_w/state->count:180;if(tabw>180)tabw=180;
    for(int i=0;i<state->count;i++){int x=edit_x+i*tabw;char title[160];snprintf(title,sizeof title,"%s%s",st_dirty(&state->docs[i])?"* ":"",basename_(state->docs[i].path));
        ui_aim_i("tab",i,x+tabw/2,57);
        if(i==state->active){gui_rect(x,42,tabw,30,AUI_BG);gui_rect(x,70,tabw,2,AUI_ACCENT);}
        aui_text_ellipsis(x+9,50,tabw-32,title,AUI_TEXT,13);aui_text_sz(x+tabw-16,50,"x",AUI_MUTED,13);}
    if(document())draw_code(document());else{aui_heading(edit_x+32,120,"Code Studio",AUI_TEXT);aui_label(edit_x+32,159,"点按“打开”选择文件夹，或点按“新建文件”开始编辑。",AUI_MUTED);}
    gui_rect(edit_x,out_y,edit_w,h-out_y-28,AUI_SURFACE);gui_rect(edit_x,out_y,edit_w,1,AUI_BORDER);
    aui_text_sz(edit_x+12,out_y+10,state->runner.mode?"问题 / 语法检查":"程序输出",AUI_MUTED,12);
    if(state->runner.mode){for(int i=output_top,y=out_y+35;i<state->problem_count&&y<h-30;i++,y+=ST_ROW){char line[360];
            snprintf(line,sizeof line,"%s  %s:%d:%d  %s",state->problems[i].code,basename_(state->problems[i].path),state->problems[i].line,state->problems[i].column,state->problems[i].message);
            aui_text_ellipsis(edit_x+12,y,edit_w-24,line,AUI_ERROR,13);}}
    else if(state->runner.text){int p=0,row=0;while(row<output_top&&p<state->runner.length)if(state->runner.text[p++]=='\n')row++;
        for(int y=out_y+35;p<state->runner.length&&y<h-30;y+=ST_ROW){int end=p;while(end<state->runner.length&&state->runner.text[end]!='\n')end++;
            int n=end-p;if(n>4096)n=4096;char line[4097];memcpy(line,state->runner.text+p,(size_t)n);line[n]=0;
            aui_text_ellipsis(edit_x+12,y,edit_w-24,line,AUI_TEXT,13);p=end+1;}}
    if(work_x<w){gui_rect(work_x,41,w-work_x,h-69,AUI_SURFACE_2);aui_heading(work_x+16,58,"工作区",AUI_TEXT);
        aui_label(work_x+16,98,"编译器：/bin/as",AUI_TEXT);
        aui_text_sz(work_x+16,129,"本地语法检查与虚拟机运行",AUI_MUTED,12);
        aui_text_sz(work_x+16,154,"原生主机构建：不可用",AUI_MUTED,12);
        aui_label(work_x+16,195,"系统任务",AUI_TEXT);
        aui_text_sz(work_x+16,226,"把当前源代码分享给任务。",AUI_MUTED,12);
        if(aui_button(work_x+16,256,214,32,"分享当前代码")){struct logit_event e={0};e.type=EV_KEY;e.a=12;e.mods=EV_MOD_CTRL;ag_gui_event(&e);}
        aui_text_sz(work_x+16,318,"F5 运行    F6 检查    F8 停止",AUI_MUTED,12);
        aui_text_sz(work_x+16,345,"Ctrl+Z 撤销    Ctrl+Y 重做",AUI_MUTED,12);
        aui_text_sz(work_x+16,372,"Ctrl+Space 补全",AUI_MUTED,12);
    }
    draw_completion();
    if(dialog==2||dialog==3){int x=edit_x+12,wide=edit_w-24;gui_rect(x,90,wide,dialog==3?155:112,AUI_SURFACE_2);
        aui_text_sz(x+12,101,dialog==2?"在此文件中查找":"在此文件中替换",AUI_TEXT,14);
        aui_textfield(x+12,126,wide-104,query,sizeof query);
        if(aui_button(x+wide-80,126,68,30,"下一个")){find_next();}
        if(dialog==3){aui_textfield(x+12,170,wide-104,replacement,sizeof replacement);
            if(aui_button(x+wide-80,170,68,30,"替换")){st_engine_replace_found(engine,query,replacement);reveal(document());}}
        aui_text_sz(x+12,dialog==3?220:177,"按 Esc 返回编辑器",AUI_MUTED,11);
    }
    gui_rect(0,h-28,w,28,AUI_SURFACE_2);aui_text_ellipsis(12,h-20,w-240,state->notice,AUI_TEXT,12);
    if(document()){const StDocument *d=document();int row=st_caret_line(d),col=1;for(int p=st_line_start(d,row);p<d->caret;p=st_next(d->text,p,d->length))col++;
        char status[96];snprintf(status,sizeof status,"UTF-8   %d:%d   %d 字节",row+1,col,d->length);aui_text_sz(w-222,h-20,status,AUI_MUTED,12);}
    if(context_open){for(int i=0;i<3;i++)ui_aim_i("context",i,4+94,ST_TREE_Y+16+32*i);
        gui_rect(4,ST_TREE_Y,188,96,AUI_SURFACE_2);aui_label(14,ST_TREE_Y+8,"新建文件",AUI_TEXT);aui_label(14,ST_TREE_Y+40,"新建文件夹",AUI_TEXT);aui_label(14,ST_TREE_Y+72,"删除…",AUI_ERROR);}
    /* Raw gui_* source/selection/tree rectangles are not in aui.c's private
     * draw-call diff. The old aui_end() could flush only the changed status
     * label while leaving stale code/popup pixels in the compositor. Full UI
     * frames therefore report the full canvas; editor-only frames report their
     * own dirty rows below and retain the fast path. */
    aui_end_rect(0,0,w,h);
    if(dialog==1||dialog==4||dialog==5||dialog==6)draw();
    if(trace_paint)printf("STUDIO_PAINT us=%llu runs=%u measures=%u\n",(unsigned long long)((monotonic_ns()-paint_start)/1000),paint_runs,paint_measures);
}

/* Typing and caret motion do not rebuild buttons, the project tree or output.
 * Raw primitives intentionally avoid aui's per-frame widget bookkeeping; the
 * next full draw still owns that. Present the central region plus status bar. */
static void draw_editor(void)
{
    const StDocument *d=document();if(!d)return;
    uint64_t started=trace_paint?monotonic_ns():0;paint_runs=paint_measures=0;
    partial_code=1;draw_code(d);partial_code=0;
    int w=aui_width(),h=aui_height();
    gui_rect(0,h-28,w,28,AUI_SURFACE_2);
    gui_clip(12,h-28,w-252,28);gui_text_run(12,h-20,12,0,AUI_TEXT,state->notice,(int)strlen(state->notice));gui_clip(0,0,0,0);
    int row=st_caret_line(d),col=1;
    for(int p=st_line_start(d,row);p<d->caret;p=st_next(d->text,p,d->length))col++;
    char status[96];snprintf(status,sizeof status,"UTF-8   %d:%d   %d 字节",row+1,col,d->length);
    gui_text_run(w-222,h-20,12,0,AUI_MUTED,status,(int)strlen(status));
    /* The dirty marker can change on undo back to the saved bytes. Repaint the
     * active tab only; other tabs cannot change during an editor-only event. */
    int tabw=edit_w/state->count;if(tabw>180)tabw=180;
    int x=edit_x+state->active*tabw;gui_rect(x,42,tabw,28,AUI_BG);
    char title[160];snprintf(title,sizeof title,"%s%s",st_dirty(d)?"* ":"",basename_(d->path));
    gui_clip(x+9,42,tabw-32,28);gui_text_run(x+9,50,13,0,AUI_TEXT,title,(int)strlen(title));gui_clip(0,0,0,0);
    gui_text_run(x+tabw-16,50,13,0,AUI_MUTED,"x",1);
    gui_flush_rect(0,42,w,h-42);
    if(trace_paint)printf("STUDIO_EDIT us=%llu runs=%u measures=%u\n",(unsigned long long)((monotonic_ns()-started)/1000),paint_runs,paint_measures);
}
