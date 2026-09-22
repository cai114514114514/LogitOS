/* Retain already painted rows for editor-only frames. Hashes include styles,
 * selection, caret, horizontal scroll and geometry; full UI draws invalidate
 * them because aui_begin clears the canvas. No cached row may survive a dialog. */
static unsigned row_keys[128];
static int partial_code;
static unsigned mix_key(unsigned h,unsigned v){return (h^v)*16777619u;}
/* Colour contiguous runs, then clip pixels at the viewport. Offscreen text is
 * measured only up to the visible right edge; a very long line cannot send a
 * megabyte through the drawing syscall. Highlighting is cached by revision so
 * hovering, scrolling and moving the caret do not re-tokenize the document. */
static void draw_code(const StDocument *d)
{
    int coloured=prepare_ink(d);
    if(!partial_code)memset(row_keys,0,sizeof row_keys);
    int p=st_line_start(d,d->top),row=d->top;
    int lo=d->caret<d->anchor?d->caret:d->anchor,hi=d->caret>d->anchor?d->caret:d->anchor;
    const int left=edit_x+54,right=edit_x+edit_w-8;
    int screen_row=0;
    for(int y=edit_y;y+ST_ROW<=out_y;y+=ST_ROW,row++,screen_row++){
        int end=st_line_end(d,p);
        unsigned key=st_hash(d->text+p,(size_t)(end-p));
        if(coloured&&end>p)key=mix_key(key,st_hash(ink+p,(size_t)(end-p)));
        key=mix_key(key,end==d->length);
        key=mix_key(key,(unsigned)row);key=mix_key(key,(unsigned)d->left);key=mix_key(key,(unsigned)edit_w);
        key=mix_key(key,lo<end&&hi>p?(unsigned)(lo>p?lo-p:0)+1:0);
        key=mix_key(key,lo<end&&hi>p?(unsigned)(hi<end?hi-p:end-p)+1:0);
        key=mix_key(key,d->caret>=p&&d->caret<=end?(unsigned)(d->caret-p)+1:0);
        key=mix_key(key,(unsigned)focus);key=mix_key(key,(unsigned)state->problem_revision);
        if(!key)key=1;
        int same=partial_code&&screen_row<128&&row_keys[screen_row]==key;
        if(screen_row<128)row_keys[screen_row]=key;
        if(same){if(end==d->length)break;p=end+1;continue;}
        if(partial_code)gui_rect(edit_x,y,edit_w,ST_ROW,AUI_BG);
        char num[24];snprintf(num,sizeof num,"%d",row+1);aui_text_sz(edit_x+8,y+2,num,AUI_MUTED,13);
        int x=left-d->left,caret_x=-1;
        gui_clip(left,y,right-left,ST_ROW);
        for(int i=p;i<end&&x<right;){
            int kind=coloured?ink[i]:ST_INK_TEXT,next=i;
            int limit=run_end(d->text,i,end);
            while(next<limit&&(!coloured||ink[next]==kind))next=st_next(d->text,next,limit);
            int w=run_width(d->text+i,next-i);
            if(d->caret>=i&&d->caret<next)caret_x=x+line_width(d,i,d->caret);
            if(x+w>left){
                int a=lo>i?lo:i,b=hi<next?hi:next;
                if(a<b){int sx=x+line_width(d,i,a),ex=x+line_width(d,i,b);gui_rect(sx,y,ex-sx,ST_ROW,AUI_SELECTION);}
                paint_runs++;gui_text_run(x,y+2,16,1,ink_color(kind),d->text+i,next-i);
            }
            x+=w;i=next;
            if(i==end&&d->caret==end)caret_x=x;
        }
        if(p==end&&d->caret==end)caret_x=x;
        if(!focus&&caret_x>=left&&caret_x<right)gui_rect(caret_x,y+2,1,17,AUI_ACCENT);
        gui_clip(0,0,0,0);
        if(state->problem_tab==state->active&&state->problem_revision==d->revision)
            for(int i=0;i<state->problem_count;i++)if(!strcmp(state->problems[i].path,d->path)&&state->problems[i].start>=p&&state->problems[i].start<=end)gui_rect(edit_x+43,y+7,5,5,AUI_ERROR);
        if(end==d->length){
            if(partial_code&&y+ST_ROW<out_y)gui_rect(edit_x,y+ST_ROW,edit_w,out_y-y-ST_ROW,AUI_BG);
            for(int j=screen_row+1;j<128;j++)row_keys[j]=0;
            break;
        }p=end+1;
    }
}
