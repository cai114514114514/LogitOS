#ifndef AETHER_MOUSE_H
#define AETHER_MOUSE_H

/* PS/2 mouse (IRQ12). Enables the auxiliary device and streaming. */
void mouse_init(void);

/* Called from the IRQ12 handler: reads one packet byte and, on a complete
 * 3-byte packet, updates the cursor and notifies the window manager. */
void mouse_handle(void);

#endif /* AETHER_MOUSE_H */
