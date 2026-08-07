#ifndef LOGIT_IO_H
#define LOGIT_IO_H
/* Host stub for c/kernel/cpu/io.h: the port accessors become ordinary calls the
 * test defines, so pci.c's legacy 0xCF8/0xCFC path runs against a synthetic
 * configuration space instead of real hardware. */
#include <stdint.h>

void     outl(uint16_t port, uint32_t val);
uint32_t inl(uint16_t port);
void     outb(uint16_t port, uint8_t val);
uint8_t  inb(uint16_t port);
void     outw(uint16_t port, uint16_t val);
uint16_t inw(uint16_t port);

#endif
