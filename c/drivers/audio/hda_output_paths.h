#ifndef HDA_OUTPUT_PATHS_H
#define HDA_OUTPUT_PATHS_H

/* Every analog output of one HDA codec, planned the way Linux plans them.
 *
 * WHY THIS EXISTS. hda_route.c finds ONE output pin and ONE DAC. On the
 * X79-VG4 (Realtek ALC887) the speakers may be on the rear green jack, on the
 * front headphone jack or on the monitor, and nobody could say which. Driving a
 * single pin guesses; driving every connected pin and letting the headphone jack
 * mute the others (the policy every desktop Linux uses) removes the guess.
 *
 * This file is pure data and arithmetic: no MMIO, no kernel headers, no codec
 * quirks. The driver reads the widget graph, hands it to
 * hda_output_plan_build(), executes the verbs this module compiles, and asks
 * hda_output_automute_decide() what each pin should do after every jack
 * poll. The host gates compile this very file.
 *
 * Sources cited throughout:
 *   - Intel High Definition Audio Specification 1.0a ("HDA 1.0a"),
 *     https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf
 *   - Linux v6.12, sound/pci/hda/hda_auto_parser.c, snd_hda_parse_pin_defcfg()
 *   - Linux v6.12, sound/pci/hda/hda_generic.c: try_assign_dacs(),
 *     look_for_dac(), snd_hda_gen_update_outputs(), do_automute(),
 *     check_auto_mute_availability()
 *   - Linux v6.12, sound/pci/hda/hda_jack.c, is_jack_detectable() */

#include <stddef.h>
#include <stdint.h>
#include "hda_route.h"

/* HDA 1.0a 7.3.3.31, Configuration Default: bits 31:30 Port Connectivity. */
#define HDA_CONFIG_PORT_CONNECTIVITY_SHIFT      30
#define HDA_CONFIG_PORT_CONNECTIVITY_MASK       0x3u
#define HDA_PORT_CONNECTIVITY_JACK              0u
#define HDA_PORT_CONNECTIVITY_NO_CONNECTION     1u
#define HDA_PORT_CONNECTIVITY_FIXED_FUNCTION    2u
#define HDA_PORT_CONNECTIVITY_JACK_AND_FIXED    3u

/* HDA 1.0a 7.3.3.31: bits 23:20 Default Device. */
#define HDA_CONFIG_DEFAULT_DEVICE_SHIFT         20
#define HDA_CONFIG_DEFAULT_DEVICE_MASK          0xfu
#define HDA_DEFAULT_DEVICE_LINE_OUT             0x0u
#define HDA_DEFAULT_DEVICE_SPEAKER              0x1u
#define HDA_DEFAULT_DEVICE_HEADPHONE_OUT        0x2u

/* HDA 1.0a 7.3.3.31: bits 11:8 Misc, bits 7:4 Default Association,
 * bits 3:0 Sequence. Misc bit 0 is "Jack Detect Override": firmware sets it on
 * a jack whose presence line is not wired, so the sense bit must be ignored
 * (Linux AC_DEFCFG_MISC_NO_PRESENCE). */
#define HDA_CONFIG_MISC_SHIFT                   8
#define HDA_CONFIG_MISC_MASK                    0xfu
#define HDA_CONFIG_MISC_NO_PRESENCE_DETECT      0x1u
#define HDA_CONFIG_ASSOCIATION_SHIFT            4
#define HDA_CONFIG_ASSOCIATION_MASK             0xfu
#define HDA_CONFIG_SEQUENCE_MASK                0xfu

/* HDA 1.0a 7.3.4.6, Audio Widget Capabilities bit 9: Digital. A digital
 * converter (S/PDIF out) is not an analog DAC and must not carry an analog
 * pin's signal (Linux look_for_dac(): cap_digital must equal is_digital). */
#define HDA_WIDGET_CAPABILITY_DIGITAL           (1u << 9)

/* HDA 1.0a 7.3.4.9, Pin Capabilities. */
#define HDA_PIN_CAPABILITY_TRIGGER_REQUIRED     (1u << 1)
#define HDA_PIN_CAPABILITY_PRESENCE_DETECT      (1u << 2)
#define HDA_PIN_CAPABILITY_HEADPHONE_DRIVE      (1u << 3)
#define HDA_PIN_CAPABILITY_OUTPUT               (1u << 4)

