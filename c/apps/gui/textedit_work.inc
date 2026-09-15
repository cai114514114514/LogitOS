/* SPDX-License-Identifier: MIT */
/* Native document workspace. No model key or global "latest selection" is
 * used here: every action carries this window's authenticated task binding. */
static struct ag_work_view work;
static char instruction[AG_GOAL],work_error[192];
static char *proposal_text,*source_text;
static uint64_t proposal_id;
static unsigned proposal_len,source_len,source_offset;
static int source_open,source_index,work_available,source_list;
static struct aui_rect body;
static int layout_bottom;
struct te_row {int start,length,y,px,height;};
struct te_reference_hit {struct aui_rect rect;unsigned object,offset;};
static struct te_reference_hit reference_hits[64];static unsigned reference_count;
static int raw_painted;
static unsigned review_ink(void)
{
    /* The success token is designed for indicators. Mix it with normal text
     * for long passages on paper; the bright token alone is hard to read. */
    if(aui_is_dark())return AUI_SUCCESS;
    unsigned a=AUI_SUCCESS,b=AUI_TEXT;
    return rgb((((a>>16)&255)+2*((b>>16)&255))/3,(((a>>8)&255)+2*((b>>8)&255))/3,((a&255)+2*(b&255))/3);
}
static struct te_row *rows_cache;
static int row_count,row_capacity,cache_width=-1,cache_font,cache_mono,cache_style;
static const char *cache_text;
static unsigned cache_epoch;
static const char *phase_label(unsigned p)
{
    switch(p){case AG_QUEUED:return "准备继续";case AG_RUNNING:return "正在处理";
    case AG_WAIT_MODEL:return "等待模型连接";case AG_PAUSED:return "已暂停";
    case AG_CANCELLED:return "已取消";case AG_DONE:return "本轮工作已完成";
    case AG_CONFLICT:return "等待你审阅";case AG_WAIT_BUDGET:return "等待延长预算";
    default:return "需要核对保存状态";}
}
static void work_refresh(int enable)
{
    if(!agent_task)return;
    struct ag_message m={.type=AG_WORK_VIEW,.task=agent_task,.object=enable?1:0};void *p=0;
    int rc=ag_call(&m,0,&p);work_available=!rc&&m.bytes==sizeof work;
    if(work_available){memcpy(&work,p,sizeof work);
        if(work.proposal&&work.proposal!=proposal_id){
            struct ag_message q={.type=AG_DOCUMENT,.task=agent_task,.object=1,.operation=work.proposal};void *b=0;
            if(!ag_call(&q,0,&b)&&q.bytes<=MAXT&&q.operation==work.proposal){
                free(proposal_text);proposal_text=b;proposal_len=q.bytes;proposal_id=q.operation;b=0;cache_width=-1;
            }free(b);
        }
        if(!work.proposal){free(proposal_text);proposal_text=0;proposal_id=0;proposal_len=0;view_mode=0;}
    }else snprintf(work_error,sizeof work_error,"任务服务暂不可用（%d），正文仍可编辑",rc);
    free(p);
}
static void ask_logit(void)
{sidebar=1;editor_focus=0;work_refresh(1);}
static void work_submit(void)
{
    if(!instruction[0]||load_refused)return;agent_sync();
    if(agent_task&&agent_dirty)return;
    struct ag_message m={0};void *p=0;int rc;
    if(!agent_task){
        uint64_t context;rc=ag_publish_document(fname,text,(unsigned)tlen,agent_revision,&context);
        if(!rc){struct ag_create req={0};req.context=context;snprintf(req.goal,sizeof req.goal,"%s",instruction);
            if(ag_parent_path(fname,req.output,sizeof req.output)<0||!strcmp(req.output,"/"))snprintf(req.output,sizeof req.output,"/docs");m=(struct ag_message){.type=AG_CREATE,.object=1,.bytes=sizeof req};
            rc=ag_call(&m,&req,&p);if(!rc){agent_task=m.task;agent_revision=1;agent_dirty=0;}}
    }else {m=(struct ag_message){.type=AG_REVISE,.task=agent_task,.revision=agent_revision,.bytes=(unsigned)strlen(instruction)};
        rc=ag_call(&m,instruction,&p);}
    free(p);if(!rc){instruction[0]=0;work_error[0]=0;view_mode=0;work_refresh(1);}
    else snprintf(work_error,sizeof work_error,"未能开始（%d），要求和正文已保留",rc);
}
static void work_control(unsigned phase,unsigned extend)
{
    struct ag_control q={phase,extend};struct ag_message m={.type=AG_CONTROL,.task=agent_task,.bytes=sizeof q};void *p=0;
    int r=ag_call(&m,&q,&p);free(p);if(r<0)snprintf(work_error,sizeof work_error,"操作未完成（%d），工作进度已保留",r);
    else work_error[0]=0;work_refresh(0);
}
static void work_decide(int apply)
{
    /* Do not silently publish a locally edited revision then approve against
     * a different comparison. The user sees that new comparison first. */
    if(agent_dirty){agent_sync();work_refresh(0);agent_notice="正文已更新，请重新审阅后应用";return;}
    struct ag_message m={.type=AG_WORK_DECIDE,.task=agent_task,.revision=agent_revision,
        .operation=proposal_id,.object=(unsigned)apply};void *p=0;
    int rc=ag_call(&m,0,&p);free(p);
    if(rc<0){snprintf(work_error,sizeof work_error,"版本已变化或提交未完成（%d），请重新审阅",rc);work_refresh(0);return;}
    work_error[0]=0;agent_notice=apply?"修改已应用，新的报告版本已保存":"已保留当前正文";
    if(apply){m=(struct ag_message){.type=AG_DOCUMENT,.task=agent_task};p=0;
        if(!ag_call(&m,0,&p)&&m.bytes<=MAXT){memcpy(text,p,m.bytes);tlen=m.bytes;text[tlen]=0;
            agent_revision=m.revision;caret=anchor=tlen;edit_epoch++;saved=1;undo_valid=0;}free(p);}
    work_refresh(0);view_mode=0;scroll=0;follow_caret=0;
}
static void work_source(int index,unsigned offset)
{
    if(index<0||(unsigned)index>=work.task.object_count)return;
    const struct ag_object *o=&work.task.objects[index];struct ag_slice q={offset,16384};
    struct ag_message m={.type=AG_WORK_SOURCE,.task=agent_task,.object=o->id,.revision=o->revision,.bytes=sizeof q};void *p=0;
    int r=ag_call(&m,&q,&p);
    if(!r){free(source_text);source_text=p;source_len=m.bytes;source_open=1;source_index=index;source_offset=offset;
        editor_focus=0;scroll=0;follow_caret=0;cache_width=-1;
        printf("TEXTEDIT_SOURCE task=%llu object=%llu revision=%llu offset=%u bytes=%u\n",(unsigned long long)agent_task,(unsigned long long)o->id,(unsigned long long)o->revision,offset,m.bytes);}
    else {free(p);snprintf(work_error,sizeof work_error,"来源不可读取（%d）",r);}
}
static int work_citation_click(int x,int y)
{
    if(source_list)return 0;
    for(unsigned i=0;i<reference_count;i++){struct te_reference_hit *h=&reference_hits[i];
        if(x>=h->rect.x&&x<h->rect.x+h->rect.w&&y>=h->rect.y&&y<h->rect.y+h->rect.h){work_source((int)h->object,h->offset);return 1;}}
    return 0;
}
static void work_markup(const char *mark)
{
    int n=(int)strlen(mark),lo=caret<anchor?caret:anchor,hi=caret>anchor?caret:anchor;
    if(tlen>MAXT-2*n){agent_notice="文档已达到 1 MiB";return;}
    char *b=malloc((size_t)(hi-lo+2*n+1));if(!b)return;
    memcpy(b,mark,n);memcpy(b+n,text+lo,hi-lo);memcpy(b+n+hi-lo,mark,n);
    if(replace_selection(b,hi-lo+2*n)){caret-=n;anchor=lo+n;}free(b);editor_focus=1;
}
static int work_body_hit(int x,int y)
{return !source_open&&!view_mode&&x>=body.x&&x<body.x+body.w&&y>=body.y&&y<body.y+body.h;}
static int row_add(int start,int length,int y,int px,int height)
{
    if(row_count==row_capacity){int cap=row_capacity?row_capacity*2:256;if(cap>MAXT+1)cap=MAXT+1;
        if(cap<=row_capacity)return 0;void *p=realloc(rows_cache,(size_t)cap*sizeof *rows_cache);if(!p)return 0;
        rows_cache=p;row_capacity=cap;}
    rows_cache[row_count++]=(struct te_row){start,length,y,px,height};return 1;
}
static void layout_rows(const char *s,int n,int width,int styled)
{
    if(cache_text==s&&cache_epoch==edit_epoch&&cache_width==width&&cache_font==te_font&&cache_mono==te_mono&&cache_style==styled)return;
    cache_text=s;cache_epoch=edit_epoch;cache_width=width;cache_font=te_font;cache_mono=te_mono;cache_style=styled;
    row_count=0;int y=0,start=0;char *old=text;int oldlen=tlen;text=(char *)s;tlen=n;
    for(;;){int end=start;while(end<n&&s[end]!='\n')end++;
        int px=te_font,off=start;
        if(styled&&off+2<=end&&s[off]=='#'&&s[off+1]==' '){px=te_font+12;off+=2;y+=10;}
        else if(styled&&off+3<=end&&s[off]=='#'&&s[off+1]=='#'&&s[off+2]==' '){px=te_font+5;off+=3;y+=8;}
        te_weight=styled?LOGIT_FACE_BOLD:0;
        do {int limit=end;if(limit-off>1000){limit=off+1000;while(limit>off&&te_is_cont(s[limit]))limit--;}
            int fit=te_fit(off,limit,width,px),h=px+10;
            /* Keep a source token whole when possible. Splitting '[S1:' from
             * its range would expose transport syntax and lose the link. */
            if(styled&&off+fit<end){int open=-1;
                for(int k=off;k<off+fit;k++){if(s[k]=='['&&k+1<end&&s[k+1]=='S')open=k;if(s[k]==']')open=-1;}
                if(open>off)fit=open-off;
            }
            if(!row_add(off,fit,y,px,h)){agent_notice="排版内存不足，文档内容仍然完整";cache_width=-1;goto done;}
            y+=h;off+=fit;
        }while(off<end);
        if(end==n)break;start=end+1;y+=3;
    }
done:layout_bottom=y;text=old;tlen=oldlen;te_weight=0;
}
static void work_place_caret(int x,int y,int mods)
{
    /* Switch styled headings to editable Markdown before hit testing, so a
     * hidden '#' cannot move the insertion point into another code point. */
    layout_rows(text,tlen,body.w,0);int target=y-body.y+scroll,row=row_count-1;
    for(int i=0;i<row_count;i++)if(target<rows_cache[i].y+rows_cache[i].height){row=i;break;}
    if(row<0)return;struct te_row *r=&rows_cache[row];int p=r->start,end=p+r->length;
    while(p<end){int next=ted_next(text,p,end);int w=text_measure_px(text+r->start,next-r->start,r->px,te_mono);
        if(w>x-body.x)break;p=next;}
    caret=p;if(!(mods&EV_MOD_SHIFT))anchor=caret;
}
/* Citation labels are a view of the original [S:id-span] bytes, never a
 * rewrite of the document. Click targets retain the authorized object and
 * byte offset, so a pretty label cannot invent or widen a source grant. */
