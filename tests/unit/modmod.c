/* A module built to be RUN by the host test, not merely inspected.
 *
 * It is compiled by the CROSS compiler with the kernel's own flags (see
 * tests/module.mk) and then loaded, relocated and CALLED by an ordinary Linux
 * program. That works because both sides are x86-64 SysV -- the kernel's
 * -ffreestanding -mno-red-zone -fno-pic object is callable from a host binary
 * as long as its relocations were applied correctly, which is precisely the
 * claim under test. Inspecting a relocated pointer proves the loader wrote a
 * plausible number there; executing the code proves it wrote the right one.
 *
 * Every field below exists to force ONE relocation type out of the compiler.
 * The set was not guessed: it is the set measured on c/drivers/core/qemu_edu.c
 * and the two virtio drivers (see c/kernel/module/module.h).
 *
 *   mt_ext / mt_log calls            R_X86_64_PLT32
 *   g_counter (.bss) read-modify     R_X86_64_PC32   (rip-relative)
 *   "literal" string argument        R_X86_64_32     (absolute, zero-extended)
 *   g_table[] indexed load           R_X86_64_32S    (absolute, sign-extended)
 *   g_msgp = g_hello (.data)         R_X86_64_64
 *   mt_ref in section "logit_drivers"  R_X86_64_64 in a NAMED section, which
 *                                    is the exact mechanism DRIVER_DECLARE
 *                                    uses and the one thing the loader must
 *                                    find by name rather than by index.
 *
 * The host test asserts the exact type census, so if a compiler upgrade stops
 * emitting one of these the test says which one disappeared instead of
 * quietly covering four cases and reporting five. */

#include <stdint.h>

/* Resolved by the host test's own table -- the stand-in for kprintf/dev_*. */
extern int  mt_ext(int x);
extern void mt_log(const char *s);

static int g_counter;                       /* .bss: must arrive ZEROED */
static const char g_hello[] = "hello-from-module";
static int g_table[4] = { 10, 20, 30, 40 }; /* .data */

/* .data holding a pointer into .rodata: the R_X86_64_64 that a `struct
 * driver`'s ->name field is. */
const char *g_msgp = g_hello;

/* The DRIVER_DECLARE shape, reduced to its mechanism: a pointer into another
 * of the module's own sections, sitting in a section the loader locates by
 * NAME. Deliberately the same section name a real driver uses, because the
 * loader's lookup is a string compare and a test using a different name would
 * not exercise it. */
static int g_marker = 0xC0FFEE;
int *const mt_ref __attribute__((used, section("logit_drivers"))) = &g_marker;

/* The entry point. Returns a value that can only be right if the .bss write,
 * the .data read, the .rodata pointer and the external call all landed.
 *
 * Called TWICE by the host test with the same arguments and required to return
 * a DIFFERENT value the second time: that is what proves g_counter is real
 * writable memory inside the loaded block rather than a constant the compiler
 * folded, and it is the check that a loader which forgot to copy .data (or
 * mapped it read-only) cannot pass. */
int mt_entry(int a, int b)
{
    g_counter += a;
    mt_log("literal");              /* forces a string-literal address */
    return mt_ext(a * b) + g_counter + g_table[b & 3];
}

/* Read back the .rodata pointer through .data, so the host can check the
 * R_X86_64_64 by content and not only by "it points somewhere inside". */
const char *mt_msg(void) { return g_msgp; }

int mt_marker(void) { return g_marker; }