/* HDA 1.0a 7.3.3.12, Pin Widget Control. */
#define HDA_PIN_CONTROL_HEADPHONE_ENABLE        (1u << 7)
#define HDA_PIN_CONTROL_OUTPUT_ENABLE           (1u << 6)
#define HDA_PIN_CONTROL_ALL_OUTPUTS_OFF         0u

/* HDA 1.0a 7.3.3.15, Pin Sense: bit 31 is Presence Detect. */
#define HDA_PIN_SENSE_PRESENCE                  (1u << 31)

/* Upper bound on routed outputs. Linux allows five line-outs, five speakers
 * and five headphones (HDA_MAX_OUTS); real codecs have at most six analog
 * output pins and the status table below has to stay small. */
#define HDA_OUTPUT_PATHS_MAX                    8

/* What a pin is for, in routing priority order (Linux assigns DACs to
 * line-outs first, then speakers, then headphones). HDA_OUTPUT_ROLE_ANY is the
 * fallback route found by hda_route_find() for a pin whose Configuration
 * Default names none of the three; it keeps the pre-planner behaviour. */
enum hda_output_role {
    HDA_OUTPUT_ROLE_LINE_OUT = 0,
    HDA_OUTPUT_ROLE_SPEAKER = 1,
    HDA_OUTPUT_ROLE_HEADPHONE = 2,
    HDA_OUTPUT_ROLE_ANY = 3
};

/* What the presence-detect line of a jack said at the last poll. NO_DETECT is
 * kept apart from EMPTY on purpose: "nothing is plugged in" and "this jack
 * cannot tell" lead to opposite conclusions when silence is being diagnosed. */
enum hda_jack_state {
    HDA_JACK_NO_DETECT = 0,
    HDA_JACK_EMPTY = 1,
    HDA_JACK_PLUGGED = 2
};

struct hda_output_path {
    enum hda_output_role role;
    struct hda_route route;            /* pin first, converter last */
    int converter_shared;              /* the DAC also feeds an earlier path */
    int jack_detectable;               /* presence detect is usable on this pin */
};

struct hda_output_plan {
    unsigned path_count;
    struct hda_output_path paths[HDA_OUTPUT_PATHS_MAX];
};

/* Walk the widget table (one audio function group, in NID order) and plan a
 * path for every connected line-out, speaker and headphone pin. Returns the
 * number of paths, zero when no pin was usable (the caller then falls back to
 * hda_route_find()). Pins whose Configuration Default says "no connection",
 * pins that cannot drive output, digital outputs and non-output pins are left
 * entirely alone. */
int hda_output_plan_build(const struct hda_widget *widgets, size_t widget_count,
                          struct hda_output_plan *plan);

/* Wrap one route found by hda_route_find() as a one-path plan, so the
 * single-output fallback runs through the same executor. */
void hda_output_plan_from_single_route(const struct hda_route *route,
                                       struct hda_output_plan *plan);

/* Whether the pin's presence-detect line can be trusted: the pin advertises it
 * AND firmware did not mark it unwired. */
int hda_pin_jack_detectable(const struct hda_widget *pin);

/* Pin Widget Control that switches this path's pin on: OUTPUT_ENABLE, plus
 * HEADPHONE_ENABLE for a headphone pin that can drive one. */
unsigned hda_output_path_pin_control(const struct hda_output_path *path);

/* The complete command stream for one path: power, converter format and
 * stream, connection selects, every amplifier at unity gain and unmuted, then
 * the pin control. At most HDA_ROUTE_VERBS verbs. -1 on a malformed route. */
int hda_output_path_verbs(const struct hda_output_path *path, unsigned format,
                          unsigned stream_tag, struct hda_route_verb *verbs,
                          size_t capacity);

/* Linux auto-mute: given each path's jack state, decide whether its pin should
 * be driving. A plugged headphone mutes the line-outs and the speakers; a
 * plugged line-out mutes the speakers; headphone pins are never muted. A jack
 * that cannot detect never mutes anything. `jack_states` and `output_enabled`
 * each hold plan->path_count entries. */
void hda_output_automute_decide(const struct hda_output_plan *plan,
                                const enum hda_jack_state *jack_states,
                                unsigned char *output_enabled);

/* Short stable name for logs and the HUD: "line", "speaker", "hp", "out". */
const char *hda_output_role_name(enum hda_output_role role);

/* Short stable name of a jack state: "plugged", "empty", "no-detect". */
const char *hda_jack_state_name(enum hda_jack_state state);

#endif