static void draw_styled_run(int x,int y,const char *s,int n,int px,int heading,unsigned color)
{
    raw_painted=1;int i=0,bold=heading;
    while(i<n){
        if(i+1<n&&s[i]=='*'&&s[i+1]=='*'){bold=!bold;i+=2;continue;}
        if(i+2<n&&s[i]=='['&&s[i+1]=='S'){
            int end=i+2;while(end<n&&s[end]!=']'&&end-i<120)end++;
            if(end<n&&s[end]==']'){
                char token[128];memcpy(token,s+i+2,(size_t)(end-i-2));token[end-i-2]=0;
                char *at=0;unsigned long long id=strtoull(token,&at,10);
                if(at&&*at==':'){
                    char *tail=0;unsigned long off=strtoul(at+1,&tail,10);
                    for(unsigned j=0;j<work.task.object_count;j++){struct ag_object *o=&work.task.objects[j];
                        if(id!=o->id||!tail||*tail!='-'||off>=o->bytes)continue;
                        char label[48];int len=snprintf(label,sizeof label,"[来源 %llu]",id);
                        int width=text_measure_px(label,len,px,0);
                        gui_text_run(x,y,px,0,AUI_ACCENT,label,len);
                        if(reference_count<64)reference_hits[reference_count++]=(struct te_reference_hit){{x,y,width,px+4},j,(unsigned)off};
                        x+=width;i=end+1;goto next;
                    }
                }
            }
        }
        int end=i+1;
        while(end<n&&!(end+1<n&&((s[end]=='*'&&s[end+1]=='*')||(s[end]=='['&&s[end+1]=='S'))))end++;
        gui_text_run_w(x,y,px,te_mono,color,s+i,end-i,bold);
        x+=text_measure_px(s+i,end-i,px,te_mono|(bold?LOGIT_FACE_BOLD:0));i=end;
next:;
    }
}
static void draw_document(void)
{
    const char *s=source_open?source_text:view_mode&&proposal_text?proposal_text:text;
    int n=source_open?(int)source_len:view_mode?(int)proposal_len:tlen;
    if(!s)s="";int styled=!editor_focus||source_open||view_mode;
    layout_rows(s,n,body.w,styled);
    if(follow_caret&&editor_focus&&!source_open&&!view_mode){
        for(int i=0;i<row_count;i++){struct te_row *r=&rows_cache[i];if(caret>=r->start&&caret<=r->start+r->length){
            if(r->y<scroll)scroll=r->y;if(r->y+r->height>scroll+body.h)scroll=r->y+r->height-body.h;break;}}
        follow_caret=0;}
    int max=layout_bottom-body.h;if(max<0)max=0;scroll=clampi(scroll,0,max);
    int prefix=0,aend=0,bend=0;
    if(work.proposal&&proposal_text)ted_difference(text,tlen,proposal_text,(int)proposal_len,&prefix,&aend,&bend);
    int lo=caret<anchor?caret:anchor,hi=caret>anchor?caret:anchor;
    for(int i=0;i<row_count;i++){struct te_row *r=&rows_cache[i];int y=body.y+r->y-scroll;
        if(y<body.y||y+r->px>body.y+body.h)continue;
        int end=r->start+r->length;
        if(!source_open&&work.proposal&&end>prefix&&r->start<(view_mode?bend:aend)){
            aui_round(body.x-8,y-2,body.w+16,r->height,3,AUI_SURFACE_2);
            if(view_mode)aui_fill_a(body.x-8,y-2,body.w+16,r->height,AUI_SUCCESS,18);
            aui_fill(body.x-9,y-2,3,r->height,view_mode?AUI_SUCCESS:AUI_WARNING);
        }
        if(editor_focus&&!source_open&&!view_mode&&hi>r->start&&lo<end){
            int from=lo>r->start?lo:r->start,to=hi<end?hi:end;
            int x1=from>r->start?text_measure_px(s+r->start,from-r->start,r->px,te_mono):0;
            int x2=to>r->start?text_measure_px(s+r->start,to-r->start,r->px,te_mono):x1;
            aui_fill(body.x+x1,y,x2-x1,r->height,AUI_FACE);
        }
        if(styled)draw_styled_run(body.x,y,s+r->start,r->length,r->px,r->px>te_font,view_mode&&end>prefix&&r->start<bend?review_ink():AUI_TEXT);
        else if(te_mono)gui_text_run(body.x,y,r->px,1,AUI_TEXT,s+r->start,r->length);
        else aui_text_n(body.x,y,s+r->start,r->length,view_mode&&end>prefix&&r->start<bend?review_ink():AUI_TEXT,r->px);
        if(editor_focus&&!view_mode&&!source_open&&caret>=r->start&&caret<=end){
            int x=caret>r->start?text_measure_px(s+r->start,caret-r->start,r->px,te_mono):0;
            aui_fill(body.x+x,y,2,r->px,AUI_ACCENT);
        }
    }
}
static void side_text(int x,int y,int width,const char *s,unsigned color,int px,int maxlines)
{
    int left=(int)strlen(s);while(left&&maxlines--){int take=left;if(take>120)take=120;
        while(take>0&&((unsigned char)s[take]&0xc0)==0x80)take--;
        while(take>0&&text_measure_px(s,take,px,0)>width)take=ted_prev(s,take);
        if(take>0&&take<left&&s[take]!=' '){int word=take;while(word>take/2&&s[word-1]!=' ')word--;
            if(word>take/2)take=word;}
        if(take<=0)break;aui_text_n(x,y,s,take,color,px);s+=take;left-=take;y+=px+8;}
}
static void draw(void)
{
    int W=aui_width(),H=aui_height(),side=sidebar&&W>=760&&H>=640;
    int sw=side?clampi(W*31/100,290,360):0,dw=W-sw,x=dw+24,rw=sw-48;
    reference_count=0;raw_painted=0;aui_begin(AUI_BG);aui_fill(0,0,dw,48,AUI_SURFACE_2);aui_vhairline(dw,0,H-28);
    static const char *fonts[]={"系统字体","等宽字体"};static const char *sizes[]={"16","18","20","24"};
    int size=te_font==16?0:te_font==18?1:te_font==20?2:3;
    if(aui_select(16,8,110,fonts,2,&te_mono))cache_width=-1;
    if(aui_select(134,8,66,sizes,4,&size)){te_font=(int[]){16,18,20,24}[size];cache_width=-1;}
    if(aui_button_ex(212,8,34,30,"B",AUI_V_GHOST,!source_open&&!view_mode&&!load_refused))work_markup("**");
    if(aui_button_ex(250,8,34,30,"I",AUI_V_GHOST,!source_open&&!view_mode&&!load_refused))work_markup("*");
    if(aui_button_ex(292,8,62,30,"保存",AUI_V_GHOST,!load_refused)){if(save_document()>=0)saved=1;}
    if(aui_button_ex(dw-116,8,102,30,side?"收起工作":"这份文档",AUI_V_GHOST,1)){sidebar=!side;editor_focus=0;}
    aui_round(8,52,dw-16,H-86,6,AUI_SURFACE);
    body=(struct aui_rect){32,112,dw-64,H-176};if(body.w<40)body.w=40;if(body.h<20)body.h=20;
    if(source_open){char label[192];const struct ag_object *o=&work.task.objects[source_index];
        snprintf(label,sizeof label,"来源 %llu · 版本 %llu · %s",(unsigned long long)o->id,(unsigned long long)o->revision,o->path);
        aui_text_ellipsis(28,70,dw-300,label,AUI_TEXT,14);
        if(aui_button_ex(dw-258,65,66,28,"返回",AUI_V_GHOST,1)){source_open=0;scroll=0;cache_width=-1;}
        if(aui_button_ex(dw-186,65,76,28,"从头看",AUI_V_GHOST,source_offset>0))work_source(source_index,0);
        if(aui_button_ex(dw-104,65,76,28,"下一段",AUI_V_GHOST,source_offset+source_len<o->bytes))work_source(source_index,source_offset+source_len);
    }else {
        char label[192];snprintf(label,sizeof label,"%s · 当前版本 %llu",fname,(unsigned long long)agent_revision);
        aui_text_ellipsis(28,72,dw-252,label,AUI_MUTED,14);
        static const char *tabs[]={"当前正文","拟修改"};
        if(aui_segmented(dw-218,64,192,30,tabs,2,&view_mode)){editor_focus=0;scroll=0;follow_caret=0;cache_width=-1;}
        if(!proposal_text)view_mode=0;
    }
    aui_hairline(28,100,dw-56);draw_document();
    if(work.proposal&&!source_open)aui_text_sz(28,H-55,view_mode?"绿色标记：候选版本中变化的范围，尚未应用":"黄色标记：与候选稿不同的范围，当前正文已保留",AUI_MUTED,12);
    if(side){
        aui_text_sz(x,32,"这份文档的工作",AUI_TEXT,22);
        char summary[160];snprintf(summary,sizeof summary,"基于当前文档 · 版本 %llu",(unsigned long long)agent_revision);
        aui_text_ellipsis(x,68,rw,summary,AUI_MUTED,14);aui_hairline(x,104,rw);
        aui_ring(x+10,139,10,2,AUI_ACCENT);aui_circle(x+10,139,4,AUI_ACCENT);
        aui_text_sz(x+34,125,"当前目标",AUI_TEXT,15);
        side_text(x+34,153,rw-34,agent_task&&work_available?work.task.goal:"交给 TextEdit 一项工作",AUI_TEXT,16,2);
        aui_hairline(x,216,rw);
        aui_text_sz(x,236,"已使用的资料",AUI_TEXT,15);
        snprintf(summary,sizeof summary,agent_task?"%u 份已授权资料  ›":"当前文档（包含尚未保存的编辑）",work.task.object_count);
        if(aui_button_ex(x,262,rw,32,summary,AUI_V_GHOST,agent_task&&work_available))source_list=!source_list;
        aui_hairline(x,308,rw);
        aui_text_sz(x,329,"已完成的工作",AUI_TEXT,15);
        aui_text_sz(x,359,agent_task&&work.task.stage>=1?"Finder：已整理资料":"Finder：等待整理资料",AUI_MUTED,15);
        aui_text_sz(x,387,work.proposal?"TextEdit：修改已准备好":agent_task&&work.task.stage==2?"TextEdit：本轮已完成":"TextEdit：等待文稿",AUI_MUTED,15);
        aui_hairline(x,425,rw);
        int sy=445;
        aui_text_sz(x,sy,"当前状态",AUI_TEXT,15);
        aui_text_sz(x,sy+29,!agent_task?"等待你的要求":!work_available?"等待任务服务":phase_label(work.task.phase),AUI_TEXT,18);
        if(H>=720){
            const char *hint=work.proposal?"请查看两个版本，再决定是否应用。":!agent_task?"关闭窗口后，已开始的工作仍会保留。":work.task.phase==AG_WAIT_MODEL?"连接恢复后可继续，当前进度已保存。":"任务进度会保留，你可以随时接管。";
            side_text(x,sy+59,rw,hint,AUI_MUTED,13,2);}
        int by=H-146;
        if(work.proposal){
            if(aui_button_ex(x,by,rw,38,"应用修改",AUI_V_PRIMARY,proposal_text&&work_available&&!agent_dirty))work_decide(1);
            if(aui_button_ex(x,by+42,rw,28,"保留当前版本",AUI_V_GHOST,work_available))work_decide(0);
        }else {
            int p=(int)work.task.phase,running=p==AG_RUNNING||p==AG_QUEUED;
            if(aui_button_ex(x,by,rw/2-4,30,running?"暂停工作":p==AG_WAIT_BUDGET?"延长预算":"继续工作",AUI_V_SECONDARY,
                agent_task&&work_available&&(running||p==AG_PAUSED||p==AG_WAIT_MODEL||p==AG_WAIT_BUDGET)))
                work_control(running?AG_PAUSED:AG_QUEUED,p==AG_WAIT_BUDGET?32:0);
            if(aui_button_ex(x+rw/2+4,by,rw/2-4,30,"取消本轮",AUI_V_GHOST,agent_task&&work_available&&p!=AG_CANCELLED&&p!=AG_DONE))work_control(AG_CANCELLED,0);
            if(agent_task){snprintf(summary,sizeof summary,"推理 %u / %u 次 · 任务 %llu",work.task.calls,work.task.call_limit,(unsigned long long)agent_task);
                aui_text_sz(x,by+44,summary,AUI_MUTED,12);}
        }
        int can=!load_refused&&(!agent_task||(work_available&&(work.task.phase==AG_DONE||work.task.phase==AG_PAUSED)));
        int submit=aui_textfield_ex(x,H-64,rw-44,instruction,sizeof instruction,"继续告诉 TextEdit 要做什么…",can);
        if(aui_button_ex(x+rw-36,H-64,36,30,"↑",AUI_V_PRIMARY,can&&instruction[0])||submit)work_submit();
        if(source_list){
            int mh=clampi(150+(int)work.task.object_count*38,210,H-60);
            if(aui_dialog_begin("本任务已授权的资料",580,mh)){
                aui_label(24,18,"按任务保存的来源版本读取，原始资料保持原样。",AUI_MUTED);
                static int source_scroll;aui_scroll_begin(20,50,540,mh-110,&source_scroll,(int)work.task.object_count*38);
                for(unsigned i=0;i<work.task.object_count;i++){struct ag_object *o=&work.task.objects[i];
                    snprintf(summary,sizeof summary,"[S%llu] %s · v%llu",(unsigned long long)o->id,o->path,(unsigned long long)o->revision);
                    if(aui_button_ex(0,(int)i*38,520,34,summary,AUI_V_GHOST,1)){work_source((int)i,0);source_list=0;}}
                aui_scroll_end();const char *buttons[]={"返回文档"};if(aui_dialog_buttons(buttons,1)>=0)source_list=0;aui_dialog_end();}
        }
    }
    aui_fill(0,H-28,W,28,AUI_SURFACE_2);aui_hairline(0,H-28,W);
    char status[240];const char *notice=agent_notice?agent_notice:work_error[0]?work_error:0;
    if(notice)snprintf(status,sizeof status,"%s",notice);
    else snprintf(status,sizeof status,"%d 字节 / 1 MiB · %s%s",tlen,saved?"已保存":"有未保存的编辑",side?"":" · 放大窗口可显示文档工作侧栏");
    aui_text_ellipsis(20,H-21,W-40,status,notice?AUI_WARNING:AUI_MUTED,12);
    /* The optional monospace face uses the raw run API; include its viewport
     * explicitly. Proportional text is fully tracked by AUI's primitive diff. */
    if(te_mono||raw_painted)aui_end_rect(body.x,body.y,body.w,body.h);else aui_end();
}
