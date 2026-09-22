#include "model.h"
/* The application owns preferences. UI widgets return semantic actions and
 * never mutate them, so keyboard and pointer input have exactly one owner. */
void clock_apply(struct clock_state *state, enum clock_action action)
{
    if (action == CLOCK_TOGGLE_FORMAT)
        state->hour24 = !state->hour24;
    if (action == CLOCK_TOGGLE_SECONDS)
        state->seconds = !state->seconds;
}

void clock_hm(char out[6], int hour, int minute, int hour24)
{
    if (!hour24) {
        hour %= 12;
        if (!hour)
            hour = 12;
    }
    out[0] = '0' + hour / 10;
    out[1] = '0' + hour % 10;
    out[2] = ':';
    out[3] = '0' + minute / 10;
    out[4] = '0' + minute % 10;
    out[5] = 0;
}
