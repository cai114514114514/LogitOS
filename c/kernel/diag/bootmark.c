/* See bootmark.h. Index -> stage (kernel_main order):
 *   1  loader: ExitBootServices accepted            (c/boot/efi/loader.c)
 *   2  loader: about to jump to the kernel
 *   3  kernel_main entered
 *   4  IDT, GDT, PIC remap
 *   5  PIT programmed
 *   6  SIMD self-test passed (the last early square)
 *   7  framebuffer mapped by fb_init()
 *   8  block devices probed (blk_init)
 *   9  root mount attempted
 *  10  root result reported (hwreport_dump on a missing root)
 *  11  /proc and the page-cache backend installed
 *  12  trust banner printed
 *  13  NICs probed (netdev_init, inside net_init)
 *  14  net_init returned
 *  15  fonts loaded (text_init)
 *  16  image codecs registered (img_init)
 *  17  window manager initialised (wm_init)
 *  18  first desktop frame (wm_render)
 *  19  other CPUs brought up (smp_init)
 *  20  device drivers bound (dev_probe_all)
 *  21  boot report about to take the screen (no root filesystem)
 * The X79-VG4's first kernel boot (2026-10-31) stopped after square 9.
 * Squares are 24x24 at x = 8 + 30 * (index - 1), y = 8, written straight to
 * the scanout memory. Not verified on X79-VG4 hardware. */
#include <stddef.h>
#include "bootmark.h"
#include "fb.h"
#include "kprintf.h"
#include "hwscreen_font.h"

static volatile uint8_t *g_lfb;
static uint32_t g_pitch, g_w, g_h;
static uint64_t g_addr;
static uint64_t *g_pdpt;
static int g_slot = -1;
static uint64_t g_pd[512] __attribute__((aligned(4096)));

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

void bootmark_init(uint64_t info)
{
    if (!info) return;
    const uint8_t *base = (const uint8_t *)(uintptr_t)info;
    uint32_t total = rd32(base);
    if (total < 16 || total > 0x100000) return;
    for (uint32_t off = 8; off + 8 <= total; ) {
        uint32_t type = rd32(base + off), size = rd32(base + off + 4);
        if (type == 0 || size < 8 || size > total - off) return;
        if (type == 8 && size >= 29) {                   /* normalized framebuffer tag */
            g_addr = rd32(base + off + 8) | (uint64_t)rd32(base + off + 12) << 32;
            g_pitch = rd32(base + off + 16);
            g_w = rd32(base + off + 20);
            g_h = rd32(base + off + 24);
            if (base[off + 28] != 32 || !g_addr || g_w < 400 || g_h < 40) { g_addr = 0; return; }
            break;
        }
        off += (size + 7) & ~7u;
    }
    if (!g_addr) return;
    uint64_t end = g_addr + (uint64_t)g_pitch * 40;      /* only the top rows are touched */
    if (end <= 0x40000000ull) { g_lfb = (volatile uint8_t *)(uintptr_t)g_addr; return; }
    if ((g_addr >> 30) != ((end - 1) >> 30) || (g_addr >> 30) >= 512) return;
    /* the loader's tables: PML4[0] -> PDPT; its first GiB is the runway */
    uint64_t cr3; __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = (uint64_t *)(uintptr_t)(cr3 & 0x000ffffffffff000ull);
    if (!(pml4[0] & 1)) return;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[0] & 0x000ffffffffff000ull);
    int slot = (int)(g_addr >> 30);
    if (pdpt[slot] & 1) return;
    for (int i = 0; i < 512; i++)                        /* 2 MiB pages, uncached */
        g_pd[i] = ((uint64_t)slot << 30) + ((uint64_t)i << 21) | 0x83 | 0x18;
    pdpt[slot] = (uint64_t)(uintptr_t)g_pd | 3;           /* the kernel image is identity-mapped */
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
    g_pdpt = pdpt; g_slot = slot;
    g_lfb = (volatile uint8_t *)(uintptr_t)g_addr;
}

void bootmark_early_end(void)
{
    if (g_slot >= 0) {
        g_pdpt[g_slot] = 0;
        uint64_t cr3; __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
        g_slot = -1;
    }
    g_lfb = NULL;
}

void bootmark_resume(void)
{
    uint64_t addr, bytes;
    if (g_addr && fb_boot_lfb_range(&addr, &bytes) == 1 && addr == g_addr)
        g_lfb = (volatile uint8_t *)(uintptr_t)addr;
}

void bootmark(int index)
{
    kprintf("[mark] %d\n", index);
    if (!g_lfb || index < 1) return;
    uint32_t x0 = 8 + 30 * (uint32_t)(index - 1);
    if (x0 + 24 > g_w) return;
    uint32_t c = (index & 1) ? 0x0000e070u : 0x0000c0e0u;  /* green / cyan (xRGB and xBGR alike) */
    for (uint32_t y = 8; y < 32; y++) {
        volatile uint32_t *row = (volatile uint32_t *)(g_lfb + (uint64_t)y * g_pitch);
        for (uint32_t x = x0; x < x0 + 24; x++) row[x] = c;
    }
}

/* ---- the live console ---------------------------------------------------- */
static int g_con_x, g_con_y, g_con_off;

static void con_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c)
{
    for (uint32_t yy = y; yy < y + h && yy < g_h; yy++) {
        volatile uint32_t *row = (volatile uint32_t *)(g_lfb + (uint64_t)yy * g_pitch);
        for (uint32_t xx = x; xx < x + w && xx < g_w; xx++) row[xx] = c;
    }
}

void bootcon_putc(char c)
{
    if (!g_lfb || g_con_off) return;
    uint32_t s = g_w >= 1600 ? 2 : 1, cw = 8 * s, ch = 16 * s, top = 40;
    if (g_h <= top + ch) return;
    uint32_t cols = g_w / cw, rows = (g_h - top) / ch;
    if (c == '\r') return;
    if (c == '\n' || g_con_x >= (int)cols) {
        g_con_x = 0;
        g_con_y = (g_con_y + 1) % (int)rows;
        con_fill(0, top + (uint32_t)g_con_y * ch, g_w, ch, 0x00101418u);              /* the new line */
        con_fill(0, top + (uint32_t)((g_con_y + 1) % (int)rows) * ch, g_w, ch, 0x00101418u);  /* gap after it */
        if (c == '\n') return;
    }
    if (c < 0x20 || c > 0x7e) c = '?';
    const uint8_t *g = hwscreen_font[c - 0x20];
    uint32_t x0 = (uint32_t)g_con_x * cw, y0 = top + (uint32_t)g_con_y * ch;
    for (uint32_t r = 0; r < 16; r++)
        for (uint32_t b = 0; b < 8; b++)
            con_fill(x0 + b * s, y0 + r * s, s, s, (g[r] & (0x80 >> b)) ? 0x00d8dde6u : 0x00101418u);
    g_con_x++;
}

void bootcon_stop(void) { g_con_off = 1; }
