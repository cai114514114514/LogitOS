/* One font/run contract for drawing, caret placement and pointer hit testing.
 * The old byte*8 estimate disagreed with proportional glyphs even for ASCII;
 * a UTF-8 byte is not a visible character. The fixed 16 is now a font request
 * passed to BOTH consumers, rather than an assumption about gui_text's font. */
#define ADDRESS_FONT_PX 16
int text_measure(const char *, int, int, int);
static int addr_hscroll;
static int addr_width(int end) { return text_measure(url,end,ADDRESS_FONT_PX,0); }
static int addr_hit(int x)
{
    int want=x-14+addr_hscroll,best=0,bd=0x7fffffff;
    for(int p=0;p<=ulen;) {
        int d=addr_width(p)-want;if(d<0)d=-d;
        if(d<bd){bd=d;best=p;}
        if(p==ulen)break;
        p++;while(p<ulen&&((unsigned char)url[p]&0xc0)==0x80)p++;
    }
    return best;
}
static void draw_address_bar(int editing)
{
#ifdef BROWSER_ADDRESS_OLD_GEOMETRY
    gui_text(14,TABH+7,rgb(40,40,48),url);
    if(editing)gui_rect(14+ucaret*8,TABH+7,2,16,rgb(90,150,240));
#else
    gui_glass(0,TABH,win_w,BARH,1,255,255,255,70);
    gui_glass(10,TABH+5,win_w-20,20,8,255,255,255,95);
    int width=win_w-50;if(width<2)width=2;
    int caret=addr_width(ucaret),total=addr_width(ulen);
    if(!editing)addr_hscroll=0;
    else {
        if(caret<addr_hscroll)addr_hscroll=caret;
        if(caret>addr_hscroll+width-2)addr_hscroll=caret-width+2;
        int max=total-width+2;if(max<0)max=0;
        if(addr_hscroll>max)addr_hscroll=max;
    }
    gui_clip(14,TABH+6,width,18);
    if(editing&&usel!=ucaret) {
        int a=addr_width(usel),b=caret;
        if(a>b){int t=a;a=b;b=t;}
        gui_rect(14+a-addr_hscroll,TABH+6,b-a,18,rgb(140,180,250));
    }
    gui_text_run(14-addr_hscroll,TABH+7,ADDRESS_FONT_PX,0,rgb(40,40,48),url,ulen);
    if(editing)gui_rect(14+caret-addr_hscroll,TABH+7,2,16,rgb(90,150,240));
    gui_clip(0,0,0,0);
#endif
    if(bookmark_find(url)>=0)gui_text(win_w-26,TABH+7,rgb(240,180,60),"*");
}
