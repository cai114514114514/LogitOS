#ifndef LOGIT_PASSIVE_FRAME_H
#define LOGIT_PASSIVE_FRAME_H
#include <stdint.h>
#include "../../../include/weaksym.h"
struct node;struct layout_context;
#define PASSIVE_FRAME_SCRIPT_NOTICE "嵌入预览，页面脚本尚未运行"
struct passive_frame_view {struct layout_context *layout;int width,height,scripts_disabled;unsigned generation;};
#ifdef PASSIVE_FRAME_OPTIONAL
#define PF_FN LOGIT_WEAK
#else
#define PF_FN
#endif
PF_FN int passive_frames_enabled(void);
PF_FN void passive_frames_reset(void);
PF_FN void passive_frames_set_parent(const char *url,const char *csp,int policy_known);
PF_FN int passive_frames_update(struct node *root,unsigned long long mutation);
PF_FN int passive_frames_pending(void);
PF_FN int passive_frame_view(const struct node *host,struct passive_frame_view *out);
/* Content viewport relative to an iframe border box. Shared with paint. */
PF_FN int passive_frame_content_box(const struct node *host,int w,int h,int *x,int *y,int *cw,int *ch);
#ifdef PASSIVE_FRAME_OPTIONAL
LOGIT_WEAK_STUB(passive_frames_enabled);
LOGIT_WEAK_STUB(passive_frames_reset);
LOGIT_WEAK_STUB(passive_frames_set_parent);
LOGIT_WEAK_STUB(passive_frames_update);
LOGIT_WEAK_STUB(passive_frames_pending);
LOGIT_WEAK_STUB(passive_frame_view);
LOGIT_WEAK_STUB(passive_frame_content_box);
#endif
#undef PF_FN
#endif
