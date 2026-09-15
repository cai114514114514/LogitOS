#include "layout.h"
static int min(int a, int b)
{
    return a < b ? a : b;
}
struct clock_layout clock_layout(int width, int height, int scale)
{
    struct clock_layout l = {.compact = width < 520};
    l.footer_y = height - 52;
    /* Reserve readout and controls before sizing the optional dial. A tiny
     * window keeps usable text instead of squeezing it underneath the buttons. */
    l.face_size = min(l.compact ? width - 40 : width / 2 - 36, height - (l.compact ? 240 : 138));
    l.face_size = min(l.face_size, 38400 / (scale > 0 ? scale : 100));
    if (l.face_size < 64)
        l.face_size = 0;
    l.face_x = l.compact ? (width - l.face_size) / 2 : 28;
    l.face_y = 66;
    l.text_x = l.compact ? 20 : width / 2 + 16;
    l.text_w = l.compact ? width - 40 : width - l.text_x - 24;
    l.text_y = l.compact ? 76 + l.face_size : (height < 300 ? 70 : 106);
    if (!l.face_size)
        l.text_y = 66;
    return l;
}
