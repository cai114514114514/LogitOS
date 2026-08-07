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

static uint8_t mouse_read(void)  /* one byte the device sent unprompted (e.g. a device ID) */
{
    wait_output();
    return inb(PS2_DATA);
}

/* 3 for a plain PS/2 mouse, 4 once the wheel is on (packets grow by a byte). */
static int packet_len = 3;

/* Turn on the Microsoft "IntelliMouse" extension: a device that sees the sample
 * rates 200, 100, 80 in a row changes its reported ID from 0 to 3 and starts
 * sending FOUR-byte packets whose extra byte is the scroll wheel. There is no
 * feature bit to query first -- the knock IS the query, and the ID afterwards is
 * the answer, which is why this reads the ID rather than assuming.
 *
 * Without it a PS/2 mouse has no wheel at all (the 3-byte packet has no room for
 * one), so EV_WHEEL would be a constant the kernel could never generate. */
static void mouse_enable_wheel(void)
{
    mouse_write(0xF3); mouse_write(200);
    mouse_write(0xF3); mouse_write(100);
    mouse_write(0xF3); mouse_write(80);
    mouse_write(0xF2);                        /* get device ID */
    if (mouse_read() == 3) packet_len = 4;
    mouse_write(0xF3); mouse_write(100);      /* back to a sane report rate */
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
    mouse_enable_wheel();       /* ... then ask for the wheel, which F6 does not undo */
    mouse_write(0xF4);          /* enable data reporting */
    drain();
}

static uint8_t packet[4];
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
    if (cycle < packet_len)
        return;
    cycle = 0;

    int dx = (int)packet[1] - ((packet[0] << 4) & 0x100);
    int dy = (int)packet[2] - ((packet[0] << 3) & 0x100);
    int left = packet[0] & 0x01;
    int right = packet[0] & 0x02;
    int middle = packet[0] & 0x04;
    /* Wheel notches this packet. Signed 8-bit; QEMU (like real hardware) sends
     * +1 for a scroll toward the user and -1 away, which is already the sign
     * EV_WHEEL and the DOM's deltaY use -- so no negation here, on purpose. */
    int dz = packet_len == 4 ? (int)(int8_t)packet[3] : 0;

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

    /* Buttons are reported as a LEVEL in every packet, not as transitions: this
     * driver has never known "went down" from "still down", and it stays that
     * way -- the WM already tracks the previous level to derive edges, and it is
     * the side that knows which window owns the press. */
    wm_mouse_event(mx, my, left, right, middle, dz);
}
