#ifndef CSS_BACKGROUND_HOST_H
#define CSS_BACKGROUND_HOST_H
#include "logit_abi.h"
/* A pixel consumer, not an op-count oracle: blend every real paint syscall
 * immediately, before the renderer reuses its scratch RGBA buffer. */
void gui_rect(int, int, int, int, unsigned);
void gui_rrect(int, int, int, int, int, unsigned);
void gui_blit(int, int, int, int, const unsigned char *, int, int);
void gui_clip(int, int, int, int);
void gui_text_run_w(int, int, int, int, unsigned, const char *, int, int);
static inline void gui_text_run(int x,int y,int px,int mono,unsigned c,const char *s,int n)
{ gui_text_run_w(x,y,px,mono,c,s,n,0); }
static inline int ui_scale(void) { return 100; }
static inline int text_run_metrics_px(const char *s,int n,int px,int face,struct logit_text_metrics *out)
{ (void)s;(void)n;(void)px;(void)face;(void)out;return 0; }
#endif
