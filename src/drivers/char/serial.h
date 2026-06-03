#ifndef AQUA_SERIAL_H
#define AQUA_SERIAL_H

/* COM1 serial driver — primary debug/log channel and test transport. */

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);

#endif /* AQUA_SERIAL_H */
