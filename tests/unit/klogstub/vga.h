#ifndef LOGIT_VGA_H
#define LOGIT_VGA_H
/* host-test stub: see tests/unit/log_test.c */
enum vga_color { VGA_BLACK = 0, VGA_RED = 4, VGA_WHITE = 15 };
void vga_clear(void);
void vga_set_color(enum vga_color fg, enum vga_color bg);
void vga_putc(char c);
void vga_puts(const char *s);
#endif
