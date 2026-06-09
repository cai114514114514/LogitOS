#ifndef _ASSERT_H
#define _ASSERT_H
/* Release build: assertions compiled out (keeps QuickJS lean + dependency-free). */
#define assert(x) ((void)0)
#endif
