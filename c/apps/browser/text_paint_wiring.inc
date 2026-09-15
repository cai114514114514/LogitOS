#include "logit_abi.h"
#include "layout_text.h"
/* Included after sty() by browser_paint.c. Layout's ltx_measure_run adds
 * spacing to native font advances; handing the same whole string to the GUI
 * would silently paint the old spacing inside a correctly widened box.
 * Shared segment measurement keeps layout and paint advances equal. With
 * word-spacing, keep whole words together; letter-spacing uses UTF-8 scalar
 * boundaries, matching ltx_measure_run's current unit (not grapheme clusters).
 * Vertical orientation and complex-script cluster spacing remain unimplemented
 * at the shared formatter/font boundary, not simulated by rotating positions. */
int text_measure(const char *,int,int,int);
static int text_native_advance(const struct item *e,const char *text,int len)
{
    return fc_text_measure_runs(text,len,e->font_px,
        e->mono|(e->bold?LOGIT_FACE_BOLD:0),text_measure);
}
static int text_segment_advance(const struct item *e,const char *text,int len,int ls,int ws)
{
    return text_native_advance(e,text,len)+ls+(text[0]==' '?ws:0);
}
int browser_text_run_advance(const struct item *e,int len)
{
    if(!e||!e->text||len<=0)return 0;if(len>e->len)len=e->len;
    const struct cstyle *s=sty(e);
    int ls=s?s->letter_spacing:0,ws=s?s->word_spacing:0;
#ifdef BROWSER_CARET_RAW_METRICS
    ls=ws=0;
#endif
    /* The former prefix measurer ignored spacing that paint had already
     * applied. Reuse its exact segments; per-prefix whole-run reshaping would
     * also disagree with separately painted words. Cluster/source mapping for
     * length-changing text-transform remains a separate layout contract. */
    if(!ls&&!ws)return text_native_advance(e,e->text,len);
    int dx=0;
    for(int p=0;p<len;){int q=ltx_spacing_next(e->text,len,p,ls);dx+=text_segment_advance(e,e->text+p,q-p,ls,ws);p=q;}
    return dx;
}
/* Return the same advance the caller would obtain from text_native_advance.
 * Splitting only measurement would still leave SYS_GUI_TEXT_RUN silently
 * dropping everything after byte 1023; splitting only paint would make the
 * caret drift at every boundary where the font rounds a new native run. */
static int paint_text_native(const struct item *e,int sx,int sy,uint32_t col,
                             const char *text,int len)
{
    int dx=0;
    for(int p=0;p<len;){
        int q=fc_text_run_next(text,len,p);
        gui_text_run_w(sx+dx,sy,e->font_px,e->mono,col,text+p,q-p,e->bold);
        dx+=text_native_advance(e,text+p,q-p);p=q;
    }
    return dx;
}
static void paint_text_run_spaced(const struct item *e,int sx,int sy,uint32_t col)
{
    const struct cstyle *s=sty(e);
    int ls=s?s->letter_spacing:0,ws=s?s->word_spacing:0;
#ifdef PAINT_NO_TEXT_SPACING
    ls=ws=0;
#endif
    if(!ls&&!ws){paint_text_native(e,sx,sy,col,e->text,e->len);return;}
    int p=0,dx=0;
    while(p<e->len){
        int q=ltx_spacing_next(e->text,e->len,p,ls);
        paint_text_native(e,sx+dx,sy,col,e->text+p,q-p);
        dx+=text_segment_advance(e,e->text+p,q-p,ls,ws);p=q;
    }
}
