#ifndef LOGIT_MOUSE_H
#define LOGIT_MOUSE_H

/* PS/2 mouse (IRQ12). Enables the auxiliary device and streaming. */
void mouse_init(void);

/* Called from the IRQ12 handler: reads one packet byte and, on a complete
 * packet, updates the cursor and notifies the window manager. Packets are 3
 * bytes, or 4 once mouse_init has switched the device into IntelliMouse mode
 * (the extra byte is the scroll wheel). */
void mouse_handle(void);

#endif /* LOGIT_MOUSE_H */
