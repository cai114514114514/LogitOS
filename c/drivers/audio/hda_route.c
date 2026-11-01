#include "hda_route.h"

/* HDA 1.0a sections 7.3.4.6, 7.3.4.9 and 7.3.4.10 define these capability
 * fields. Keep the wire values here so the host and kernel cannot disagree.
 * Specification: https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf
 * Linux reference throughout: v6.12 sound/pci/hda/hda_generic.c,
 * snd_hda_activate_path(), activate_amp_in/out(), path_power_update().
 * https://github.com/torvalds/linux/blob/v6.12/sound/pci/hda/hda_generic.c
 * This is bounded generic routing, not Realtek vendor-coefficient setup.
 * Model and QEMU evidence: not verified on X79-VG4 hardware. */
#define WIDGET_AMP_IN       (1u << 1)
#define WIDGET_AMP_OUT      (1u << 2)
#define WIDGET_POWER        (1u << 10)
#define PIN_HEADPHONE       (1u << 3)
#define PIN_OUTPUT          (1u << 4)
#define PIN_EAPD            (1u << 16)
#define AMP_MUTE_CAP        (1u << 31)

/* HDA 1.0a section 7.3.3, named verb tables: 12-bit verbs take 8-bit data;
 * Amplifier Gain/Mute and Converter Format are 4-bit verbs with 16-bit data.
 * Values use hda.c's 0x300/0x200 notation for those two wide-payload verbs. */
#define SET_POWER           0x705
#define SET_SELECT          0x701
#define SET_FORMAT          0x200
#define SET_STREAM          0x706
#define SET_AMP             0x300
#define SET_PIN             0x707
#define SET_EAPD            0x70c
#define AMP_OUTPUT          0x8000u
#define AMP_INPUT           0x4000u
#define AMP_STEREO          0x3000u
#define AMP_MUTE            0x0080u

/* NIDs name nodes, not positions: firmware enumeration order is not a graph
 * edge. Linear lookup is bounded by the codec's eight-bit NID space. */
static const struct hda_widget *find_widget(const struct hda_widget *widgets,
                                           size_t count, unsigned nid)
{
    for (size_t i = 0; i < count; i++) {
        if (widgets[i].nid == nid) {
            return &widgets[i];
        }
    }
    return NULL;
}

/* Future multi-output users pass earlier routes back in; sharing a converter
 * would silently make two independent outputs consume the same stream. */
static int already_assigned(const struct hda_route *assigned, size_t count,
                             unsigned nid, int converter)
{
    for (size_t i = 0; i < count; i++) {
        unsigned index = converter ? assigned[i].count - 1 : 0;
        if (assigned[i].nodes[index].nid == nid) {
            return 1;
        }
    }
    return 0;
}

/* HDA 1.0a section 7.3.3.31 defines device[23:20], association[7:4] and
 * sequence[3:0]. Policy follows Linux v6.12 hda_auto_parser.c,
 * snd_hda_parse_pin_defcfg(): line-out > speaker > headphone > other.
 * Association 0 (unassociated) and 15 (independent) both sort after 1..14;
 * sequence orders within that class. NID breaks exact ties reproducibly.
 * https://github.com/torvalds/linux/blob/v6.12/sound/pci/hda/hda_auto_parser.c */
static uint32_t pin_priority(const struct hda_widget *pin)
{
    unsigned device = (pin->cfg >> 20) & 0xf;
    unsigned association = (pin->cfg >> 4) & 0xf;
    unsigned sequence = pin->cfg & 0xf;

    if (device > 2) {
        device = 3;
    }
    if (association == 0 || association == 15) {
        association = 16;
    }
    return (device << 24) | (association << 16) | (sequence << 8) | pin->nid;
}

/* A candidate's second edge must end at an unclaimed DAC. No recursion:
 * cycles and three-edge paths cannot accidentally escape P7's depth bound. */
static int usable_dac(const struct hda_widget *widget,
                       const struct hda_route *assigned, size_t assigned_count)
{
    return widget && widget->type == HDA_ROUTE_DAC &&
           !already_assigned(assigned, assigned_count, widget->nid, 1);
}

/* Search every connected output pin before applying priority. An unreachable
 * line-out must not hide a working headphone path. The historical driver
 * guessed "first DAC + first pin" after a direct-only search; correction:
 * silence with plausible registers is not a route, so there is no fallback. */
