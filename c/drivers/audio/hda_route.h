#ifndef HDA_ROUTE_H
#define HDA_ROUTE_H

#include <stddef.h>
#include <stdint.h>

/* HDA 1.0a, Amplifier Gain/Mute: the input index is four bits. Lists larger
 * than this are refused rather than truncated into a different audio route. */
#define HDA_ROUTE_CONNECTIONS 16
#define HDA_ROUTE_NODES 3      /* pin -> mixer/selector -> DAC: P7's depth bound */
#define HDA_ROUTE_VERBS 32     /* at most 3 power + 2 select + 2 stream + 19 amp + 2 pin */

/* Values are HDA 1.0a Audio Widget Capabilities, section 7.3.4.6. */
enum hda_route_type {
    HDA_ROUTE_DAC = 0,
    HDA_ROUTE_MIXER = 2,
    HDA_ROUTE_SELECTOR = 3,
    HDA_ROUTE_PIN = 4
};

struct hda_widget {
    unsigned nid;
    unsigned type;
    uint32_t caps;
    uint32_t pin_caps;
    uint32_t cfg;
    uint32_t amp_in;
    uint32_t amp_out;
    unsigned connection_count;
    unsigned connections[HDA_ROUTE_CONNECTIONS];
};

struct hda_route {
    unsigned count;
    struct hda_widget nodes[HDA_ROUTE_NODES]; /* sink first, converter last */
    unsigned indices[HDA_ROUTE_NODES];       /* expanded connection-list indices */
};

struct hda_route_verb {
    unsigned nid;
    unsigned verb;       /* codec_cmd convention: 0x300 for 4-bit verb 3 */
    unsigned payload;
};

/* Pick the highest-priority reachable pin, excluding both pins and DACs from
 * earlier assignments. No allocation, MMIO, kernel headers or codec quirks. */
int hda_route_find(const struct hda_widget *widgets, size_t count,
                   const struct hda_route *assigned, size_t assigned_count,
                   struct hda_route *route);

/* Compile the proven path into the very commands the driver sends. The
 * caller must supply HDA_ROUTE_VERBS slots; errors produce no commands.
 * SET_POWER entries are barriers: the consumer must confirm D0 and reject
 * PS-Error with bounded GET_POWER_STATE polling (HDA 1.0a 7.3.3.10). */
int hda_route_verbs(const struct hda_route *route, unsigned format,
                    unsigned stream_tag, struct hda_route_verb *verbs,
                    size_t capacity);

#endif
