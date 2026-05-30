#ifndef AQUA_KPRINTF_H
#define AQUA_KPRINTF_H

/* Minimal formatted output. Writes to both the VGA text screen and COM1.
 * Supported conversions: %s %c %d %u %x %p %%  */

void kprintf(const char *fmt, ...);

#endif /* AQUA_KPRINTF_H */
