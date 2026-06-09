#ifndef AETHER_KEYBOARD_H
#define AETHER_KEYBOARD_H

/* Called from the IRQ1 handler: read a scancode and echo the typed character. */
void keyboard_handle(void);

#endif /* AETHER_KEYBOARD_H */
