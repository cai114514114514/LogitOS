/* Pixel hit testing and keyboard bindings only. Edit semantics live in the
 * engine so mouse, keyboard and future command clients share one history. */
static void place_caret(int x,int y,int extend)
{
    const StDocument *d=document();if(!d)return;
    int row=d->top+(y-edit_y)/ST_ROW,p=st_line_start(d,row),end=st_line_end(d,p);
    int target=x-edit_x-54+d->left,width=0;
    while(p<end){int next=st_next(d->text,p,end),w=line_width(d,p,next);
        if(target<width+w/2)break;width+=w;p=next;}
    st_engine_select(engine,p,extend?d->anchor:p);focus=0;reveal(d);
}
static void move_caret(int key,unsigned mods)
{
    enum StMove move=ST_LEFT;
    switch(key){
    case KEY_LEFT:move=ST_LEFT;break;case KEY_RIGHT:move=ST_RIGHT;break;
    case KEY_HOME:move=(mods&EV_MOD_CTRL)?ST_FIRST:ST_HOME;break;
    case KEY_END:move=(mods&EV_MOD_CTRL)?ST_LAST:ST_END;break;
    case KEY_UP:case KEY_PGUP:move=ST_UP;break;
    case KEY_DOWN:case KEY_PGDN:move=ST_DOWN;break;
    default:return;
    }
    int rows=(key==KEY_PGUP||key==KEY_PGDN)?edit_h/ST_ROW:1;
    st_engine_move(engine,move,rows,mods&EV_MOD_SHIFT);reveal(document());
}
static void find_next(void){st_engine_find(engine,query);reveal(document());}
static void insert_text(const char *s,int n){st_engine_insert(engine,s,n);reveal(document());}
static void key_event(int key,unsigned mods)
{
    completion_due=0;
    if(key==19){save_current();return;}
    if(key==KEY_F5||key==18){start_job(0);return;}
    if(key==KEY_F6){start_job(1);return;}
    if(key==KEY_F8){st_engine_stop(engine);return;}
    if(key==15){open_picker();return;}
    if(key==14){new_entry(mods&EV_MOD_SHIFT);return;}
    if(key==6||(key==8&&(mods&EV_MOD_CTRL))){dialog=key==6?2:3;focus=1;return;}
    if(key==27){dialog=focus=0;st_engine_dismiss_completion(engine);return;}if(focus)return;
    const StDocument *d=document();if(!d)return;
    if(state->completion_count&&(key==KEY_UP||key==KEY_DOWN)){
        completion_selected=(completion_selected+(key==KEY_UP?state->completion_count-1:1))%state->completion_count;return;}
    if(state->completion_count&&key=='\t'){st_engine_accept_completion(engine,completion_selected);reveal(d);return;}
    if(key>=KEY_UP&&key<=KEY_RIGHT){move_caret(key,mods);return;}
    if(key>=KEY_F1&&key<=KEY_F12)return;
    if(key==1){st_engine_select(engine,d->length,0);return;}
    if(key==26||key==25){st_engine_undo(engine,key==25||(mods&EV_MOD_SHIFT));reveal(d);return;}
    int lo=d->caret<d->anchor?d->caret:d->anchor,hi=d->caret>d->anchor?d->caret:d->anchor;
    if(key==3||key==24){if(hi>lo){int r=clip_set(CLIP_F_TEXT,d->text+lo,hi-lo);
        if(r<0)set_notice("Clipboard rejected the selection");else if(key==24)insert_text("",0);}return;}
    if(key==22){char *clip=malloc(ST_DOCUMENT_LIMIT+1);if(!clip){set_notice("Paste allocation failed");return;}
        int expected=clip_len(CLIP_F_TEXT),n=clip_get(CLIP_F_TEXT,clip,ST_DOCUMENT_LIMIT+1);
        if(n>0&&n==expected&&n<=ST_DOCUMENT_LIMIT)insert_text(clip,n);else if(n!=expected)set_notice("Paste rejected: clipboard could not be read completely");
        free(clip);st_engine_dismiss_completion(engine);return;}
    if((key==0||key==' ')&&(mods&EV_MOD_CTRL)){complete();return;}
    if(key==8||key==127){if(key==8)st_engine_backspace(engine);else st_engine_delete(engine,1);reveal(d);return;}
    if(key=='\n'||key=='\r'){st_engine_newline(engine);reveal(d);return;}
    if(key=='\t'){insert_text("    ",4);return;}
    if(key>=32&&!(mods&(EV_MOD_CTRL|EV_MOD_ALT|EV_MOD_SUPER))){st_engine_type(engine,(unsigned)key);reveal(document());
        /* Completion scans symbols and can read modules. Do it once after a
         * typing pause, never in the character insertion path. Ctrl+Space is
         * still immediate. Any navigation/edit cancels the pending request. */
        if((key>='a'&&key<='z')||(key>='A'&&key<='Z')||key=='_'||key=='.')completion_due=monotonic_ms()+180;
        else st_engine_dismiss_completion(engine);}
}
