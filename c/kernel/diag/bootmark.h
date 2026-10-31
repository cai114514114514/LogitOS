/* Boot breadcrumbs on the screen: one square per stage, top left, for a
 * machine whose only output is the display (no serial header reachable).
 * Squares 1-2 are the UEFI loader's (yellow): ExitBootServices done, about to
 * jump. From 3 on they are the kernel's (alternating green and cyan); the
 * stage of each index is listed in bootmark.c. Count the squares on a photo
 * of a stopped boot to know the last stage reached. X79-VG4 bring-up. */
#ifndef BOOTMARK_H
#define BOOTMARK_H
#include <stdint.h>

/* Find the boot framebuffer in the normalized boot block. If it lies above
 * the loader's 1-GiB identity runway, map its 1-GiB region with a static
 * page directory until bootmark_early_end(). */
void bootmark_init(uint64_t info);
/* Remove the temporary mapping; called before pmm_init() edits the tables. */
void bootmark_early_end(void);
/* After fb_init(): draw again through the kernel's own framebuffer mapping. */
void bootmark_resume(void);
void bootmark(int index);

/* The kernel log on the screen as it is written (kprintf's console sink
 * calls this for every byte): lines in the built-in 8x16 font below the
 * squares, wrapping to the top when the screen is full, the line after the
 * newest kept blank. Live whenever the squares are; bootcon_stop() hands the
 * screen to the desktop or the boot report. */
void bootcon_putc(char c);
void bootcon_stop(void);

#endif
