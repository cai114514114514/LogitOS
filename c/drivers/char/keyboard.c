#include <stdint.h>
#include "keyboard.h"
#include "io.h"
#include "wm.h"
#include "aether_abi.h"   /* KEY_* codes for arrows / page keys */

#define KBD_DATA 0x60

/* US QWERTY, scancode set 1, unshifted. 0 = no printable character. */
static const char scancode_map[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x29] = '`', [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' ',
};

/* US QWERTY, scancode set 1, shifted layer (Shift held). */
static const char scancode_map_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}', [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = ':',
    [0x28] = '"', [0x29] = '~', [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x39] = ' ',
};

#define SC_LCTRL  0x1D
#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36

void keyboard_handle(void)
{
    static int ctrl, shift, ext;
    uint8_t sc = inb(KBD_DATA);

    if (sc == 0xE0) { ext = 1; return; }            /* extended-key prefix */
    if (ext) {                                       /* arrows / page / home / end */
        ext = 0;
        if (sc & 0x80) return;                       /* release */
        int k = 0;
        switch (sc) {
            case 0x48: k = KEY_UP;   break;  case 0x50: k = KEY_DOWN;  break;
            case 0x49: k = KEY_PGUP; break;  case 0x51: k = KEY_PGDN;  break;
            case 0x47: k = KEY_HOME; break;  case 0x4F: k = KEY_END;   break;
            case 0x4B: k = KEY_LEFT; break;  case 0x4D: k = KEY_RIGHT; break;
        }
        if (k) wm_key(k);
        return;
    }

    switch (sc) {
    case SC_LCTRL:                                  ctrl  = 1; return;
    case SC_LCTRL  | 0x80:                          ctrl  = 0; return;
    case SC_LSHIFT:        case SC_RSHIFT:          shift = 1; return;
    case SC_LSHIFT | 0x80: case SC_RSHIFT | 0x80:   shift = 0; return;
    }
    if (sc & 0x80)
        return;                      /* other key releases */

    char base = scancode_map[sc & 0x7F];
    if (!base)
        return;
    if (ctrl && base >= 'a' && base <= 'z') {
        wm_key(base - 'a' + 1);      /* Ctrl+letter -> control code (Ctrl+S=0x13) */
        return;
    }
    char c = shift ? scancode_map_shift[sc & 0x7F] : base;
    if (c)
        wm_key(c);                   /* route to the focused UI */
}
