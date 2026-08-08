#include <stdint.h>
#include "keyboard.h"
#include "io.h"
#include "wm.h"
#include "logit_abi.h"   /* KEY_* codes for arrows / page keys */

#define KBD_DATA 0x60

/* US QWERTY, scancode set 1, unshifted. 0 = no printable character. */
static const char scancode_map[128] = {
    /* Escape. Not printable, but every app already treats key 27 as cancel:
     * aui dismisses popups and modals on it, Finder closes its menu and Get
     * Info. All of that was dead code -- the one scancode nobody had mapped. */
    [0x01] = 27,
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
    [0x01] = 27,                       /* Shift+Esc is still Esc */
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

/* Base-page (unprefixed) modifier scancodes. */
#define SC_LCTRL  0x1D
#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_LALT   0x38

/* E0-prefixed modifier scancodes, i.e. the right-hand keys and the two COMMAND
 * keys. The set-1 encoding reuses the base code after the 0xE0 byte, so `E0 1D`
 * is right Ctrl and 0x1D alone is left Ctrl -- which is exactly why the prefix
 * has to be remembered rather than the byte matched on its own. */
#define SC_E0_RCTRL 0x1D
#define SC_E0_RALT  0x38
#define SC_E0_LGUI  0x5B
#define SC_E0_RGUI  0x5C

/* Modifier state, at file scope because it is no longer only this driver's
 * business: every key AND mouse event carries an EV_MOD_* mask, and the mouse
 * IRQ has no other way to know whether shift is down. Read from IRQ context
 * (both IRQ1 and IRQ12), written only by IRQ1 -- a torn read is not possible for
 * a single int, and a modifier that changes in the same microsecond as a click
 * is genuinely ambiguous anyway.
 *
 * ONE BIT PER PHYSICAL KEY, not one flag per modifier. The old code kept a
 * single `mod_shift` that both Shift keys set and both cleared, so holding both
 * and releasing either un-shifted the keyboard while a key was still physically
 * down. That was invisible while nothing combined modifiers; it stops being
 * invisible the moment Cmd+Shift+something exists. The same shape covers the
 * right-hand Ctrl and Alt, which this driver used to drop on the floor -- the
 * comment that used to sit here said so and called it someone else's problem.
 * It is this file's problem and it is four lines. */
#define MK_LSHIFT 0x01
#define MK_RSHIFT 0x02
#define MK_LCTRL  0x04
#define MK_RCTRL  0x08
#define MK_LALT   0x10
#define MK_RALT   0x20
#define MK_LSUPER 0x40
#define MK_RSUPER 0x80
static int mod_keys;

static void modk(int bit, int down)
{
    if (down) mod_keys |= bit;
    else      mod_keys &= ~bit;
}

int kbd_mods(void)
{
    return ((mod_keys & (MK_LSHIFT | MK_RSHIFT)) ? EV_MOD_SHIFT : 0) |
           ((mod_keys & (MK_LCTRL  | MK_RCTRL))  ? EV_MOD_CTRL  : 0) |
           ((mod_keys & (MK_LALT   | MK_RALT))   ? EV_MOD_ALT   : 0) |
           ((mod_keys & (MK_LSUPER | MK_RSUPER)) ? EV_MOD_SUPER : 0);
}

void keyboard_handle(void)
{
    static int ext;
    uint8_t sc = inb(KBD_DATA);

    if (sc == 0xE0) { ext = 1; return; }            /* extended-key prefix */
    if (ext) {                                       /* arrows / page / home / end / Cmd */
        ext = 0;
        int rel = sc & 0x80, code = sc & 0x7F;
        /* Modifiers are matched BEFORE the release filter below. That filter is
         * where the COMMAND key used to die: `if (sc & 0x80) return;` swallowed
         * every extended release, so a modifier tracked here would have latched
         * down forever after the first press. A key whose whole job is to be
         * held has to see both edges. */
        switch (code) {
        case SC_E0_LGUI:  modk(MK_LSUPER, !rel); return;
        case SC_E0_RGUI:  modk(MK_RSUPER, !rel); return;
        case SC_E0_RCTRL: modk(MK_RCTRL,  !rel); return;
        case SC_E0_RALT:  modk(MK_RALT,   !rel); return;
        }
        if (rel) return;                             /* other extended releases */
        int k = 0;
        switch (code) {
            case 0x48: k = KEY_UP;   break;  case 0x50: k = KEY_DOWN;  break;
            case 0x49: k = KEY_PGUP; break;  case 0x51: k = KEY_PGDN;  break;
            case 0x47: k = KEY_HOME; break;  case 0x4F: k = KEY_END;   break;
            case 0x4B: k = KEY_LEFT; break;  case 0x4D: k = KEY_RIGHT; break;
        }
        if (k) wm_key(k);
        return;
    }

    switch (sc) {
    case SC_LCTRL:          modk(MK_LCTRL,  1); return;
    case SC_LCTRL  | 0x80:  modk(MK_LCTRL,  0); return;
    case SC_LSHIFT:         modk(MK_LSHIFT, 1); return;
    case SC_LSHIFT | 0x80:  modk(MK_LSHIFT, 0); return;
    case SC_RSHIFT:         modk(MK_RSHIFT, 1); return;
    case SC_RSHIFT | 0x80:  modk(MK_RSHIFT, 0); return;
    case SC_LALT:           modk(MK_LALT,   1); return;
    case SC_LALT   | 0x80:  modk(MK_LALT,   0); return;
    }
    if (sc & 0x80)
        return;                      /* other key releases */

    char base = scancode_map[sc & 0x7F];
    if (!base)
        return;
    /* Cmd+letter stays a LETTER. Ctrl+letter is folded to a control code here
     * because a decade of terminal habit lives on that mapping, but Cmd is a
     * command modifier, not a character one -- Cmd+W must arrive as 'w' with
     * EV_MOD_SUPER, or the window manager's shortcut table would have to be
     * written against 0x17 and would then be indistinguishable from Ctrl+W. */
    if ((mod_keys & (MK_LCTRL | MK_RCTRL)) && base >= 'a' && base <= 'z') {
        wm_key(base - 'a' + 1);      /* Ctrl+letter -> control code (Ctrl+S=0x13) */
        return;
    }
    char c = (mod_keys & (MK_LSHIFT | MK_RSHIFT)) ? scancode_map_shift[sc & 0x7F] : base;
    if (c)
        wm_key(c);                   /* route to the focused UI */
}
