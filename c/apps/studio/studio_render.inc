/* Pixel measurements belong to the view. Cache exact runs rather than assuming
 * every UTF-8 byte is one monospace cell: CJK fallback and display scale change
 * advances. A previous view made TWO syscalls per visible codepoint per frame.
 * Runs are bounded to 256 bytes (the kernel's text request is also bounded).
 * Warm frames reuse measured runs; selection and hit testing use this same path. */
/* 16 px text needs its real ascender/descender room. At 21 px, the real
 * guest differential found 30 changed pixels: repainting a blank row erased
 * the previous line's descenders, and another repaint doubled their alpha.
 * Row clipping plus a 24 px pitch keeps each retained row self-contained. */
#define ST_ROW 24
#define ST_RUN_BYTES 256
static struct { unsigned hash; int n,width; char text[ST_RUN_BYTES]; } widths[512];
static unsigned paint_runs,paint_measures;
static int trace_paint;
static unsigned char *ink;
static int ink_capacity;
static const char *ink_text;
static uint64_t ink_revision;
static char ink_path[128];
static void reset_ink(void){ink_text=NULL;}
static int run_end(const char *s,int start,int end)
{
    if(end-start<=ST_RUN_BYTES)return end;
    int p=start+ST_RUN_BYTES;while(p>start&&((unsigned char)s[p]&0xc0)==0x80)p--;
    return p;
}
static int run_width(const char *s,int n)
{
    if(!n)return 0;
    unsigned hash=st_hash(s,(size_t)n),slot=hash%512;
    if(widths[slot].n==n&&widths[slot].hash==hash&&!memcmp(widths[slot].text,s,(size_t)n))return widths[slot].width;
    paint_measures++;
    int w=text_measure_px(s,n,16,1);
    widths[slot].hash=hash;widths[slot].n=n;widths[slot].width=w;
    memcpy(widths[slot].text,s,(size_t)n);return w;
}
static int line_width(const StDocument *d,int start,int end)
{
    int width=0;
    while(start<end){int next=run_end(d->text,start,end);width+=run_width(d->text+start,next-start);start=next;}
    return width;
}
static int prepare_ink(const StDocument *d)
{
    if(ink_text==d->text&&ink_revision==d->revision&&!strcmp(ink_path,d->path))return 1;
    if(d->length>ink_capacity){
        unsigned char *p=realloc(ink,(size_t)d->length);if(!p)return 0;
        ink=p;ink_capacity=d->length;
    }
    st_highlight(d->text,d->length,ink);
    ink_text=d->text;ink_revision=d->revision;snprintf(ink_path,sizeof ink_path,"%s",d->path);return 1;
}
static unsigned ink_color(int kind)
{
    switch(kind){
    case ST_INK_KEYWORD:return rgb(198,120,221);
    case ST_INK_STRING:return rgb(152,195,121);
    case ST_INK_COMMENT:return rgb(110,118,132);
    case ST_INK_NUMBER:return rgb(229,192,123);
    case ST_INK_OPERATOR:return rgb(86,182,194);
    case ST_INK_CALL:return rgb(97,175,239);
    default:return AUI_TEXT;
    }
}
static void studio_theme(void)
{
    aui_theme_override(1);
    aui_t.bg=rgb(0,0,0);aui_t.surface=rgb(17,17,17);aui_t.surface_2=rgb(25,25,25);
    aui_t.face=rgb(32,32,32);aui_t.face_hover=rgb(45,45,45);aui_t.face_active=rgb(57,57,57);
    aui_t.border=rgb(48,48,48);aui_t.hi=rgb(63,63,63);
    aui_t.text=rgb(220,223,230);aui_t.muted=rgb(145,145,150);
    aui_t.selection=rgb(38,62,88);aui_t.accent=aui_t.focus=rgb(86,182,194);
}
