#include "es1370.h"
#include "es1370_internal.h"
#include "pci.h"
#include "kheap.h"
#include "kprintf.h"
#include <string.h>

#define ES_VENDOR 0x1274u
#define ES_DEVICE 0x5000u

static int identify_device(struct device *device, uint16_t *port)
{
    if (!device || device->bus_type != DEV_BUS_PCI || device->seg ||
        device->vendor != ES_VENDOR || device->device != ES_DEVICE ||
        device->header_type || device->class_code != 4 || device->subclass != 1) {
        return -1;
    }
    uint32_t identity = pci_cfg_read(device->bus, device->slot, device->func, PCI_CFG_VENDOR);
    uint32_t bar = pci_cfg_read(device->bus, device->slot, device->func, PCI_CFG_BAR0);
    const struct dev_resource *resource = &device->res[0];
    if (identity != 0x50001274u || bar == UINT32_MAX || !(bar & 1u) ||
        !(resource->flags & DEV_RES_IO) || (resource->flags & DEV_RES_MEM) ||
        !resource->start || resource->size < ES_IO_BYTES || resource->size > 256u ||
        (resource->size & (resource->size - 1u)) ||
        resource->start > UINT16_MAX - (resource->size - 1u) ||
        (resource->start & (resource->size - 1u)) ||
        (bar & ~3u) != resource->start) {
        return -1;
    }
    *port = (uint16_t)resource->start;
    return 0;
}

static void release_card(struct es1370_card *card)
{
    /* Never hold the card gate while draining IRQ callbacks or detaching the
     * mixer: both may be waiting to enter a driver callback themselves. */
    if (card->stream[ES_PLAYBACK].registered) {
        snd_unregister_device(&card->sound);
    }
    if (card->stream[ES_CAPTURE].registered) {
        snd_unregister_capture_device(&card->capture);
    }
    int stopped = 1;
    if (card->io_enabled) {
        uint64_t flags = io_lock_enter(&card->gate);
        card->stream[ES_PLAYBACK].registered = 0;
        card->stream[ES_CAPTURE].registered = 0;
        stopped = es1370_stop_all_locked(card) == 0;
        io_lock_leave(&card->gate, flags);
    }
    if (card->irq_registered && dev_irq_release(card->device)) {
        {
            IO_GUARD(&card->gate);
            es1370_quarantine(card);
        }
        kprintf("[es1370] IRQ release unconfirmed; context and DMA retained\n");
        return;
    }
    card->irq_registered = 0;
    int disabled = !card->io_enabled || dev_disable_checked(card->device) == 0;
    if (disabled) {
        card->io_enabled = 0;
    }
    if (!stopped || !disabled || card->dma.blocked) {
        {
            IO_GUARD(&card->gate);
            es1370_quarantine(card);
        }
        kprintf("[es1370] stop unconfirmed; context and DMA retained\n");
        return;
    }
    dma_device_quiesced(&card->dma);
    if ((card->stream[ES_PLAYBACK].ring && dma_free_coherent(card->stream[ES_PLAYBACK].ring)) ||
        (card->stream[ES_CAPTURE].ring && dma_free_coherent(card->stream[ES_CAPTURE].ring)) ||
        (card->phantom && dma_free_coherent(card->phantom))) {
        es1370_record_tombstone(card);
        kprintf("[es1370] DMA release refused; context retained\n");
        return;
    }
    kfree(card);
}

int es1370_probe(struct device *device)
{
    uint16_t port;
    if (!device || es1370_function_quarantined(device) ||
        identify_device(device, &port) || (snd_present() && snd_capture_present())) {
        return -1;
    }
    struct es1370_card *card = kmalloc(sizeof *card);
    if (!card) {
        return -1;
    }
    memset(card, 0, sizeof *card);
    card->device = device;
    card->port = port;
    dma_device_init(&card->dma, "es1370", DMA_MASK_32);
    if (dev_enable_checked(device, 0)) {
        /* No DMA address was published and no IRQ callback exists yet. */
        kfree(card);
        return -1;
    }
    card->io_enabled = 1;
    if (es1370_stop_all_locked(card) || es1370_codec_configure(card)) {
        goto failed;
    }
    card->stream[ES_PLAYBACK].ring = dma_alloc_coherent(&card->dma, ES_RING_BYTES, 4096, 0);
    card->phantom = dma_alloc_coherent(&card->dma, ES_PHANTOM_BYTES, 4096, 0);
    if (!es1370_buffer_valid(card->stream[ES_PLAYBACK].ring, ES_RING_BYTES) ||
        !es1370_buffer_valid(card->phantom, ES_PHANTOM_BYTES)) {
        goto failed;
    }
    memset(card->stream[ES_PLAYBACK].ring->cpu, 0, ES_RING_BYTES);
    memset(card->phantom->cpu, 0, ES_PHANTOM_BYTES);
    /* Optional capture allocation does not remove a usable playback device.
     * Both rings are physical DMA32 allocations; there is no playback-ring
     * alias or synthetic microphone input. */
    card->stream[ES_CAPTURE].ring = dma_alloc_coherent(&card->dma, ES_RING_BYTES, 4096, 0);
    if (card->stream[ES_CAPTURE].ring &&
        !es1370_buffer_valid(card->stream[ES_CAPTURE].ring, ES_RING_BYTES)) {
        goto failed;
    }
    if (card->stream[ES_CAPTURE].ring) {
        memset(card->stream[ES_CAPTURE].ring->cpu, 0, ES_RING_BYTES);
    }
    if (es1370_engines_initialize(card)) {
        goto failed;
    }
    es1370_pcm_initialize(card);
    if (dev_irq_request(device, es1370_interrupt, card, "es1370") < 0) {
        goto failed;
    }
    card->irq_registered = 1;
    card->sound.irq_mode = device->irq_mode;
    card->capture.irq_mode = device->irq_mode;
    if (!snd_present() && snd_register_device(&card->sound) == 0) {
        card->stream[ES_PLAYBACK].registered = 1;
    }
    if (card->stream[ES_CAPTURE].ring && !snd_capture_present() &&
        snd_register_capture_device(&card->capture) == 0) {
        card->stream[ES_CAPTURE].registered = 1;
    }
    if (!card->stream[ES_PLAYBACK].registered && !card->stream[ES_CAPTURE].registered) {
        goto failed;
    }
    dev_set_drvdata(device, card);
    kprintf("[es1370] %s io=%x DMA32 rate=%u IRQ=%u playback=%u capture=%u\n",
            device->name, port, ES_RATE, device->irq_mode,
            card->stream[ES_PLAYBACK].registered, card->stream[ES_CAPTURE].registered);
    if (card->stream[ES_PLAYBACK].registered) {
        snd_init();
    }
    return 0;

failed:
    release_card(card);
    return -1;
}

void es1370_remove(struct device *device)
{
    if (!device) {
        return;
    }
    struct es1370_card *card = dev_get_drvdata(device);
    if (card) {
        dev_set_drvdata(device, NULL);
        release_card(card);
    }
}

static const struct dev_match es1370_matches[] = {
    DEV_MATCH_VD(ES_VENDOR, ES_DEVICE),
    DEV_MATCH_END
};
static struct driver es1370_driver = {
    .name = "es1370", .bus_type = DEV_BUS_PCI, .match = es1370_matches,
    .probe = es1370_probe, .remove = es1370_remove
};
DRIVER_DECLARE(es1370_driver);
