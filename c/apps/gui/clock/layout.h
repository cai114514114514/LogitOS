#ifndef CLOCK_LAYOUT_H
#define CLOCK_LAYOUT_H
#define CLOCK_MIN_WIDTH 240
#define CLOCK_MIN_HEIGHT 240
struct clock_layout {
    int face_x, face_y, face_size, text_x, text_y, text_w, footer_y, compact;
};
struct clock_layout clock_layout(int width, int height, int scale);
#endif
