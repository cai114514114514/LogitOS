#ifndef LOGIT_KEYBOARD_H
#define LOGIT_KEYBOARD_H

/* Called from the IRQ1 handler: read a scancode and echo the typed character. */
void keyboard_handle(void);

/* Modifier keys currently held, as an EV_MOD_* mask (include/abi/logit_abi.h).
 * The mouse driver reads this so a click can carry shift/ctrl/alt: the keyboard
 * is where that state lives, and there is exactly one of it. */
int kbd_mods(void);

#endif /* LOGIT_KEYBOARD_H */
