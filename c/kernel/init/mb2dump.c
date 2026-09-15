#include <stdint.h>
#include "kprintf.h"

#ifdef BOOT_MB2_DUMP

/* This file is an instrument, not a second Multiboot2 consumer.  It runs
 * before pmm_init()/acpi_set_mb2_info(), prints the exact fields the BIOS
 * fixture prints, and exits through QEMU's debug port.  Keeping it behind a
 * compile-time gate prevents a diagnostic walk from becoming boot policy. */

static void putc_hex(char c)
{
    kprintf("%c", c);
}

static void hex8(uint8_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    putc_hex(digits[value >> 4]);
    putc_hex(digits[value & 15]);
}

static void hex32(uint32_t value)
{
    for (int shift = 28; shift >= 0; shift -= 4)
        putc_hex("0123456789ABCDEF"[(value >> shift) & 15]);
}

static void hex64(uint64_t value)
{
    hex32((uint32_t)(value >> 32));
    hex32((uint32_t)value);
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t u64(const uint8_t *p)
{
    return (uint64_t)u32(p) | ((uint64_t)u32(p + 4) << 32);
}

static void finish(int pass, const char *reason)
{
    if (pass)
        kprintf("MB2 RESULT PASS\n");
    else
        kprintf("MB2 RESULT FAIL %s\n", reason);
    /* isa-debug-exit returns (value << 1) | 1.  The gate treats the serial
     * result as the oracle because QEMU therefore has a non-zero status even
     * on the successful 0x10 exit. */
    __asm__ volatile ("outb %0, $0xf4" : : "a"((uint8_t)(pass ? 0x10 : 0x11)));
}

void mb2_dump(uint64_t mb_info)
{
    const uint8_t *block = (const uint8_t *)(uintptr_t)mb_info;
    uint32_t total = u32(block);
    if (total < 16 || total > 0x100000) {
        finish(0, "total");
        return;
    }

    kprintf("MB2 BEGIN\nMB2 TOTAL ");
    hex32(total);
    putc_hex('\n');

    uint32_t off = 8;
    while (off + 8 <= total) {
        const uint8_t *tag = block + off;
        uint32_t type = u32(tag);
        uint32_t size = u32(tag + 4);
        if (size < 8 || size > total - off) {
            finish(0, "bounds");
            return;
        }

        kprintf("MB2 TAG ");
        hex32(type);
        putc_hex(' ');
        hex32(size);
        putc_hex('\n');

        if (type == 6) {
            if (size < 16 || u32(tag + 8) < 24) {
                finish(0, "memory-map");
                return;
            }
            uint32_t stride = u32(tag + 8);
            for (uint32_t pos = 16; pos + stride <= size; pos += stride) {
                kprintf("MB2 MMAP ");
                hex64(u64(tag + pos));
                putc_hex(' ');
                hex64(u64(tag + pos + 8));
                putc_hex(' ');
                hex32(u32(tag + pos + 16));
                putc_hex('\n');
            }
        } else if (type == 14 || type == 15) {
            if (size < 28 || (type == 15 && size < 44)) {
                finish(0, "acpi");
                return;
            }
            const uint8_t *rsdp = tag + 8;
            kprintf("MB2 ACPI ");
            for (unsigned i = 0; i < 6; ++i)
                hex8(rsdp[9 + i]);
            putc_hex(' ');
            hex8(rsdp[15]);
            putc_hex(' ');
            hex32(u32(rsdp + 16));
            putc_hex(' ');
            hex64(type == 15 ? u64(rsdp + 24) : 0);
            putc_hex('\n');
        } else if (type == 8) {
            if (size < 32) {
                finish(0, "framebuffer");
                return;
            }
            kprintf("MB2 FB ");
            hex64(u64(tag + 8));
            putc_hex(' ');
            hex32(u32(tag + 16));
            putc_hex(' ');
            hex32(u32(tag + 20));
            putc_hex(' ');
            hex32(u32(tag + 24));
            putc_hex(' ');
            hex8(tag[28]);
            putc_hex(' ');
            hex8(tag[29]);
            putc_hex('\n');
        }

        uint32_t next = (off + size + 7u) & ~7u;
        if (type == 0) {
            if (size != 8 || next != total) {
                finish(0, "end-not-last");
                return;
            }
            kprintf("MB2 END\n");
            finish(1, 0);
            return;
        }
        if (next <= off) {
            finish(0, "zero-or-small-size");
            return;
        }
        off = next;
    }
    finish(0, "missing-end");
}

#endif