int hda_route_find(const struct hda_widget *widgets, size_t count,
                   const struct hda_route *assigned, size_t assigned_count,
                   struct hda_route *route)
{
    uint32_t best_priority = UINT32_MAX;

    if (!route) {
        return -1;
    }
    route->count = 0;
    if (!widgets || (assigned_count && !assigned)) {
        return -1;
    }
    for (size_t i = 0; i < assigned_count; i++) {
        if (assigned[i].count < 2 || assigned[i].count > HDA_ROUTE_NODES) {
            return -1;
        }
    }

    for (size_t i = 0; i < count; i++) {
        const struct hda_widget *pin = &widgets[i];
        struct hda_route candidate = {0};
        uint32_t priority;

        /* HDA 1.0a Config Default: connectivity[31:30] == 01 means no
         * physical connection. Pin Capabilities bit 4 advertises output. */
        if (pin->type != HDA_ROUTE_PIN || !(pin->pin_caps & PIN_OUTPUT) ||
            ((pin->cfg >> 30) & 3) == 1 ||
            pin->connection_count > HDA_ROUTE_CONNECTIONS ||
            already_assigned(assigned, assigned_count, pin->nid, 0)) {
            continue;
        }
        priority = pin_priority(pin);
        if (priority >= best_priority) {
            continue;
        }
        candidate.nodes[0] = *pin;
        for (unsigned edge = 0; edge < pin->connection_count; edge++) {
            const struct hda_widget *source =
                find_widget(widgets, count, pin->connections[edge]);

            candidate.indices[0] = edge;
            if (usable_dac(source, assigned, assigned_count)) {
                candidate.nodes[1] = *source;
                candidate.count = 2;
                break;
            }
#ifndef HDA_NEGCTL_DIRECT_ONLY
            if (!source ||
                (source->type != HDA_ROUTE_MIXER && source->type != HDA_ROUTE_SELECTOR) ||
                source->connection_count > HDA_ROUTE_CONNECTIONS) {
                continue;
            }
            for (unsigned input = 0; input < source->connection_count; input++) {
                const struct hda_widget *dac =
                    find_widget(widgets, count, source->connections[input]);

                if (usable_dac(dac, assigned, assigned_count)) {
                    candidate.nodes[1] = *source;
                    candidate.nodes[2] = *dac;
                    candidate.indices[1] = input;
                    candidate.count = 3;
                    break;
                }
            }
            if (candidate.count) {
                break;
            }
#endif
        }
        if (candidate.count) {
            *route = candidate;
            best_priority = priority;
        }
    }
#ifdef HDA_NEGCTL_DIRECT_ONLY
    /* Reproduce the old unproven fallback as well as the direct-only walk.
     * The ALC887 assertion must fail even though this reports success. */
    if (!route->count) {
        for (size_t i = 0; i < count; i++) {
            if (widgets[i].type != HDA_ROUTE_PIN ||
                !(widgets[i].pin_caps & PIN_OUTPUT) ||
                ((widgets[i].cfg >> 30) & 3) == 1) {
                continue;
            }
            for (size_t j = 0; j < count; j++) {
                if (widgets[j].type == HDA_ROUTE_DAC) {
                    route->nodes[0] = widgets[i];
                    route->nodes[1] = widgets[j];
                    route->count = 2;
                    return 0;
                }
            }
        }
    }
#endif
    return route->count ? 0 : -1;
}

/* HDA Amplifier Capabilities: offset[6:0] is the zero-dB step, steps[14:8]
 * bounds legal gain. Unlike the old fixed 0x2a, this also handles a mute-only
 * pin or mixer without writing reserved gain bits (ALC887 sections 8.1.10/11,
 * 8.11/8.12). Saturation keeps inconsistent firmware caps inside the range. */
static unsigned unity_gain(uint32_t caps)
{
    unsigned offset = caps & 0x7f;
    unsigned steps = (caps >> 8) & 0x7f;

    return offset < steps ? offset : steps;
}

/* Appending is internal to a fixed, preflighted buffer of 32 commands; no
 * kernel-side emitter can silently lose the last power or unmute operation. */
static void append_verb(struct hda_route_verb *verbs, unsigned *count,
                         unsigned nid, unsigned verb, unsigned payload)
{
    verbs[*count].nid = nid;
    verbs[*count].verb = verb;
    verbs[*count].payload = payload;
    (*count)++;
}

