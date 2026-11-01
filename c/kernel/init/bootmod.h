#ifndef LOGIT_BOOTMOD_H
#define LOGIT_BOOTMOD_H

#include <stdint.h>

/* Boot modules: files the UEFI loader read off the boot volume (the USB ESP)
 * before ExitBootServices and left in RAM for the kernel. Today that is the
 * AMD Polaris microcode, so the stick delivers it even when the root disk,
 * which the owner can only reinstall from Windows, carries none.
 *
 * The registry is read-only after bootmod_init and holds no copy of any file:
 * a module is the loader's own pages, named by the LOGIT_BOOT_TAG_MODULE tags
 * in the boot information block. */

/* Parse the module tags of the NORMALIZED boot information block (the view
 * kmain passes to pmm_init), register every well-formed one, hold its pages
 * out of the allocator and print "[bootmod] <name> <bytes> bytes" for each.
 * Malformed tags are skipped with a printed reason and never registered.
 *
 * MUST run before pmm_init: that function frees every page the firmware map
 * calls AVAILABLE, and the loader's pages carry exactly that type. Needs
 * kprintf but no allocator, so it can run that early. Safe to call twice; the
 * second call replaces the registry (host tests rely on this). */
void bootmod_init(uint64_t info);

/* The module whose file name is NAME, or NULL. *bytes (if non-NULL) receives
 * its length. The pointer is read-only by contract (const) and by the pin
 * check every consumer applies; it is NOT write-protected in the page tables,
 * because the direct map is built from 2 MiB leaves shared with neighbouring
 * RAM and splitting them for one file is not worth the risk it adds to
 * pmm_init. NULL also covers "registered but not yet reachable": a module
 * above 1 GiB is only addressable once the direct map is up. */
const void *bootmod_find(const char *name, uint64_t *bytes);

/* How many modules are registered (for the boot log and tests). */
unsigned bootmod_count(void);

#endif
