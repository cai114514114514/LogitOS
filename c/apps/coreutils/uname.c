#include "clib.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* Was "LogitOS x86_64 -- from scratch": a project tagline where a system
     * name belongs, the first thing `uname -a` showed on the 2026-10-26 desktop
     * tour. tests/boot/run-exec-test.sh and run-mm-test.sh match the
     * "LogitOS x86_64" prefix, which is all that is left. */
    outs("LogitOS x86_64\n");
    return 0;
}
