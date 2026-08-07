/* The two allocator symbols the browser's ring-3 sources call, provided for
 * host test binaries that link those sources but whose main() does not define
 * them itself.
 *
 * It exists because css_vars.c stopped being a self-contained text pass: its
 * @media verdict now comes from LibCSS (css_media_matches -> css_engine.c ->
 * css_select_ctx_media_matches), and pulling css_engine.c in pulls dom.c in
 * with it. Every test that already defines kmalloc/kfree in its own main()
 * simply does not link this file. */
#include <stdlib.h>

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
