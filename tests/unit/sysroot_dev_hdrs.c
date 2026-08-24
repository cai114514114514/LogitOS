/* sysroot_dev_hdrs.c -- the file `tcc -E` is run over ON THE DEVICE and on
 * the host, so the two preprocessed outputs can be compared byte for byte
 * (tests/boot/run-sysroot-device.sh). The seven headers are the ones the
 * task named: the headers tcc's own source includes that a libc provides.
 * The declaration at the end is so the file is a translation unit and not
 * an empty one. Nothing here may depend on the date, the host, or a macro
 * that differs between the two tccs -- the point is that it does not. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <setjmp.h>
int sysroot_dev_hdrs_probe;
