#include "es1370_internal.h"
#include "pci.h"
#include "io.h"
#include "ktime.h"

#define ES_QUARANTINE_SLOTS 16u
#define ES_POLL_LIMIT 100000u
#define ES_WAIT_NS UINT64_C(10000000)

struct quarantined_function {
    unsigned used;
    uint8_t bus, slot, function;
};
static struct quarantined_function quarantined[ES_QUARANTINE_SLOTS];
static io_lock_t quarantine_gate = IO_LOCK_INIT;
static unsigned quarantine_table_full;

int es1370_function_quarantined(const struct device *device)
{
    IO_GUARD(&quarantine_gate);
    if (quarantine_table_full) {
        return 1;
    }
    for (unsigned i = 0; i < ES_QUARANTINE_SLOTS; ++i) {
        if (quarantined[i].used && quarantined[i].bus == device->bus &&
            quarantined[i].slot == device->slot &&
            quarantined[i].function == device->func) {
            return 1;
        }
    }
    return 0;
}

void es1370_record_tombstone(struct es1370_card *card)
{
    if (card->tombstone_recorded) {
        return;
    }
    IO_GUARD(&quarantine_gate);
    card->tombstone_recorded = 1;
    for (unsigned i = 0; i < ES_QUARANTINE_SLOTS; ++i) {
        if (!quarantined[i].used) {
            quarantined[i] = (struct quarantined_function){1,
                card->device->bus, card->device->slot, card->device->func};
            return;
        }
    }
    /* Without room for another persistent identity, no new ES1370 may
     * bind. Forgetting an old allocation would permit DMA-address reuse. */
    quarantine_table_full = 1;
}

uint32_t es1370_read(struct es1370_card *card, uint16_t offset)
{
    return inl((uint16_t)(card->port + offset));
}

void es1370_write(struct es1370_card *card, uint16_t offset, uint32_t value)
{
    outl((uint16_t)(card->port + offset), value);
}

int es1370_wait_clear(struct es1370_card *card, uint16_t offset, uint32_t mask)
{
    uint64_t started = time_mono_ns();
    for (unsigned attempt = 0; attempt < ES_POLL_LIMIT; ++attempt) {
        uint32_t value = es1370_read(card, offset);
        if (value == UINT32_MAX) {
            return -1;
        }
        if (!(value & mask)) {
            return 0;
        }
        uint64_t now = time_mono_ns();
        if (now < started || now - started >= ES_WAIT_NS) {
            return -1;
        }
        io_relax();
    }
    return -1;
}

void es1370_quarantine(struct es1370_card *card)
{
    if (card->io_enabled) {
        es1370_write(card, ES_SERIAL, 0);
        es1370_write(card, ES_CONTROL, ES_CONTROL_IDLE);
        uint16_t command = pci_cfg_read16(card->device->bus, card->device->slot,
            card->device->func, PCI_CFG_COMMAND);
        if (command != UINT16_MAX) {
            /* IRQ vector release cannot run from its own ISR. Suppress the
             * PCI source immediately; retain memory regardless of readback. */
            pci_cfg_write16(card->device->bus, card->device->slot,
                card->device->func, PCI_CFG_COMMAND,
                (uint16_t)((command & ~PCI_CMD_MASTER) | PCI_CMD_INTX_DIS));
            (void)pci_cfg_read16(card->device->bus, card->device->slot,
                                card->device->func, PCI_CFG_COMMAND);
        }
    }
    for (unsigned direction = 0; direction < ES_DIRECTIONS; ++direction) {
        card->stream[direction].running = 0;
    }
    card->control = ES_CONTROL_IDLE;
    card->serial = 0;
    card->faulted = 1;
    es1370_record_tombstone(card);
    dma_device_quarantine(&card->dma);
}

int es1370_buffer_valid(const struct dma_buffer *buffer, size_t bytes)
{
    if (!buffer || !buffer->cpu || buffer->size < bytes) {
        return 0;
    }
    uint64_t address = dma_addr_value(buffer->dma);
    return address && !(address & 3u) && address <= DMA_MASK_32 &&
           bytes - 1u <= DMA_MASK_32 - address;
}

