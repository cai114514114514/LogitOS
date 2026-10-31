/* The boot log on the screen, for a machine with no serial port to watch and
 * no root filesystem to keep the log on. X79-VG4 bring-up, 2026-10-31.
 * QEMU evidence only; not verified on X79-VG4 hardware. */
#ifndef HWSCREEN_H
#define HWSCREEN_H

/* Take over the framebuffer and show the kernel log ring as pages of text in
 * a built-in bitmap font (no font files needed), summary lines ([hw], [board],
 * [efi], [ahci], [blk], [fs], [cpu], [mce]) first, then the whole log. Each
 * page stays up for HWSCREEN_PAGE_MS so it can be photographed; the cycle
 * repeats, re-reading the ring each time. Never returns. */
void hwscreen_report(void) __attribute__((noreturn));

/* Pure layout, exposed for the host test: wrap `text` (lines separated by
 * '\n') at `cols` into out[][cols+1], at most `max_lines`. Returns the count. */
int hwscreen_wrap(const char *text, int cols, char (*out)[161], int max_lines);

#define HWSCREEN_PAGE_MS 12000

#endif
