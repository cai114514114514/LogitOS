/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ac97_internal.h"
#include "../../../kernel/pci/pci.h"
#include "kprintf.h"

/* One stable slot holds both direction's callback identities. An unresolved
 * stop or IRQ teardown pins it permanently rather than reusing DMA handles. */
static struct ac97_card card;
static io_lock_t registration_gate = IO_LOCK_INIT;

static int io_resource_valid(const struct dev_resource *resource,
                              uint64_t required)
{
    return resource->flags == DEV_RES_IO && resource->start &&
           resource->size >= required && resource->size <= UINT16_MAX + 1ull &&
           !(resource->size & (resource->size - 1)) &&
           !(resource->start & (resource->size - 1)) &&
           resource->start <= UINT16_MAX + 1ull - resource->size;
}

static int resources_valid(struct device *device, const struct ac97_model *model)
{
    const struct dev_resource *mixer = &device->res[0];
    const struct dev_resource *master = &device->res[1];
    if (!io_resource_valid(mixer, 256) || !io_resource_valid(master, 64))
        return 0;
    uint32_t identity = pci_cfg_read(device->bus, device->slot, device->func, 0);
    uint32_t mixer_bar = pci_cfg_read(device->bus, device->slot, device->func, 0x10);
    uint32_t master_bar = pci_cfg_read(device->bus, device->slot, device->func, 0x14);
    if (identity != (((uint32_t)model->device_id << 16) | UINT32_C(0x8086)) ||
        (mixer_bar & 3) != 1 || (master_bar & 3) != 1 ||
        (mixer_bar & ~3u) != mixer->start ||
        (master_bar & ~3u) != master->start)
        return 0;
    return mixer->start + mixer->size <= master->start ||
           master->start + master->size <= mixer->start;
}

void ac97_contain_pci(struct ac97_card *owner)
{
    struct device *device = owner->device;
    uint16_t command = pci_cfg_read16(device->bus, device->slot,
                                      device->func, PCI_CFG_COMMAND);
    if (command != UINT16_MAX) {
        uint16_t wanted = (command | PCI_CMD_INTX_DIS) & ~PCI_CMD_MASTER;
        pci_cfg_write16(device->bus, device->slot, device->func,
                         PCI_CFG_COMMAND, wanted);
        uint16_t observed = pci_cfg_read16(device->bus, device->slot,
                                           device->func, PCI_CFG_COMMAND);
        if ((observed & (PCI_CMD_INTX_DIS | PCI_CMD_MASTER)) != PCI_CMD_INTX_DIS)
            kprintf("AC97: PCI rejected interrupt/DMA containment\n");
    }
    /* No audio port access after this attempt, even when PCI rejected it:
     * decode state is uncertain. A late IRQ retries this PCI-only operation. */
    dev_disable_checked(device);
    owner->io_enabled = 0;
}

static int stop_both_streams(struct ac97_card *owner)
{
    IO_GUARD(&owner->gate);
    int input_stopped = ac97_stream_stop_locked(owner, &owner->capture);
    int output_stopped = ac97_stream_stop_locked(owner, &owner->playback);
    return input_stopped || output_stopped;
}

static void unregister_consumers(struct ac97_card *owner)
{
    /* snd unregister takes worker locks and can call ->stop. Do not hold our
     * gate here. Both consumers must be detached before either ring is freed. */
    if (owner->capture_registered) {
        snd_unregister_capture_device(&owner->input);
        owner->capture_registered = 0;
    }
    if (owner->sound_registered) {
        snd_unregister_device(&owner->sound);
        owner->sound_registered = 0;
    }
}

