#ifndef BROWSER_PAINT_BOX_H
#define BROWSER_PAINT_BOX_H

/* Used paint geometry in window points. Percentages have already been resolved
 * against their corresponding axis, then ALL radii share the CSS overlap
 * reduction. The same record describes a background and its inner border. */
struct paint_box {
    int x,y,w,h;
    int rx[4],ry[4];                /* TL TR BR BL, clockwise */
};

#endif
