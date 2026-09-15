#ifndef OPENLOGIT_WIRE_H
#define OPENLOGIT_WIRE_H
/* Window command transport, included by logit.h after _sys is declared.
 * Coordinates here are window-local logical points; the kernel validates
 * arguments, copies user buffers and invokes OpenLogit's display backend.
 * Drawing changes the window backing store; flush publishes one frame. */
static inline void ol_window_clear(unsigned color)
{ _sys(SYS_GUI_CLEAR, color, 0, 0); }
static inline void ol_window_rect(int x, int y, int w, int h, unsigned color)
{ _sys(SYS_GUI_RECT, ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF),
       ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF), color); }
static inline void ol_window_rrect(int x, int y, int w, int h, int radius, unsigned color)
{ _sys(SYS_GUI_RRECT, ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF),
       ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF), ((long)(radius & 0xFF) << 24) | (color & 0xFFFFFF)); }
static inline void ol_window_text(int x, int y, unsigned color, const char *s)
{ _sys(SYS_GUI_TEXT, ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF), color, (long)s); }
static inline void ol_window_text_mono(int x, int y, unsigned color, int cell, const char *s)
{ _sys(SYS_GUI_TEXT_MONO, ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF),
       ((long)(cell & 0xFF) << 24) | (color & 0xFFFFFF), (long)s); }
static inline void ol_window_text_run_w(int x, int y, int px, int mono, unsigned color,
                                  const char *s, int len, int bold)
{ struct logit_run r = { x, y, px, mono, color, s, len, bold }; _sys(SYS_GUI_TEXT_RUN, (long)&r, 0, 0); }
static inline void ol_window_text_run(int x, int y, int px, int mono, unsigned color, const char *s, int len)
{ ol_window_text_run_w(x, y, px, mono, color, s, len, 0); }
static inline void ol_window_blit(int x, int y, int w, int h, const unsigned char *rgba, int sw, int sh)
{ struct logit_blit b = { x, y, w, h, rgba, sw, sh }; _sys(SYS_GUI_BLIT, (long)&b, 0, 0); }
static inline void ol_window_icon(int id, int x, int y, int px, unsigned color)
{ _sys(SYS_GUI_ICON, ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF),
       ((long)(id & 0xFFFF) << 16) | (px & 0xFFFF), color); }
static inline void ol_window_glass(int x, int y, int w, int h, int radius,
                             unsigned char tr, unsigned char tg, unsigned char tb, unsigned char ta)
{ _sys(SYS_GUI_GLASS, ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF),
       ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF),
       ((long)radius << 32) | ((long)tr << 24) | ((long)tg << 16) | ((long)tb << 8) | ta); }
static inline void ol_window_flush(void)
{ _sys(SYS_GUI_FLUSH, 0, 0, 0); }
static inline void ol_window_flush_rect(int x, int y, int w, int h)
{ _sys(SYS_GUI_FLUSH_RECT, ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF),
       ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF), 0); }
#endif
