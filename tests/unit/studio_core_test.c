/* SPDX-License-Identifier: MIT */
/* No window system, no GUI implementation, and no replacement engine. This
 * links the same app modules as studio.aex and drives their public commands. */
#include "../../c/apps/studio/engine.h"
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

static int checks;
#define CHECK(name,cond) do {if(!(cond)){fprintf(stderr,"FAIL %s at %d\n",name,__LINE__);exit(1);}checks++;} while(0)
typedef struct {uint64_t time;char values[10][128];} Host;
static uint64_t now(void *p){return ((Host *)p)->time;}
static int keyslot(const char *key)
{if(!strcmp(key,"app.studio.project"))return 0;if(!strcmp(key,"app.studio.active"))return 1;return 2+atoi(key+15);}
static int get(void *p,const char *key,char *out,int max)
{return snprintf(out,(size_t)max,"%s",((Host *)p)->values[keyslot(key)]);}
static void set(void *p,const char *key,const char *value)
{snprintf(((Host *)p)->values[keyslot(key)],128,"%s",value);}
static void put(const char *path,const char *s)
{FILE *f=fopen(path,"wb");CHECK("fixture-open",f!=NULL);CHECK("fixture-write",fwrite(s,1,strlen(s),f)==strlen(s));CHECK("fixture-close",fclose(f)==0);}
static void path(char out[128],const char *root,const char *name)
{CHECK("fixture-path",snprintf(out,128,"%s/%s",root,name)<128);}
static void replace(StEngine *e,const char *s)
{CHECK("select-all",st_engine_select(e,st_engine_document(e)->length,0)==0);CHECK("replace",st_engine_insert(e,s,(int)strlen(s))==0);}
static void settle(StEngine *e)
{
    for(int i=0;i<5000;i++){st_engine_tick(e);if(st_engine_state(e)->runner.finished)return;usleep(1000);}
    CHECK("child-deadline",0);
}
static int candidate(StEngine *e,const char *s)
{st_engine_complete(e);const StState *v=st_engine_state(e);for(int i=0;i<v->completion_count;i++)if(!strcmp(v->completions[i].label,s))return 1;return 0;}
int main(int argc,char **argv)
{
    CHECK("arguments",argc==3);signal(SIGPIPE,SIG_IGN);Host host={0};
    StHost api={.context=&host,.now=now,.get=get,.set=set,.compiler=argv[1],.modules=argv[2]};
    StEngine *e=st_engine_create(&api);CHECK("create",e!=NULL);
    char main[128],module[128],other[128];path(main,argv[2],"main.as");path(module,argv[2],"m.as");path(other,argv[2],"other.as");
    put(main,"print(42)\n");put(module,"value = 7\n");put(other,"x = 1\n");
    CHECK("project",st_engine_project(e,argv[2])==0);CHECK("tree",st_engine_state(e)->tree_count>=3);
    CHECK("open",st_engine_open(e,main)==0);replace(e,"中文\nabc");
    CHECK("utf8-boundary",st_engine_select(e,1,1)<0);
    CHECK("selection-preserved",st_engine_document(e)->caret==10);
    st_engine_move(e,ST_FIRST,1,0);st_engine_move(e,ST_RIGHT,1,0);
    CHECK("unicode-motion",st_engine_document(e)->caret==3);
    CHECK("delete-unicode",st_engine_delete(e,0)==0&&!strcmp(st_engine_document(e)->text,"文\nabc"));
    CHECK("undo-unicode",st_engine_undo(e,0)==1&&!strcmp(st_engine_document(e)->text,"中文\nabc"));
    CHECK("redo-unicode",st_engine_undo(e,1)==1&&!strcmp(st_engine_document(e)->text,"文\nabc"));
    st_engine_undo(e,0);st_engine_insert(e,"!",1);CHECK("redo-tail",st_engine_undo(e,1)==0);
    replace(e,"a a");CHECK("find",st_engine_find(e,"a")==0);
    st_engine_replace_found(e,"a","中");CHECK("replace-selection",!strcmp(st_engine_document(e)->text,"中 a"));
    CHECK("save",st_engine_save(e)==0);CHECK("clean",!st_dirty(st_engine_document(e)));
    put(main,"external\n");CHECK("conflict",st_engine_save(e)==-2);
    CHECK("close",st_engine_close(e,0)==0);CHECK("reopen",st_engine_open(e,main)==0);
    CHECK("clean-draft-external",!strcmp(st_engine_document(e)->text,"external\n"));
    replace(e,"unsaved 中文\n");CHECK("checkpoint-on-close",st_engine_close(e,0)==0);
    CHECK("recover-open",st_engine_open(e,main)==0);CHECK("draft-recovered",!strcmp(st_engine_document(e)->text,"unsaved 中文\n"));
    CHECK("save-recovered",st_engine_save(e)==0);
    CHECK("module-open",st_engine_open(e,module)==0);replace(e,"def unsaved_symbol():\n    return 7\n");
    st_engine_activate(e,0);replace(e,"import m\nm.");CHECK("unsaved-module-completion",candidate(e,"unsaved_symbol"));
    Host host2={0};StHost api2=api;api2.context=&host2;StEngine *e2=st_engine_create(&api2);CHECK("create-second",e2!=NULL);
    st_engine_project(e2,argv[2]);st_engine_open(e2,other);replace(e2,"import m\nm.");
    CHECK("isolated-module-context",!candidate(e2,"unsaved_symbol"));CHECK("first-context-retained",candidate(e,"unsaved_symbol"));
    st_engine_destroy(e2);
    replace(e,"x =\ny =\n");st_engine_start(e,1);settle(e);
    const StState *v=st_engine_state(e);CHECK("structured-check",v->problem_count==2&&WEXITSTATUS(v->runner.status)==1);
    CHECK("diagnostic-location",v->problems[1].line==2&&v->problems[1].column==4);
    CHECK("problem-navigation",st_engine_select_problem(e,1)==0&&st_engine_document(e)->caret==7);
    st_engine_insert(e,"1",1);CHECK("stale-navigation",st_engine_select_problem(e,0)<0);
    replace(e,"print(1)\n");st_engine_start(e,1);settle(e);CHECK("valid-check",v->problem_count==0);
    replace(e,"x =\n");st_engine_start(e,1);st_engine_insert(e," ",1);st_engine_undo(e,0);settle(e);
    CHECK("stale-diagnostics",v->problem_count==0&&strstr(v->notice,"older version"));
    replace(e,"print(42)\n");st_engine_start(e,0);settle(e);
    CHECK("real-run-output",!v->runner.mode&&WIFEXITED(v->runner.status)&&WEXITSTATUS(v->runner.status)==0&&strstr(v->runner.text,"42"));
    st_engine_tick(e);CHECK("run-output-retained",!v->runner.mode);
    /* The actual engine still launches `as FILE`. This source must go through
     * native build/run and preserve the program's nonzero exit, not the VM.
     * The pipe transfers the output through two native scoped owners before
     * a borrowed stdout view writes it to the real runner capture. */
    replace(e, "# aether: 3.0\n"
               "def main() -> i64:\n"
               "    unsafe:\n"
               "        storage = alloc(4)\n"
               "        pointer: Ptr[i32] = i32ptr(addr(storage))\n"
               "        pointer[0] = 7\n"
               "        assert pointer[0] == 7\n"
               "        dealloc(storage)\n"
               "    with owner = region(1):\n"
               "        with moved = owner.move():\n"
               "            with view = moved.borrow_mut(0, 1):\n"
               "                view[0] = 7\n"
               "            assert moved[0] == 7 and len(moved) == 1\n"
               "    command = run(\"echo\", \"studio native\") |> run(\"cat\")\n"
               "    with reader, writer = pipe():\n"
               "        assert port_stats()[\"open\"] == 2\n"
               "        writer.write(command.out())\n"
               "        writer.close()\n"
               "        output = reader.readall()\n"
               "        with console = port(1):\n"
               "            console.write(output)\n"
               "    assert port_stats()[\"open\"] == 0\n"
               "    return 7\n");
    st_engine_start(e, 1);
    settle(e);
    CHECK("native-check", v->problem_count == 0 && WEXITSTATUS(v->runner.status) == 0);
    st_engine_start(e, 0);
    settle(e);
    if (!WIFEXITED(v->runner.status) || WEXITSTATUS(v->runner.status) != 7 ||
        strcmp(v->runner.text, "studio native\n")) {
        fprintf(stderr, "native runner status=%d mode=%d output=%s\n",
                v->runner.status, v->runner.mode, v->runner.text);
    }
    CHECK("native-run", WIFEXITED(v->runner.status) && WEXITSTATUS(v->runner.status) == 7 &&
          !strcmp(v->runner.text, "studio native\n"));
    /* Ownership diagnostics use the same unsaved snapshot and navigation path
     * as ordinary type errors. A compiler-only check cannot prove this wiring. */
    replace(e, "# aether: 3.0\n"
               "def main() -> None:\n"
               "    with owner = region(1):\n"
               "        with moved = owner.move():\n"
               "            pass\n"
               "        owner[0] = 1\n");
    st_engine_start(e, 1);
    settle(e);
    CHECK("region-diagnostic", v->problem_count == 1 &&
          !strcmp(v->problems[0].code, "AS3402") && v->problems[0].line == 6);
    CHECK("region-navigation", st_engine_select_problem(e, 0) == 0 &&
          st_engine_document(e)->caret == v->problems[0].start);
    replace(e, "# aether: 3.0\n"
               "def main() -> None:\n"
               "    with owner = region(1):\n"
               "        with view = owner.borrow(0, 1):\n"
               "            owner[0] = 1\n");
    st_engine_start(e, 1);
    settle(e);
    CHECK("borrow-diagnostic", v->problem_count == 1 &&
          !strcmp(v->problems[0].code, "AS3403") && v->problems[0].line == 5);
    CHECK("borrow-navigation", st_engine_select_problem(e, 0) == 0 &&
          st_engine_document(e)->caret == v->problems[0].start);
    replace(e,"while true:\n    x = 1\n");st_engine_start(e,0);usleep(10000);st_engine_stop(e);settle(e);
    CHECK("cancel",v->runner.cancelled&&WIFSIGNALED(v->runner.status));
    st_engine_activate(e,1);CHECK("shutdown",st_engine_shutdown(e)==0);st_engine_destroy(e);
    e=st_engine_create(&api);st_engine_restore(e,NULL);v=st_engine_state(e);
    CHECK("session-tabs",v->count==2);CHECK("session-active",v->active==1);
    CHECK("session-source",strstr(st_engine_document(e)->text,"unsaved_symbol")!=NULL);
    st_engine_destroy(e);
    StHost project_api={.context=&host,.now=now,.compiler=argv[1],.modules=argv[2]};
    e=st_engine_create(&project_api);CHECK("project-create",st_engine_project(e,argv[2])==0);
    CHECK("new-folder",st_engine_create_entry(e,argv[2],"folder",1)==0);
    char folder[128];path(folder,argv[2],"folder");
    CHECK("new-file",st_engine_create_entry(e,folder,"新文件.as",0)==0);
    CHECK("new-file-opened",strstr(st_engine_document(e)->path,"新文件.as")!=NULL);
    CHECK("duplicate-refused",st_engine_create_entry(e,folder,"新文件.as",0)<0);
    CHECK("traversal-refused",st_engine_create_entry(e,folder,"../escape",0)<0);
    CHECK("empty-refused",st_engine_create_entry(e,folder,"",1)<0);
    v=st_engine_state(e);int index=-1;
    for(int i=0;i<v->tree_count;i++)if(!strcmp(v->files[i].path,folder))index=i;
    CHECK("folder-visible",index>=0&&v->files[index].expanded);
    int expanded_count=v->tree_count;
    CHECK("collapse",st_engine_toggle_folder(e,index)==0&&v->tree_count==expanded_count-1);
    CHECK("stable-project-root",!strcmp(v->project,argv[2]));
    st_engine_refresh(e);CHECK("collapse-refresh",!v->files[index].expanded);
    CHECK("expand",st_engine_toggle_folder(e,index)==0&&v->tree_count==expanded_count);
    StListing listing;CHECK("picker",st_list_directory(folder,&listing)==0&&listing.count==1);
    CHECK("picker-error",st_list_directory("/absent-studio-folder",&listing)<0);
    /* Typing conveniences must not change paste or take multiple undo steps. */
    replace(e,"");int undo=v->docs[v->active].undo_count;
    CHECK("pair-open",st_engine_type(e,'(')==0&&!strcmp(st_engine_document(e)->text,"()")&&st_engine_document(e)->caret==1);
    CHECK("pair-one-undo",v->docs[v->active].undo_count==undo+1);
    CHECK("pair-close-skip",st_engine_type(e,')')==0&&!strcmp(st_engine_document(e)->text,"()")&&st_engine_document(e)->caret==2);
    CHECK("pair-undo",st_engine_undo(e,0)==1&&st_engine_document(e)->length==0);
    st_engine_type(e,'[');CHECK("pair-delete",st_engine_backspace(e)==0&&st_engine_document(e)->length==0);
    replace(e,"中文");st_engine_select(e,6,0);st_engine_type(e,'"');
    CHECK("pair-wrap-utf8",!strcmp(st_engine_document(e)->text,"\"中文\"")&&st_engine_document(e)->anchor==1&&st_engine_document(e)->caret==7);
    CHECK("pair-wrap-undo",st_engine_undo(e,0)==1&&!strcmp(st_engine_document(e)->text,"中文")&&st_engine_document(e)->anchor==0&&st_engine_document(e)->caret==6);
    replace(e,"");st_engine_type(e,'"');st_engine_type(e,'a');st_engine_type(e,'"');
    CHECK("quote-skip",!strcmp(st_engine_document(e)->text,"\"a\"")&&st_engine_document(e)->caret==3);
    replace(e,"# comment ");st_engine_type(e,'(');CHECK("no-comment-pair",!strcmp(st_engine_document(e)->text,"# comment ("));
    replace(e,"\"hello");st_engine_type(e,'(');CHECK("no-string-pair",!strcmp(st_engine_document(e)->text,"\"hello("));
    replace(e,"\"hello\\");st_engine_type(e,'"');CHECK("escaped-quote",!strcmp(st_engine_document(e)->text,"\"hello\\\""));
    replace(e,"");st_engine_type(e,0x201c);CHECK("chinese-quote-pair",!strcmp(st_engine_document(e)->text,"“”")&&st_engine_document(e)->caret==3);
    st_engine_backspace(e);CHECK("chinese-quote-delete",st_engine_document(e)->length==0);
    st_engine_insert(e,"([{",3);CHECK("paste-unmodified",!strcmp(st_engine_document(e)->text,"([{"));
    replace(e,"a\nb\n");st_engine_viewport(e,1000,0);CHECK("viewport-clamp",st_engine_document(e)->top==2);
    char removed[128];strcpy(removed,st_engine_document(e)->path);
    CHECK("draft-before-delete",st_engine_close(e,v->active)==0&&st_engine_open(e,removed)==0);
    CHECK("nonempty-folder-retained",st_engine_remove_entry(e,folder)<0&&access(removed,F_OK)==0);
    CHECK("delete-open-file",st_engine_remove_entry(e,removed)==0&&access(removed,F_OK)<0&&v->count==0);
    CHECK("deleted-draft-not-restored",st_engine_open(e,removed)<0&&v->count==0);
    CHECK("deleted-file-folder-is-empty",st_engine_remove_entry(e,folder)==0);
    CHECK("recreate-folder",st_engine_create_entry(e,argv[2],"folder",1)==0);
    CHECK("recreate-deleted-name",st_engine_create_entry(e,folder,"新文件.as",0)==0&&st_engine_document(e)->length==0);
    CHECK("empty-folder-create",st_engine_create_entry(e,argv[2],"empty-delete",1)==0);
    char empty[128];path(empty,argv[2],"empty-delete");
    CHECK("empty-folder-delete",st_engine_remove_entry(e,empty)==0&&access(empty,F_OK)<0);
    st_engine_destroy(e);

    /* Missing session paths formerly became phantom empty tabs, then could
     * not close because their nonexistent parent could not hold a checkpoint. */
    Host ghosts={0};StHost ghost_api=api;ghost_api.context=&ghosts;
    char gone[128],gone_dir[128],clean[128];path(gone,argv[2],"gone/demo.as");
    path(gone_dir,argv[2],"gone");path(clean,argv[2],"clean.as");put(clean,"saved\n");
    e=st_engine_create(&ghost_api);st_engine_project(e,argv[2]);
    CHECK("missing-open-refused",st_engine_open(e,gone)<0&&st_engine_state(e)->count==0);
    set(&ghosts,"app.studio.tab.0",gone);set(&ghosts,"app.studio.tab.1",clean);set(&ghosts,"app.studio.active","1");
    st_engine_restore(e,NULL);
    CHECK("missing-session-pruned",st_engine_state(e)->count==1&&!strcmp(st_engine_document(e)->path,clean)&&!ghosts.values[3][0]);
    CHECK("missing-session-active",st_engine_state(e)->active==0);
    CHECK("bookmark-close",st_engine_close(e,0)==0);CHECK("remove-clean-source",unlink(clean)==0);
    CHECK("clean-bookmark-not-ghost",st_engine_open(e,clean)<0);
    set(&ghosts,"app.studio.tab.0",gone);st_engine_restore(e,NULL);
    CHECK("all-missing-session-cleared",st_engine_state(e)->count==0&&!ghosts.values[2][0]);
    CHECK("gone-directory",mkdir(gone_dir,0700)==0);put(gone,"saved\n");
    CHECK("open-before-external-delete",st_engine_open(e,gone)==0);
    CHECK("remove-open-parent",unlink(gone)==0&&rmdir(gone_dir)==0);
    CHECK("clean-missing-parent-close",st_engine_close(e,0)==0&&st_engine_state(e)->count==0);
    CHECK("recreate-gone-directory",mkdir(gone_dir,0700)==0);put(gone,"saved\n");st_engine_open(e,gone);
    replace(e,"recover me\n");CHECK("dirty-checkpoint",st_engine_close(e,0)==0);CHECK("remove-dirty-source",unlink(gone)==0);
    CHECK("missing-dirty-draft-retained",st_engine_open(e,gone)==0&&!strcmp(st_engine_document(e)->text,"recover me\n"));
    st_engine_destroy(e);printf("PASS %d engine checks (no GUI linked)\n",checks);return 0;
}
