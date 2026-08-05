#include "clib.h"

/* sleep N -- wait N seconds, by polling the wall clock and yielding. */
static int secs_of(struct logit_time *t) { return t->hour * 3600 + t->minute * 60 + t->second; }

int main(int argc, char **argv)
{
    int want = argc > 1 ? c_atoi(argv[1]) : 1;
    struct logit_time t0; get_time(&t0);
    for (;;) {
        struct logit_time t; get_time(&t);
        int dt = secs_of(&t) - secs_of(&t0);
        if (dt < 0) dt += 86400;
        if (dt >= want) break;
        sys_yield();
    }
    return 0;
}
