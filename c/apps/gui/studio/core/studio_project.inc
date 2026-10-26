/* Native project commands and a directory chooser. The picker lists actual
 * filesystem entries; selecting a folder never requires typing its path. */
#define ST_TREE_Y 108
static StListing picker;
static int picker_top,tree_selected=-1,context_open;
static char create_parent[128],entry_name[64],entry_error[256],tree_path[128],delete_path[128];
static void select_tree(int index)
{tree_selected=index;snprintf(tree_path,sizeof tree_path,"%s",index>=0&&index<state->tree_count?state->files[index].path:"");}
static void sync_tree_selection(void)
{tree_selected=-1;for(int i=0;i<state->tree_count;i++)if(!strcmp(tree_path,state->files[i].path)){tree_selected=i;break;}}
static void parent_path(char *path)
{char *slash=strrchr(path,'/');if(slash){if(slash==path)slash[1]=0;else *slash=0;}}
static void browse(const char *path)
{
    char copy[128];snprintf(copy,sizeof copy,"%s",path);
    if(st_list_directory(copy,&picker)<0){set_notice("无法读取此文件夹");return;}
    picker_top=0;
}
static void open_picker(void)
{browse(state->project);dialog=1;focus=1;context_open=0;completion_due=0;st_engine_dismiss_completion(engine);aui_set_focus(0);}
static void new_entry(int folder)
{
    sync_tree_selection();
    snprintf(create_parent,sizeof create_parent,"%s",state->project);
    if(tree_selected>=0&&tree_selected<state->tree_count){
        snprintf(create_parent,sizeof create_parent,"%s",state->files[tree_selected].path);
        if(!state->files[tree_selected].directory)parent_path(create_parent);
    }
    entry_error[0]=entry_name[0]=0;dialog=folder?5:4;focus=1;context_open=0;completion_due=0;aui_set_focus(0);
}
static void create_entry(void)
{
    reset_ink();if(st_engine_create_entry(engine,create_parent,entry_name,dialog==5)==0){snprintf(tree_path,sizeof tree_path,"%s%s%s",create_parent,strcmp(create_parent,"/")?"/":"",entry_name);
        sync_tree_selection();dialog=focus=0;}else snprintf(entry_error,sizeof entry_error,"%s",state->notice);
}
static void delete_entry_dialog(void)
{
    sync_tree_selection();context_open=0;
    if(tree_selected<0){set_notice("请选择要删除的文件或文件夹");return;}
    snprintf(delete_path,sizeof delete_path,"%s",tree_path);
    entry_error[0]=0;dialog=6;focus=1;completion_due=0;st_engine_dismiss_completion(engine);aui_set_focus(0);
}
static void draw_project_dialog(void)
{
    if(dialog==6){
        if(aui_dialog_begin("删除文件或空文件夹",480,210)){
            aui_text_ellipsis(16,14,448,delete_path,AUI_TEXT,13);
            aui_text_sz(16,48,"删除此项目及其恢复草稿。",AUI_WARNING,13);
            aui_text_sz(16,72,"未存储的更改将被放弃。文件夹必须为空。",AUI_MUTED,12);
            aui_text_ellipsis(16,110,448,entry_error,AUI_ERROR,12);
            const char *labels[]={"取消","删除"};int choice=aui_dialog_buttons(labels,2);
            if(choice==0)dialog=focus=0;
            else if(choice==1){
                reset_ink();
                if(st_engine_remove_entry(engine,delete_path)==0){select_tree(-1);dialog=focus=0;}
                else snprintf(entry_error,sizeof entry_error,"%s",state->notice);
            }
            aui_dialog_end();
        }
    }else if(dialog==1){
        int height=aui_height()-180;if(height>450)height=450;
        if(aui_dialog_begin("打开文件或文件夹",540,height)){
            if(aui_button(16,12,66,28,"上一级")){char p[128];strcpy(p,picker.path);parent_path(p);browse(p);}
            if(aui_button(90,12,70,28,"根目录"))browse("/");
            if(aui_button(168,12,90,28,"Documents"))browse("/docs");
            if(aui_button(266,12,70,28,"Home"))browse("/home");
            aui_text_ellipsis(16,52,504,picker.path,AUI_TEXT,14);
            int rows=(height-154)/28;
            for(int j=0;j<rows&&picker_top+j<picker.count;j++){
                StFile *f=&picker.files[picker_top+j];char label[80];snprintf(label,sizeof label,"%s  %s",f->directory?">":" ",f->name);
                if(aui_button(16,80+j*28,504,26,label)){
                    char path[128];strcpy(path,f->path);
                    if(f->directory)browse(path);else if(open_document(path)==0){dialog=focus=0;}
                    break;
                }
            }
            if(!picker.count)aui_label(24,100,"此文件夹是空的",AUI_MUTED);
            if(picker.limited)aui_text_sz(16,height-68,"已达到目录列表上限",AUI_WARNING,12);
            const char *labels[]={"打开文件夹","取消"};int choice=aui_dialog_buttons(labels,2);
            if(choice==0){if(st_engine_project(engine,picker.path)==0){dialog=focus=0;tree_top=0;select_tree(-1);}}
            else if(choice==1)dialog=focus=0;
            aui_dialog_end();
        }
    }else if(dialog==4||dialog==5){
        if(aui_dialog_begin(dialog==5?"新建文件夹":"新建文件",480,190)){
            aui_text_ellipsis(16,14,448,create_parent,AUI_MUTED,13);
            aui_label(16,44,"名称",AUI_TEXT);
            int submitted=aui_textfield(16,70,448,entry_name,sizeof entry_name);
            /* The new dialog's field is widget 1 after the frame reset below.
             * Focus once so the user can immediately type a name. */
            if(!aui_focus_id())aui_set_focus(1);
            aui_text_ellipsis(16,108,448,entry_error,AUI_ERROR,12);
            const char *labels[]={"创建","取消"};int choice=aui_dialog_buttons(labels,2);
            if(choice==0||submitted)create_entry();else if(choice==1)dialog=focus=0;
            aui_dialog_end();
        }
    }
}
