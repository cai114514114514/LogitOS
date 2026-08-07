/* Host-test stub: kprintf goes to stdout (implemented in mm_test_util.c). */
#ifndef MMSTUB_KPRINTF_H
#define MMSTUB_KPRINTF_H

void kprintf(const char *fmt, ...);

/* Test hooks: count/inspect what the kernel code reported. A test asserts on
 * the number of "[mm] BUG:" lines, not on eyeballing the output. */
int  mm_log_lines(void);              /* total kprintf calls so far */
int  mm_log_bugs(void);               /* lines containing "BUG:" */
void mm_log_reset(void);
void mm_log_quiet(int quiet);         /* 1 = swallow output (expected-failure tests) */
const char *mm_log_last(void);

#endif
