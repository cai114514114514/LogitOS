#ifndef LOGIT_KEYBOARD_H
#define LOGIT_KEYBOARD_H

/* Called from the IRQ1 handler: read a scancode and echo the typed character. */
void keyboard_handle(void);

#endif /* LOGIT_KEYBOARD_H */
