/* Exercises the production paint loop with recording raster primitives. The
 * guest gate separately checks real fonts/pixels; these stubs only prove call
 * budgets, byte boundaries, colour coverage and cache invalidation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../c/apps/studio/engine.h"
#include "../../c/apps/studio/highlight.h"
#define rgb(r,g,b) (((r)<<16)|((g)<<8)|(b))
static struct {unsigned bg,surface,surface_2,face,face_hover,face_active,border,hi,text,muted,selection,accent,focus;} aui_t;
#define AUI_BG aui_t.bg
#define AUI_TEXT aui_t.text
#define AUI_MUTED aui_t.muted
#define AUI_ACCENT aui_t.accent
#define AUI_SELECTION aui_t.selection
#define AUI_ERROR 0xff0000
#define AUI_SURFACE_2 aui_t.surface_2
static int focus,edit_x=196,edit_y=76,out_y=496,edit_w=700;
static StState fixture_state;
static const StState *state=&fixture_state;
static int calls,measures,colours[7],bad_utf8,caret_x,selection_width,clip_y,clip_h,bad_row_clip;
static void aui_theme_override(int v){(void)v;}
static int text_measure_px(const char *s,int n,int px,int mono)
{(void)px;(void)mono;measures++;int w=0;for(int i=0;i<n;i=st_next(s,i,n))w+=(unsigned char)s[i]<128?10:20;return w;}
static void aui_text_sz(int x,int y,const char *s,unsigned c,int px){(void)x;(void)y;(void)s;(void)c;(void)px;}
static void aui_text_ellipsis(int x,int y,int w,const char *s,unsigned c,int px)
{(void)w;aui_text_sz(x,y,s,c,px);}
static void gui_clip(int x,int y,int w,int h){(void)x;(void)w;clip_y=y;clip_h=h;}
static void gui_rect(int x,int y,int w,int h,unsigned c)
{(void)y;(void)h;if(c==AUI_ACCENT)caret_x=x;if(c==AUI_SELECTION)selection_width+=w;}
#include "../../c/apps/studio/studio_render.inc"
static void gui_text_run(int x,int y,int px,int mono,unsigned c,const char *s,int n)
{(void)x;(void)px;(void)mono;calls++;if(clip_y!=y-2||clip_h!=ST_ROW)bad_row_clip++;if(!st_utf8(s,n))bad_utf8++;for(int i=0;i<7;i++)if(c==(i==ST_INK_KEYWORD?rgb(198,120,221):ink_color(i)))colours[i]++;}
#include "../../c/apps/studio/studio_code.inc"
static StEngine *engine;
static int dialog, context_open, completion_selected, work_x = 896;
static uint64_t completion_due;
static uint64_t popup_test_now(void *context) { (void)context; return 0; }
static const StDocument *document(void) { return st_engine_document(engine); }
static void reveal(const StDocument *d) { (void)d; }
#include "../../c/apps/studio/studio_completion.inc"
static int checks;
#define CHECK(label,cond) do {checks++;if(!(cond)){fprintf(stderr,"FAIL %s at %d\n",label,__LINE__);exit(1);}}while(0)
static void sample(const char *s)
{StDocument d;CHECK("document",st_init(&d,"/test.as",s,(int)strlen(s),1)==0);reset_ink();draw_code(&d);st_dispose(&d);}
int main(void)
{
    studio_theme();fixture_state.problem_tab=-1;
    CHECK("black",aui_t.bg==0&&aui_t.surface==0x111111);
    sample("# 中文 comment\ndef sum(values):\n    total = 42\n    print(\"你好\", total)\n    return total + 1\n");
    CHECK("highlight",colours[ST_INK_KEYWORD]&&colours[ST_INK_STRING]&&colours[ST_INK_NUMBER]&&colours[ST_INK_COMMENT]&&colours[ST_INK_CALL]);
    CHECK("utf8 runs",!bad_utf8);CHECK("row clip",!bad_row_clip);
    char *longline=malloc(65537);memset(longline,'x',65536);longline[65536]=0;
    calls=measures=0;sample(longline);
    CHECK("bounded paint",calls<=2&&measures<=2);free(longline);
    const char *src="# 同一段中文注释 repeat repeat repeat\n";
    StDocument d;CHECK("document",st_init(&d,"/cache.as",src,(int)strlen(src),1)==0);reset_ink();
    draw_code(&d);measures=0;for(int i=0;i<20;i++)draw_code(&d);
    CHECK("warm cache",measures==0);
    partial_code=1;calls=0;draw_code(&d);CHECK("unchanged rows",calls==0);partial_code=0;
    CHECK("edit",st_replace(&d,"def ",4)==0);draw_code(&d);
    CHECK("revision invalidation",ink[0]==ST_INK_KEYWORD);
    st_dispose(&d);
    CHECK("document",st_init(&d,"/utf8.as","a中b",5,1)==0);reset_ink();d.caret=4;d.anchor=1;
    selection_width=0;draw_code(&d);
    CHECK("cjk caret",caret_x==edit_x+54+30);
    CHECK("cjk selection",selection_width==20);st_dispose(&d);
    unsigned char styles[80];const char *unfinished="'first\n# still string\nend' # comment";
    st_highlight(unfinished,(int)strlen(unfinished),styles);
    CHECK("multiline string",styles[7]==ST_INK_STRING&&styles[strrchr(unfinished,'#')-unfinished]==ST_INK_COMMENT);
    st_highlight("'中文",7,styles);CHECK("unfinished string",styles[6]==ST_INK_STRING);
    /* Exercise production popup geometry/input with the real edit engine.
     * Raster stubs above measure paint calls; they do not invent candidates. */
    StHost host = {.now = NULL};
    /* The host clock is unused by rendering, but edits schedule checkpoints. */
    host.now = popup_test_now;
    engine = st_engine_create(&host);
    CHECK("popup engine", engine != NULL);
    char path[] = "/tmp/studio-popup-XXXXXX";
    int fd = mkstemp(path);
    CHECK("popup fixture", fd >= 0);
    close(fd);
    CHECK("popup document", st_engine_open(engine, path) == 0);
    state = st_engine_state(engine);
    CHECK("popup prefix", st_engine_insert(engine, "pri", 3) == 0);
    st_engine_complete(engine);
    CHECK("popup candidates", state->completion_count > 0);
    StCompletionPopup popup;
    CHECK("popup geometry", completion_popup(&popup));
    char expected[128];
    snprintf(expected, sizeof expected, "%s", state->completions[0].insert);
    completion_due = 1000;
    CHECK("popup click consumes", click_completion(popup.x + 8, popup.y + 3));
    CHECK("popup click inserts", !strcmp(document()->text, expected));
    CHECK("popup click closes", !state->completion_count && !completion_due);
    st_engine_select(engine, document()->length, 0);
    st_engine_insert(engine, "", 0);
    st_engine_complete(engine);
    CHECK("popup scroll candidates", state->completion_count > 6);
    completion_selected = state->completion_count - 1;
    CHECK("popup scroll geometry", completion_popup(&popup) && popup.first > 0);
    snprintf(expected, sizeof expected, "%s", state->completions[popup.first].insert);
    CHECK("popup scrolled click", click_completion(popup.x + 8, popup.y + 3));
    CHECK("popup scrolled inserts", !strcmp(document()->text, expected));
    st_engine_complete(engine);
    completion_due = 1000;
    int caret = document()->caret;
    CHECK("popup outside passes through", !click_completion(0, 0));
    CHECK("popup outside dismisses", !state->completion_count && !completion_due);
    CHECK("popup outside preserves caret", document()->caret == caret);
    st_engine_destroy(engine);
    unlink(path);
    printf("PASS studio render %d checks\n",checks);free(ink);return 0;
}
