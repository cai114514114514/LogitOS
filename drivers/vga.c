#include <stdint.h>
#include "vga.h"

#define VGA_WIDTH   80
#define VGA_HEIGHT  25

static volatile uint16_t *const VGA = (volatile uint16_t *)0xB8000;
static int cursor_row = 0;
static int cursor_col = 0;
static uint8_t color = (VGA_BLACK << 4) | VGA_LIGHT_GREY;

static inline uint16_t cell(char c, uint8_t attr)
{
    return (uint16_t)(unsigned char)c | ((uint16_t)attr << 8);
}

void vga_set_color(enum vga_color fg, enum vga_color bg)
{
    color = (uint8_t)((bg << 4) | (fg & 0x0F));
}

void vga_clear(void)
{
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA[i] = cell(' ', color);
    cursor_row = 0;
    cursor_col = 0;
}

static void scroll(void)
{
    for (int row = 1; row < VGA_HEIGHT; row++)
        for (int col = 0; col < VGA_WIDTH; col++)
            VGA[(row - 1) * VGA_WIDTH + col] = VGA[row * VGA_WIDTH + col];

    for (int col = 0; col < VGA_WIDTH; col++)
        VGA[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = cell(' ', color);

    cursor_row = VGA_HEIGHT - 1;
}

void vga_putc(char c)
{
    switch (c) {
    case '\n':
        cursor_col = 0;
        cursor_row++;
        break;
    case '\r':
        cursor_col = 0;
        break;
    case '\t':
        cursor_col = (cursor_col + 4) & ~3;
        break;
    default:
        VGA[cursor_row * VGA_WIDTH + cursor_col] = cell(c, color);
        cursor_col++;
        break;
    }

    if (cursor_col >= VGA_WIDTH) {
        cursor_col = 0;
        cursor_row++;
    }
    if (cursor_row >= VGA_HEIGHT)
        scroll();
}

void vga_puts(const char *s)
{
    while (*s)
        vga_putc(*s++);
}