void ac97_pci_remove(struct device *device)
{
    if (!device || card.device != device ||
        __atomic_exchange_n(&card.removing, 1, __ATOMIC_ACQ_REL))
        return;
    unregister_consumers(&card);
    int stopped = stop_both_streams(&card);
    if (card.irq_registered) {
        if (dev_irq_release(device)) {
            IO_GUARD(&card.gate);
            dma_device_quarantine(&card.dma);
            ac97_contain_pci(&card);
            __atomic_store_n(&card.removing, 0, __ATOMIC_RELEASE);
            return;
        }
        card.irq_registered = 0;
    }
    int disabled = dev_disable_checked(device);
    card.io_enabled = 0;
    if (stopped || disabled)
        goto retained;
    dma_device_quiesced(&card.dma);
    if (ac97_streams_free(&card))
        goto retained;
    dev_set_drvdata(device, NULL);
    {
        IO_GUARD(&registration_gate);
        card = (struct ac97_card){0};
    }
    return;
retained:
    dma_device_quarantine(&card.dma);
    __atomic_store_n(&card.removing, 0, __ATOMIC_RELEASE);
}

static int initialize_hardware(struct device *device, const struct ac97_model *model)
{
    dma_device_init(&card.dma, "ac97", DMA_MASK_32);
    if (ac97_controller_init(&card.playback.controller, &ac97_native_bus,
                             device->res[0].start, device->res[1].start, model))
        return -1;
    if (ac97_capture_prepare(&card.capture.controller, &card.playback.controller))
        kprintf("AC97: ADC initialization failed; capture unavailable\n");
    if (ac97_streams_allocate(&card) || dev_enable_checked(device, 1))
        return -1;
    if (dev_irq_request(device, ac97_streams_interrupt, &card, "ac97") < 0)
        return -1;
    card.irq_registered = 1;
    ac97_streams_configure(&card);
    /* The existing playback owner does not preclude this card's ADC. Keep
     * each registration independent and retain the card if either succeeded. */
    if (snd_register_device(&card.sound) == 0)
        card.sound_registered = 1;
    if (card.capture.controller.initialized &&
        snd_register_capture_device(&card.input) == 0)
        card.capture_registered = 1;
    if (!card.sound_registered && !card.capture_registered)
        return -1;
    /* snd_init initializes playback state. Calling it after losing playback
     * registration could reset another card's active mixer and semaphore. */
    if (card.sound_registered)
        snd_init();
    return 0;
}

int ac97_pci_probe(struct device *device)
{
    if (!device)
        return -1;
    const struct ac97_model *model = ac97_model_find(device->vendor, device->device);
    if (!model || device->bus_type != DEV_BUS_PCI || device->seg ||
        device->class_code != 4 || device->subclass != 1 || device->header_type ||
        !resources_valid(device, model))
        return -1;
    {
        IO_GUARD(&registration_gate);
        if (card.device)
            return -1;
        card.device = device;
    }
    /* Clearing inherited BME precedes all port access. Granting it again must
     * wait until all old engines, including the unused mono MIC, are halted. */
    if (dev_enable_checked(device, 0)) {
        IO_GUARD(&registration_gate);
        card.device = NULL;
        return -1;
    }
    card.io_enabled = 1;
    dev_set_drvdata(device, &card);
    if (initialize_hardware(device, model)) {
        ac97_pci_remove(device);
        return -1;
    }
    kprintf("AC97: %s codec=%x playback=%u capture=%u 48000Hz s16 stereo periods=8 irq=%u\n",
            model->name, card.playback.controller.codec_id, card.sound_registered,
            card.capture_registered, device->irq_mode);
    return 0;
}

#define AC97_PCI_MATCH(id, label, channels) DEV_MATCH_VD(0x8086, id),
static const struct dev_match ac97_matches[] = {
    AC97_INTEL_MODELS(AC97_PCI_MATCH)
    DEV_MATCH_END
};
#undef AC97_PCI_MATCH
static struct driver ac97_driver = {
    .name = "ac97", .bus_type = DEV_BUS_PCI,
    .match = ac97_matches, .probe = ac97_pci_probe, .remove = ac97_pci_remove
};
DRIVER_DECLARE(ac97_driver);
