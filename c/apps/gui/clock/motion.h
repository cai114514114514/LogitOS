#ifndef CLOCK_MOTION_H
#define CLOCK_MOTION_H
#include <stdint.h>
struct clock_motion {
    int initialized, second, from;
    uint64_t start;
};
int clock_second(struct clock_motion *, int second, uint64_t now, int reduced, int *active);
#endif
