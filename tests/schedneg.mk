# The negative-control build of the kernel: everything the real Makefile does,
# plus -DSCHED_IGNORE_WEIGHT.
#
# NOT -include'd by anything. It is invoked explicitly:
#
#   make -f tests/schedneg.mk BUILD=build/schedneg build/schedneg/logit.iso
#
# WHY A WRAPPER AND NOT A KNOB IN THE MAKEFILE: the Makefile already carries
# five of these (CHURN, GROWFI, FPO, KLOGUNSAFE, NOSHAPE) and a sixth would be
# the natural place for this. It is not taken because several lines are editing
# that file concurrently right now and this line was asked not to. `make
# CFLAGS=...` from the command line is not the alternative -- a command-line
# assignment REPLACES the variable, so the whole target/freestanding/sse2 flag
# list would have to be copied here and would rot the first time it changed.
# Appending after the include leaves exactly one line of divergence.
#
# BUILD IS OVERRIDDEN ON THE COMMAND LINE AND MUST BE. Line 17 of the Makefile
# is `BUILD := build`, and a command-line assignment beats even a simple
# assignment, so ISO/KERNEL/DISK all follow to the new tree. Sharing build/
# instead would be worse than untidy: the Makefile's own debug-knob comment
# says objects are NOT flag-tracked, so the instrumented sched.o would be
# reused by the next ordinary build with nothing to show for it.
include Makefile
CFLAGS += -DSCHED_IGNORE_WEIGHT
