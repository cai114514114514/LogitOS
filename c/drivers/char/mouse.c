#include <stdint.h>
#include "mouse.h"
#include "io.h"
#include "fb.h"
#include "wm.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

static void wait_input(void)   /* controller ready to accept a byte */
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(PS2_STATUS) & 0x02))
            return;
}

static void wait_output(void)  /* controller has a byte for us */
{
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & 0x01)
            return;
}

static void drain(void)         /* discard any pending controller output */
{
    for (int i = 0; i < 64 && (inb(PS2_STATUS) & 0x01); i++)
        (void)inb(PS2_DATA);
}

static void mouse_write(uint8_t b)
{
    wait_input();
    outb(PS2_CMD, 0xD4);        /* next byte goes to the mouse */
    wait_input();
    outb(PS2_DATA, b);
    wait_output();
    (void)inb(PS2_DATA);        /* consume ACK (0xFA) */
}

void mouse_init(void)
{
    drain();
    wait_input();
    outb(PS2_CMD, 0xA8);        /* enable auxiliary (mouse) port */

    /* Enable IRQ12 in the controller config byte. */
    wait_input();
    outb(PS2_CMD, 0x20);
    wait_output();
    uint8_t cfg = inb(PS2_DATA);
    cfg |= 0x02;                /* enable mouse interrupt */
    cfg &= ~0x20;               /* enable mouse clock */
    wait_input();
    outb(PS2_CMD, 0x60);
    wait_input();
    outb(PS2_DATA, cfg);
    drain();                    /* flush self-test/id bytes (0xAA, 0x00, ...) */

    mouse_write(0xF6);          /* set defaults */
    mouse_write(0xF4);          /* enable data reporting */
    drain();
}

static uint8_t packet[3];
static int cycle;
static int mx, my;
static int initialized;

void mouse_handle(void)
{
    uint8_t data = inb(PS2_DATA);

    /* Resync: a real first byte has the always-1 bit (0x08) set and both
     * overflow bits (0xC0) clear. This rejects stray ACK (0xFA), self-test
     * (0xAA) and other out-of-band bytes. */
    if (cycle == 0 && ((data & 0x08) == 0 || (data & 0xC0) != 0))
        return;

    packet[cycle++] = data;
    if (cycle < 3)
        return;
    cycle = 0;

    int dx = (int)packet[1] - ((packet[0] << 4) & 0x100);
    int dy = (int)packet[2] - ((packet[0] << 3) & 0x100);
    int left = packet[0] & 0x01;
    int right = packet[0] & 0x02;

    if (!initialized) {
        mx = (int)fb_width() / 2;
        my = (int)fb_height() / 2;
        initialized = 1;
    }

    mx += dx;
    my -= dy;                   /* PS/2 Y grows upward */

    int w = (int)fb_width(), h = (int)fb_height();
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx > w - 1) mx = w - 1;
    if (my > h - 1) my = h - 1;

    wm_mouse_event(mx, my, left, right);
}
