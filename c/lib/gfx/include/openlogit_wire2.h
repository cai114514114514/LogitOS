#ifndef OPENLOGIT_WIRE2_H
#define OPENLOGIT_WIRE2_H
/* GUI ABI v2 window transport (include/abi/logit_abi.h, "GUI ABI v2").
 * Included at the end of openlogit_wire.h, so _sys and the ABI structs are
 * already in scope. Each wrapper stamps the struct's `size` with the sizeof
 * this caller was compiled against and returns the kernel's status unchanged:
 * 0, or a negative ol_status (OL_UNSUPPORTED until the matching
 * LOGIT_GUI2_F_* bit is reported by ol_gui2_info(0)). The caller's struct is
 * only written in its `size` field, so one value can be reused across calls.
 * All coordinates are window-local 24.8 points. */

/* what = 0: (LOGIT_GUI2_VERSION << 16) | LOGIT_GUI2_F_* bits.
 * what = 1: clip/group levels auto-closed at this window's flushes. */
static inline long ol_gui2_info(int what)
{ return _sys(SYS_GUI2_INFO, what, 0, 0); }

/* stroke == 0 fills the path with paint; otherwise strokes it. */
static inline long ol_window_path2(struct logit_path2 *path, struct logit_paint2 *paint,
                                   struct logit_stroke2 *stroke)
{
    path->size = sizeof *path;
    paint->size = sizeof *paint;
    if (stroke) stroke->size = sizeof *stroke;
    return _sys(SYS_GUI_PATH, (long)path, (long)paint, (long)stroke);
}
static inline long ol_window_clip_push2(struct logit_clip2 *clip)
{ clip->size = sizeof *clip; return _sys(SYS_GUI_CLIP_PUSH, (long)clip, 0, 0); }
static inline long ol_window_clip_pop2(void)
{ return _sys(SYS_GUI_CLIP_POP, 0, 0, 0); }
static inline long ol_window_group_begin2(struct logit_group2 *group)
{ group->size = sizeof *group; return _sys(SYS_GUI_GROUP_BEGIN, (long)group, 0, 0); }
static inline long ol_window_group_end2(void)
{ return _sys(SYS_GUI_GROUP_END, 0, 0, 0); }
static inline long ol_window_blit2(struct logit_blit2 *blit)
{ blit->size = sizeof *blit; return _sys(SYS_GUI_BLIT2, (long)blit, 0, 0); }
static inline long ol_window_text_run2(struct logit_run2 *run)
{ run->size = sizeof *run; return _sys(SYS_GUI_TEXT_RUN2, (long)run, 0, 0); }
static inline long ol_window_shadow2(struct logit_shadow2 *shadow)
{ shadow->size = sizeof *shadow; return _sys(SYS_GUI_SHADOW2, (long)shadow, 0, 0); }
static inline long ol_window_glass2(struct logit_glass2 *glass)
{ glass->size = sizeof *glass; return _sys(SYS_GUI_GLASS2, (long)glass, 0, 0); }
static inline long ol_window_backdrop2(struct logit_backdrop2 *backdrop)
{ backdrop->size = sizeof *backdrop; return _sys(SYS_GUI_BACKDROP, (long)backdrop, 0, 0); }
/* id 0..LOGIT_BACKDROP_MAX-1 drops one region; -1 drops every region. */
static inline long ol_window_backdrop_clear2(int id)
{ return _sys(SYS_GUI_BACKDROP_CLEAR, id, 0, 0); }
#endif
