#ifndef BROWSER_FRAME_OPEN_H
#define BROWSER_FRAME_OPEN_H
#include "dom/tree.h"
#include "app/tabs.h"
#include <string.h>

#define FRAME_OPEN_LABEL "打开嵌入页面"

/* This is browser chrome painted inside a frame's existing empty box. It is
 * never a DOM child or a synthetic link: that would give author script a new
 * navigation primitive and misrepresent an isolated child document as pixels.
 * Empty sandbox/srcdoc attributes matter just as much as nonempty ones.
 * Correction (2026-09-25): sandboxed frames now LOAD inline with their flags
 * enforced (frame/sandbox.h); this refusal is no longer about honouring the
 * attribute. It stays because this action creates a TOP-LEVEL document, and
 * nothing here propagates a frame's sandboxing flags to one (top-level
 * sandbox enforcement is not implemented): opening would strip the author's
 * restrictions. The button also only exists when the frame is not rendered
 * inline, which a loaded sandboxed network frame is. */
static const char *frame_open_source(const struct node *n)
{
    if (!n || n->type != N_ELEM || strcmp(n->tag,"iframe") ||
        dom_attr(n,"sandbox") || dom_attr(n,"srcdoc")) return 0;
    const char *s=dom_attr(n,"src");
    if (!s || !s[0] || strlen(s)>=TAB_URL) return 0;
    for (const char *p=s; *p; p++)
        if ((unsigned char)*p<=32 || *p=='\\') return 0;
    for (const char *p=s; *p; p++) {
        if (*p==':' && strncmp(s,"http://",7) && strncmp(s,"https://",8)) return 0;
        if (*p=='/' || *p=='?' || *p=='#') break;
    }
    return s;
}

/* One rectangle for paint and trusted hit testing, inset from author borders.
 * A tiny tracking frame cannot expose an invisible native action. */
static int frame_open_box(int w,int h,int *x,int *y,int *bw,int *bh)
{
    if (w<116 || h<48) return 0;
    *bw=w-16; if (*bw>160) *bw=160;
    *bh=34; *x=(w-*bw)/2; *y=(h-*bh)/2;
    return 1;
}
#endif
