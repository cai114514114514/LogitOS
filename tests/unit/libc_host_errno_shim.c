/* The one-line reason this file exists: c/apps/libc/include/errno.h declares
 * `extern int errno;` -- a plain, non-thread-local global, which is correct
 * for this kernel (one thread per process, no TLS anywhere in the freestanding
 * build). glibc's errno is thread-local, so linking an object that only
 * REFERENCES errno (inet.c, regex.c) against glibc's libc.so for the rest of
 * the host test binary's C runtime (printf, malloc, _start, ...) fails at
 * link time with "TLS definition ... mismatches non-TLS reference". Providing
 * the real, strong, non-TLS definition here in the same link satisfies the
 * reference before the dynamic linker ever needs glibc's TLS one. This is
 * link-time plumbing for tests/libc.mk's host diff tests, not part of the
 * library itself -- the real `int errno;` definition mini-libc SHIPS lives in
 * c/apps/libc/src/io.c, which these host tests do not link (io.c is excluded
 * from the analogous test-libc-diff for the same class of host-linking
 * reasons; see its LIBCDIFF_SRC filter-out in the main Makefile). */
int errno;