/* Emit capability-supported operations in signal-path order: power first,
 * converter stream, selects, amps, then pin enable/EAPD. A mixer SUMS inputs
 * and has no Connection Select; its chosen DAC is enabled with an indexed
 * input amp instead (Linux v6.12 snd_hda_activate_path/activate_amp_in).
 * Other mute-capable mixer inputs are muted to exclude analogue loopback;
 * pin input amps belong to capture and are deliberately left alone. */
int hda_route_verbs(const struct hda_route *route, unsigned format,
                    unsigned stream_tag, struct hda_route_verb *verbs,
                    size_t capacity)
{
    unsigned count = 0;
    const struct hda_widget *pin;
    const struct hda_widget *dac;

    if (!route || !verbs || capacity < HDA_ROUTE_VERBS ||
        route->count < 2 || route->count > HDA_ROUTE_NODES ||
        stream_tag == 0 || stream_tag > 15 || format > 0xffff) {
        return -1;
    }
    pin = &route->nodes[0];
    dac = &route->nodes[route->count - 1];
    if (pin->type != HDA_ROUTE_PIN || dac->type != HDA_ROUTE_DAC) {
        return -1;
    }
    for (unsigned i = 0; i + 1 < route->count; i++) {
        const struct hda_widget *node = &route->nodes[i];

        if (node->connection_count > HDA_ROUTE_CONNECTIONS ||
            route->indices[i] >= node->connection_count ||
            node->connections[route->indices[i]] != route->nodes[i + 1].nid ||
            (i && node->type != HDA_ROUTE_MIXER && node->type != HDA_ROUTE_SELECTOR)) {
            return -1;
        }
    }
    for (unsigned i = 0; i < route->count; i++) {
        if (route->nodes[i].caps & WIDGET_POWER) {
            /* HDA 1.0a 7.3.3.10: SET_POWER data 0 requests D0; its reply
             * does not prove D0. The driver must treat each as a barrier,
             * polling GET_POWER_STATE for PS-Error and actual D0 before
             * continuing. A pure command compiler cannot read that status. */
            append_verb(verbs, &count, route->nodes[i].nid, SET_POWER, 0);
        }
    }
    append_verb(verbs, &count, dac->nid, SET_FORMAT, format);
    /* HDA Converter Stream/Channel: tag[7:4], first channel[3:0] = 0. */
    append_verb(verbs, &count, dac->nid, SET_STREAM, stream_tag << 4);
    for (unsigned i = 0; i < route->count; i++) {
        const struct hda_widget *node = &route->nodes[i];

        if (i + 1 < route->count && node->type != HDA_ROUTE_MIXER) {
            append_verb(verbs, &count, node->nid, SET_SELECT, route->indices[i]);
        }
        if (node->caps & WIDGET_AMP_OUT) {
            append_verb(verbs, &count, node->nid, SET_AMP,
                        AMP_OUTPUT | AMP_STEREO | unity_gain(node->amp_out));
        }
        if (i > 0 && i + 1 < route->count && (node->caps & WIDGET_AMP_IN)) {
            for (unsigned input = 0; input < node->connection_count; input++) {
                unsigned payload = AMP_INPUT | AMP_STEREO | (input << 8) |
                                   unity_gain(node->amp_in);

                if (input != route->indices[i]) {
                    if (node->type != HDA_ROUTE_MIXER || !(node->amp_in & AMP_MUTE_CAP)) {
                        continue;
                    }
                    payload |= AMP_MUTE;
                }
#ifdef HDA_NEGCTL_NO_MIXER_UNMUTE
                /* Keep every other write so only the essential DAC-input
                 * activation disappears from the tested command stream. */
                if (node->type == HDA_ROUTE_MIXER && input == route->indices[i]) {
                    continue;
                }
#endif
                append_verb(verbs, &count, node->nid, SET_AMP, payload);
            }
        }
    }
    /* Pin Widget Control: output bit 6; headphone bit 7 only if Pin Caps
     * bit 3 advertises an amp. EAPD bit 1 only if Pin Caps bit 16 supports it.
     * HDA 1.0a Pin Widget Control / EAPD-BTL Enable verb tables. */
    unsigned pin_control = 1u << 6;
    if (pin->pin_caps & PIN_HEADPHONE) {
        pin_control |= 1u << 7;
    }
    append_verb(verbs, &count, pin->nid, SET_PIN, pin_control);
    if (pin->pin_caps & PIN_EAPD) {
        append_verb(verbs, &count, pin->nid, SET_EAPD, 1u << 1);
    }
    return (int)count;
}
