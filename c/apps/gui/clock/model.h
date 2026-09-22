#ifndef CLOCK_MODEL_H
#define CLOCK_MODEL_H
struct clock_state {
    int hour24, seconds;
};
enum clock_action { CLOCK_NO_ACTION, CLOCK_TOGGLE_FORMAT, CLOCK_TOGGLE_SECONDS };
void clock_apply(struct clock_state *, enum clock_action);
void clock_hm(char out[6], int hour, int minute, int hour24);
#endif
