#ifndef TEST_ES1370_PORT_IO_H
#define TEST_ES1370_PORT_IO_H
#include <stdint.h>
uint32_t inl(uint16_t port);
void outl(uint16_t port, uint32_t value);
void outw(uint16_t port, uint16_t value);
#endif
