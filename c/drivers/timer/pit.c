#include <stdint.h>
#include "pit.h"
#include "io.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_FREQ     1193182u     /* base oscillator frequency in Hz */

static volatile uint64_t ticks = 0;

void pit_init(uint32_t hz)
{
    uint32_t divisor = PIT_FREQ / hz;
    outb(PIT_COMMAND, 0x36);                    /* channel 0, lo/hi, mode 3 */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}

void timer_tick(void)
{
    ticks++;
}

uint64_t timer_ticks(void)
{
    return ticks;
}
