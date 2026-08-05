#ifndef LOGIT_SERIAL_H
#define LOGIT_SERIAL_H

/* COM1 serial driver — primary debug/log channel and test transport. */

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
int  serial_getc(void);   /* next RX byte, or -1 if none */

#endif /* LOGIT_SERIAL_H */
