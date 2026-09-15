#ifndef TEST_DMA_IO_H
#define TEST_DMA_IO_H
#include <stdint.h>
uint64_t test_read(const volatile void *, unsigned);
void test_write(volatile void *, unsigned, uint64_t);
extern unsigned char test_regs[];
static inline uint8_t inb(uint16_t p) { return test_read(test_regs + p - 0x1000, 1); }
static inline uint16_t inw(uint16_t p) { return test_read(test_regs + p - 0x1000, 2); }
static inline uint32_t inl(uint16_t p) { return test_read(test_regs + p - 0x1000, 4); }
static inline void outb(uint16_t p,uint8_t v) { test_write(test_regs + p - 0x1000,1,v); }
static inline void outw(uint16_t p,uint16_t v) { test_write(test_regs + p - 0x1000,2,v); }
static inline void outl(uint16_t p,uint32_t v) { test_write(test_regs + p - 0x1000,4,v); }
#endif
