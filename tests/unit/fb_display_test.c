/* Test the real framebuffer -> OpenLogit adapter, not a lookalike display.
 * The 5K case MUST run first: a prior small bind used to initialize channel
 * positions and hide the rejected large bind. Hardware and text services are
 * stubbed; packed pixel drawing, fb_init and backbuffer binding are real.
 */
#include "fb.h"
#include "openlogit_display.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *kmalloc(unsigned long size) { return malloc(size); }
void kfree(void *pointer) { free(pointer); }
void vmm_map_range(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{ (void)a; (void)b; (void)c; (void)d; }
int virtio_gpu_init(void) { return -1; }
uint32_t *virtio_gpu_fb(void) { return NULL; }
uint32_t virtio_gpu_width(void) { return 0; }
uint32_t virtio_gpu_height(void) { return 0; }
int virtio_gpu_present(void) { return 0; }
void virtio_gpu_flush(int x, int y, int w, int h)
{ (void)x; (void)y; (void)w; (void)h; }
int virtio_gpu_cursor_ready(void) { return 0; }
int virtio_gpu_cursor_define(const uint32_t *pixels, int w, int h, int x, int y)
{ (void)pixels; (void)w; (void)h; (void)x; (void)y; return -1; }
void virtio_gpu_cursor_move(int x, int y) { (void)x; (void)y; }
void text_draw_sz(int x, int y, const char *text, int size, uint32_t color)
{ (void)x; (void)y; (void)text; (void)size; (void)color; }
int text_width_sz(const char *text, int size) { (void)text; (void)size; return 0; }
int text_line_height(int size) { return size; }

static int checks, failures;

static void check(int condition, const char *name)
{
    checks++;
    if (!condition) {
        failures++;
        printf("FAIL %s\n", name);
    }
}

static void start_framebuffer(uint32_t *pixels, int width, int height,
                              unsigned char red, unsigned char green, unsigned char blue)
{
    /* Multiboot's RGB tag is a wire layout, including its 8-byte alignment. */
    _Alignas(8) unsigned char info[64] = {0};
    uint32_t header[] = {sizeof info, 0, 8, 40};
    uint64_t address = (uint64_t)(uintptr_t)pixels;
    uint32_t dimensions[] = {(uint32_t)width * 4, (uint32_t)width, (uint32_t)height};
    memcpy(info, header, sizeof header);
    memcpy(info + 16, &address, sizeof address);
    memcpy(info + 24, dimensions, sizeof dimensions);
    info[36] = 32;
    info[37] = 1;
    info[40] = red;   info[41] = 8;
    info[42] = green; info[43] = 8;
    info[44] = blue;  info[45] = 8;
    if (!fb_init((uint64_t)(uintptr_t)info)) {
        fprintf(stderr, "fixture: fb_init rejected RGB framebuffer metadata\n");
        exit(2);
    }
    fb_set_backbuffer(pixels);
}

static uint32_t *allocate_pixels(size_t count)
{
    uint32_t *pixels = calloc(count, sizeof *pixels);
    if (!pixels) {
        fprintf(stderr, "fixture: cannot allocate %zu pixels\n", count);
        exit(2);
    }
    return pixels;
}

static void fresh_5k(void)
{
    enum { WIDTH = 5120, HEIGHT = 2880 };
    uint32_t *pixels = allocate_pixels((size_t)WIDTH * HEIGHT);
    start_framebuffer(pixels, WIDTH, HEIGHT, 16, 8, 0);
    fb_blend_rect(0, 0, 1, 1, 255, 128, 64, 255);
    check(pixels[0] == 0xff8040, "fresh 5K bind initializes RGB channels");
    fb_blend_rect(WIDTH - 1, HEIGHT - 1, 1, 1, 12, 34, 56, 255);
    check(pixels[(size_t)WIDTH * HEIGHT - 1] == 0x0c2238,
          "fresh 5K bind draws the last pixel");
    check(pixels[1] == 0 && pixels[(size_t)WIDTH * HEIGHT - 2] == 0,
          "fresh 5K one-pixel draws preserve their neighbors");
    printf("fresh 5K pixels: first=%06x last=%06x\n", pixels[0],
           pixels[(size_t)WIDTH * HEIGHT - 1]);
    free(pixels);
}

static void size_boundaries(void)
{
    const int dimensions[][2] = {
        {4096, 2}, {4097, 2}, {8192, 2},
        {2, 4096}, {2, 4097}, {2, 8192}, {8192, 8192},
    };
    check(OL_DISPLAY_MAX_SIZE == 8192, "packed display has its own 8192 limit");
    for (unsigned i = 0; i < sizeof dimensions / sizeof *dimensions; i++) {
        int width = dimensions[i][0], height = dimensions[i][1];
        size_t count = (size_t)width * height;
        uint32_t *pixels = allocate_pixels(count);
        start_framebuffer(pixels, width, height, 16, 8, 0);
        fb_blend_rect(width - 1, height - 1, 1, 1, 18, 52, 86, 255);
        char name[96];
        snprintf(name, sizeof name, "framebuffer %dx%d reaches final packed pixel", width, height);
        check(pixels[count - 1] == 0x123456, name);
        check(pixels[0] == 0 && pixels[count - 2] == 0,
              "boundary draw preserves neighboring pixels");
        free(pixels);
    }
}

static void rebind_target(void)
{
    uint32_t first[64] = {0}, second[64] = {0};
    start_framebuffer(first, 8, 8, 16, 8, 0);
    fb_blend_rect(0, 0, 1, 1, 255, 128, 64, 255);
    fb_blend_rect(1, 0, 1, 1, 255, 128, 64, 255);
    check(first[0] == 0xff8040 && first[1] == 0xff8040,
          "repeated draw retains the same validated target");
    fb_set_backbuffer(second);
    fb_blend_rect(0, 0, 1, 1, 12, 34, 56, 255);
    check(first[0] == 0xff8040 && second[0] == 0x0c2238,
          "backbuffer rebinding leaves the old target unchanged");

    struct surface target = {.px = first, .w = 4, .h = 16};
    fb_target(&target);
    fb_blend_rect(3, 15, 1, 1, 12, 34, 56, 255);
    check(first[63] == 0x0c2238, "offscreen target uses its own dimensions");
    target.w = 16;
    target.h = 4;
    fb_blend_rect(15, 3, 1, 1, 18, 52, 86, 255);
    check(first[63] == 0x123456, "in-place target dimension change rebinds");
    target.px = second;
    fb_blend_rect(15, 3, 1, 1, 98, 76, 54, 255);
    check(first[63] == 0x123456 && second[63] == 0x624c36,
          "in-place pixel pointer change rebinds");

    start_framebuffer(first, 8, 8, 0, 8, 16);
    fb_blend_rect(0, 0, 1, 1, 255, 128, 64, 255);
    check(first[0] == 0x4080ff, "same backbuffer adopts changed RGB channel layout");
    start_framebuffer(first, 8, 8, 16, 8, 0);
    fb_blend_rect(0, 0, 1, 1, 255, 128, 64, 255);
    check(first[0] == 0xff8040, "channel layout can change back without stale state");
}

static void draw_after_failure(void)
{
    /* Repeat calls exercise the cache path as well as the first failed bind.
     * Small draws keep deliberate invalid-dimension controls safe to inspect. */
    for (int i = 0; i < 3; i++) {
        fb_blend_rect(0, 0, 1, 1, 255, 128, 64, 255);
        fb_put(0, 0, 0x123456);
    }
}

static void invalid_targets(void)
{
    uint32_t previous[64], candidate[64], unchanged[64];
    for (int i = 0; i < 64; i++) unchanged[i] = 0x345678;
    const int dimensions[][2] = {{8193, 1}, {1, 8193}, {0, 1}, {1, 0}, {-1, 1}, {1, -1}};
    for (unsigned i = 0; i < sizeof dimensions / sizeof *dimensions; i++) {
        memcpy(previous, unchanged, sizeof previous);
        memcpy(candidate, unchanged, sizeof candidate);
        start_framebuffer(previous, 8, 8, 16, 8, 0);
        fb_put(0, 0, unchanged[0]);
        struct surface invalid = {.px = candidate, .w = dimensions[i][0], .h = dimensions[i][1]};
        fb_target(&invalid);
        draw_after_failure();
        check(!memcmp(previous, unchanged, sizeof previous), "failed bind never writes the previous target");
        check(!memcmp(candidate, unchanged, sizeof candidate), "failed bind never writes the unvalidated target");
        fb_set_backbuffer(candidate);
        fb_blend_rect(0, 0, 1, 1, 255, 128, 64, 255);
        check(candidate[0] == 0xff8040, "valid target recovers after rejected dimensions");
    }

    memcpy(previous, unchanged, sizeof previous);
    start_framebuffer(previous, 8, 8, 16, 8, 0);
    fb_put(0, 0, unchanged[0]);
    fb_set_backbuffer(NULL);
    draw_after_failure();
    check(!memcmp(previous, unchanged, sizeof previous), "null backbuffer failure does not retain the previous target");

    memcpy(candidate, unchanged, sizeof candidate);
    start_framebuffer(candidate, 8, 8, 8, 8, 0);
    draw_after_failure();
    check(!memcmp(previous, unchanged, sizeof previous), "invalid layout leaves the previous target unchanged");
    check(!memcmp(candidate, unchanged, sizeof candidate), "invalid layout leaves the unvalidated target unchanged");
    start_framebuffer(candidate, 8, 8, 16, 8, 0);
    fb_blend_rect(0, 0, 1, 1, 255, 128, 64, 255);
    check(candidate[0] == 0xff8040, "valid channel layout recovers after rejected overlap");
}

static void direct_bind_contract(void)
{
    uint32_t old_pixels[4] = {0}, rejected_pixels[4] = {0};
    struct ol_display display = {0};
    struct ol_pixel_target old = {.px = old_pixels, .w = 2, .h = 2};
    check(ol_display_bind(&display, &old, 16, 8, 0) == OL_OK, "generic display accepts valid target");
    struct ol_display saved;
    memcpy(&saved, &display, sizeof saved);
    struct ol_pixel_target invalid = {.px = rejected_pixels, .w = 8193, .h = 1};
    check(ol_display_bind(&display, &invalid, 0, 8, 16) == OL_ARGUMENT,
          "generic display rejects dimensions outside packed limit");
    check(!memcmp(&display, &saved, sizeof display),
          "generic failed bind preserves its context contract");
    /* The generic API leaves recovery to its caller. Only fb's compatibility
     * wrapper revokes the old target because its drawing API cannot fail. */
    ol_display_put(&display, 0, 0, 0x123456);
    check(old_pixels[0] == 0x123456 && rejected_pixels[0] == 0,
          "generic caller can retain its previously validated target");
    ol_display_destroy(&display);
}

int main(void)
{
    fresh_5k();
    size_boundaries();
    rebind_target();
    invalid_targets();
    direct_bind_contract();
    printf("fb display: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
