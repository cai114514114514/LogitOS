/* Link-time stand-ins for the slice of the kernel elf.c/aex.c reference that
 * aexsig_test.c never actually reaches.
 *
 * aex_parse() -- the only entry point this test calls -- reads the container
 * header, walks the TLV region, checks the CRC and (this task's addition)
 * the Ed25519 signature. It never loads a single ELF segment, so it never
 * calls elf_load_reader()/place_page(), which are the only callers of the
 * five symbols below. tests/unit/exechost/space.c provides REAL, host-MMU-
 * backed implementations of these for tests/exec.mk's test-exec, which
 * actually does load segments -- but space.c needs mremap(), which does not
 * exist on this documented dev host (macOS/Apple Silicon; verified: it is a
 * Linux-only syscall), so linking it here to satisfy an unreachable symbol
 * would trade one host-portability problem for another, for code this test
 * never runs.
 *
 * kprintf() is different: aex.c calls it FOR REAL, every refusal and every
 * signature-verdict line goes through it, so this is a real implementation,
 * not a stub -- the test's own output IS partly this function's output.
 *
 * The other five ABORT rather than return a plausible-looking value if ever
 * called, on purpose: a future change to aexsig_test.c that starts calling
 * aex_load_image_ex() or similar would otherwise silently run against a
 * machine that was never really there, and get a "pass" that proved nothing.
 * An abort points straight at the missing space.c-equivalent instead. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include "kprintf.h"

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static void unreached(const char *who)
{
    fprintf(stderr, "aexsig_test: unexpected call to %s -- this test does not "
                    "load ELF segments, only parse the AEX container. If a "
                    "change made it reach here, it needs the real host MMU "
                    "harness (tests/unit/exechost), not this stub.\n", who);
    abort();
}

uint64_t pmm_alloc(void) { unreached("pmm_alloc"); return 0; }
void pmm_free(uint64_t phys) { (void)phys; unreached("pmm_free"); }
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{ (void)virt; (void)phys; (void)flags; unreached("vmm_map_page"); }
uint64_t *vmm_pte(uint64_t cr3, uint64_t virt)
{ (void)cr3; (void)virt; unreached("vmm_pte"); return NULL; }
int cpu_prot_nx_usable(void) { unreached("cpu_prot_nx_usable"); return 0; }
void kernel_random_bytes(uint8_t *out, int len)
{ (void)out; (void)len; unreached("kernel_random_bytes"); }
