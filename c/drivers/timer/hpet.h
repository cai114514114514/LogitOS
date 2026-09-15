#ifndef LOGIT_HPET_H
#define LOGIT_HPET_H

#include <stddef.h>
#include <stdint.h>

/* Parsed ACPI HPET description.  The parser is public because firmware bytes
 * are the dangerous half of this driver and are exercised by the host gate. */
struct hpet_desc {
    uint32_t block_id;
    uint64_t address;
    uint16_t min_tick;
    uint8_t  sequence;
    uint8_t  page_protection;
};

struct hpet_caps {
    uint64_t hz;
    uint64_t mask;
    uint32_t period_fs;
    uint8_t  timers;
    uint8_t  counter64;
};

int hpet_parse_table(const void *table, size_t bytes, struct hpet_desc *out);
int hpet_parse_caps(uint64_t raw, uint32_t firmware_block_id,
                    struct hpet_caps *out);

/* Initialises only the free-running main counter.  LogitOS does not claim the
 * comparator IRQs: the PIT remains the scheduler tick and emergency fallback. */
int      hpet_init(void);
int      hpet_ready(void);
uint64_t hpet_read(void);
uint64_t hpet_hz(void);
uint64_t hpet_mask(void);
uint32_t hpet_period_fs(void);

#endif
