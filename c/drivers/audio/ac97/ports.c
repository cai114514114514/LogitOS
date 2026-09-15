/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ac97.h"
#include "../../../kernel/cpu/io.h"
#include "../../../kernel/core/ktime.h"
#include "../../core/dma.h"

static void delay_microseconds(unsigned microseconds)
{
    uint64_t start = time_mono_ns();
    uint64_t duration = (uint64_t)microseconds * 1000;
    /* The clock may be unavailable during early probe. Both the outer reset
     * poll and this delay are bounded, so a dead clock cannot hang boot. */
    for (unsigned spin = 0; spin < microseconds * 1000u; spin++) {
        if (time_mono_ns() - start >= duration)
            break;
        __asm__ volatile ("pause");
    }
}

const struct ac97_bus ac97_native_bus = {
    .read8 = inb, .read16 = inw, .read32 = inl,
    .write8 = outb, .write16 = outw, .write32 = outl,
    .delay_us = delay_microseconds, .publish = dma_wmb
};
