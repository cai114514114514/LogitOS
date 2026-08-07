#ifndef LOGIT_SERIAL_H
#define LOGIT_SERIAL_H
/* host-test stub: the test records what would have gone to COM1. */
void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
int  serial_getc(void);
#endif
