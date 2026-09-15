/* The existing loader harness moves mmap pages with Linux mremap. Darwin has
 * no mremap; copying a fresh anonymous frame into a fixed destination preserves
 * this harness's semantics (frames are never shared). Run Darwin as x86_64 so
 * its 4096-byte pages match the guest; 16 KiB host pages cannot independently
 * enforce the guest's four adjacent permission domains. */
#ifdef __APPLE__
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2
static void *pie_mremap(void *old, size_t oldsz, size_t newsz, int flags, void *dst)
{
    (void)flags;
    if (getpagesize() != 4096 || oldsz != newsz) abort();
    void *p = mmap(dst, newsz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return p;
    memcpy(p, old, oldsz);
    munmap(old, oldsz);
    return p;
}
#define mremap pie_mremap
#endif
#include "exechost/space.c"
