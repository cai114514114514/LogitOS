/* sysroot_dev_hdrs_all.c -- the SECOND `tcc -E` file: every POSIX header the
 * tcc sources themselves include (grep '# *include <' over third_party/tcc/
 * {tcc.h,tcc.c,libtcc.c,tccpp.c,tccgen.c,tccelf.c,tccasm.c,tcctools.c,
 * x86_64-gen.c,x86_64-link.c,i386-asm.c}, 2026-08-21), minus the Windows
 * ones (direct.h io.h process.h windows.h) and dlfcn.h, which only the -run
 * JIT reaches and tcc.aex does not compile (tests/tcc.mk, tccrun.c dropped).
 * This is the set a self-hosting tcc (workflow 4) has to get through the
 * preprocessor on the device; sysroot_dev_hdrs.c is the task's seven. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
int sysroot_dev_hdrs_all_probe;
