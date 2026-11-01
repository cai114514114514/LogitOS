#include "hda_output_paths.h"

/* See hda_output_paths.h for the purpose and the list of cited sources.
 * Model and QEMU evidence only: not verified on X79-VG4 hardware. */

/* More candidate pins than this are not expected on any codec; the extras are
 * dropped after sorting, so the highest-priority pins always survive. */
#define HDA_OUTPUT_CANDIDATES_MAX               32

/* Sort-key layout: role in the top byte so line-outs precede speakers and
 * speakers precede headphones, then association, sequence and NID. Association
 * 0 (unassociated) and 15 (independent) rank after every real association, the
 * ordering hda_route.c already uses for its single-pin search (HDA 1.0a
 * 7.3.3.31 defines 0 and 15 as not belonging to a group). */
#define SORT_KEY_ROLE_SHIFT                     24
#define SORT_KEY_ASSOCIATION_SHIFT              16
#define SORT_KEY_SEQUENCE_SHIFT                 8
#define ASSOCIATION_UNASSOCIATED                0u
#define ASSOCIATION_INDEPENDENT                 15u
#define ASSOCIATION_RANK_LAST                   16u

struct pin_candidate {
    const struct hda_widget *pin;
    enum hda_output_role role;
    uint32_t sort_key;
};

/* Extract one Configuration Default field. A named helper keeps every shift
 * and mask at its call site readable instead of repeated by hand. */
static unsigned config_field(uint32_t configuration, unsigned shift, unsigned mask)
{
    return (configuration >> shift) & mask;
}

/* NIDs name nodes; array position is not a graph edge. The table is at most
 * 256 entries (the eight-bit NID space), so a linear scan is bounded. */
static const struct hda_widget *find_widget_by_nid(const struct hda_widget *widgets,
                                                   size_t widget_count, unsigned nid)
{
    for (size_t index = 0; index < widget_count; index++) {
        if (widgets[index].nid == nid) {
            return &widgets[index];
        }
    }
    return NULL;
}

/* Decide whether a pin is an analog output worth routing and what for. The
 * rules are snd_hda_parse_pin_defcfg()'s: "no connection" is skipped, the
 * Default Device picks the class, and a line-out that firmware marked
 * fixed-function is really an internal speaker (Linux's "buggy BIOS"
 * workaround). Output capability is required on top, because Linux checks it
 * later when it builds the path and a pin that cannot drive output has no
 * path. Digital outputs (S/PDIF) and everything else return 0. */
static int classify_output_pin(const struct hda_widget *pin, enum hda_output_role *role)
{
    unsigned connectivity;
    unsigned device;

    if (pin->type != HDA_ROUTE_PIN) {
        return 0;
    }
    if (!(pin->pin_caps & HDA_PIN_CAPABILITY_OUTPUT)) {
        return 0;
    }
    if (pin->connection_count == 0) {
        return 0;
    }
    connectivity = config_field(pin->cfg, HDA_CONFIG_PORT_CONNECTIVITY_SHIFT,
                                HDA_CONFIG_PORT_CONNECTIVITY_MASK);
    if (connectivity == HDA_PORT_CONNECTIVITY_NO_CONNECTION) {
        return 0;
    }
    device = config_field(pin->cfg, HDA_CONFIG_DEFAULT_DEVICE_SHIFT,
                          HDA_CONFIG_DEFAULT_DEVICE_MASK);
    if (device == HDA_DEFAULT_DEVICE_LINE_OUT) {
        *role = connectivity == HDA_PORT_CONNECTIVITY_FIXED_FUNCTION
                    ? HDA_OUTPUT_ROLE_SPEAKER
                    : HDA_OUTPUT_ROLE_LINE_OUT;
        return 1;
    }
    if (device == HDA_DEFAULT_DEVICE_SPEAKER) {
        *role = HDA_OUTPUT_ROLE_SPEAKER;
        return 1;
    }
    if (device == HDA_DEFAULT_DEVICE_HEADPHONE_OUT) {
        *role = HDA_OUTPUT_ROLE_HEADPHONE;
        return 1;
    }
    return 0;
}

/* Build the ordering key described at the top of this file. Smaller routes
 * first. The NID is the final tiebreak so equal pins plan reproducibly. */
static uint32_t candidate_sort_key(const struct hda_widget *pin, enum hda_output_role role)
{
    unsigned association = config_field(pin->cfg, HDA_CONFIG_ASSOCIATION_SHIFT,
                                        HDA_CONFIG_ASSOCIATION_MASK);
    unsigned sequence = config_field(pin->cfg, 0, HDA_CONFIG_SEQUENCE_MASK);

    if (association == ASSOCIATION_UNASSOCIATED ||
        association == ASSOCIATION_INDEPENDENT) {
        association = ASSOCIATION_RANK_LAST;
    }
    return ((uint32_t)role << SORT_KEY_ROLE_SHIFT) |
           (association << SORT_KEY_ASSOCIATION_SHIFT) |
           (sequence << SORT_KEY_SEQUENCE_SHIFT) |
           pin->nid;
}

/* Order the candidates by key. Insertion sort: at most a few dozen entries,
 * no recursion, no allocation, stable output for the tests. */
static void sort_candidates(struct pin_candidate *candidates, unsigned candidate_count)
{
    for (unsigned index = 1; index < candidate_count; index++) {
        struct pin_candidate moving = candidates[index];
        unsigned slot = index;

        while (slot > 0 && candidates[slot - 1].sort_key > moving.sort_key) {
            candidates[slot] = candidates[slot - 1];
            slot--;
        }
        candidates[slot] = moving;
    }
}

/* HDA firmware numbers the rear jacks front, centre/LFE, surround, side (the
 * sequence field), while every other audio stack, and Linux's DAC assignment,
 * expects front, surround, centre/LFE, side. snd_hda_parse_pin_defcfg() swaps
 * the middle two when there are three or four line-outs; doing the same makes
 * the second line-out get the second DAC, the way a Linux dump of this codec
 * reads. Line-outs sort first, so they are the leading candidates. */
#define SURROUND_REORDER_MIN_LINE_OUTS          3u
#define SURROUND_REORDER_MAX_LINE_OUTS          4u

static void reorder_surround_and_center_lfe(struct pin_candidate *candidates,
                                            unsigned candidate_count)
{
    unsigned line_out_count = 0;

    while (line_out_count < candidate_count &&
           candidates[line_out_count].role == HDA_OUTPUT_ROLE_LINE_OUT) {
        line_out_count++;
    }
    if (line_out_count >= SURROUND_REORDER_MIN_LINE_OUTS &&
        line_out_count <= SURROUND_REORDER_MAX_LINE_OUTS) {
        struct pin_candidate middle = candidates[1];

        candidates[1] = candidates[2];
        candidates[2] = middle;
    }
}

/* True for a converter that can carry an analog pin's signal: an audio output
 * widget that is not the digital (S/PDIF) one. */
static int is_analog_converter(const struct hda_widget *widget)
{
    return widget->type == HDA_ROUTE_DAC &&
           !(widget->caps & HDA_WIDGET_CAPABILITY_DIGITAL);
}

/* Is this converter already feeding an earlier planned path? A converter
 * carries one stream, so an unclaimed one gives the pin its own DAC; a claimed
 * one means the pin would share it (Linux is_dac_already_used()). */
static int converter_is_claimed(const struct hda_output_plan *plan, unsigned nid)
{
    for (unsigned index = 0; index < plan->path_count; index++) {
        const struct hda_route *route = &plan->paths[index].route;

        if (route->nodes[route->count - 1].nid == nid) {
            return 1;
        }
    }
    return 0;
}

/* Find a connection-list path from `pin` to `converter`, at most one mixer or
 * selector deep (Linux is_reachable_path(), bounded the same way as
 * hda_route.c: pin -> [mixer] -> DAC). A direct edge wins over a mixer edge at
 * the same pin index. Fills `route` and returns 1, or returns 0. */
static int route_pin_to_converter(const struct hda_widget *widgets, size_t widget_count,
                                  const struct hda_widget *pin,
                                  const struct hda_widget *converter,
                                  struct hda_route *route)
{
    for (unsigned edge = 0; edge < pin->connection_count; edge++) {
        const struct hda_widget *source =
            find_widget_by_nid(widgets, widget_count, pin->connections[edge]);

        if (!source) {
            continue;
        }
        if (source == converter) {
            route->count = 2;
            route->nodes[0] = *pin;
            route->nodes[1] = *converter;
            route->indices[0] = edge;
            return 1;
        }
        if (source->type != HDA_ROUTE_MIXER && source->type != HDA_ROUTE_SELECTOR) {
            continue;
        }
        for (unsigned input = 0; input < source->connection_count; input++) {
            if (source->connections[input] == converter->nid) {
                route->count = 3;
                route->nodes[0] = *pin;
                route->nodes[1] = *source;
                route->nodes[2] = *converter;
                route->indices[0] = edge;
                route->indices[1] = input;
                return 1;
            }
        }
    }
    return 0;
}

/* Give one pin a DAC the way hda_generic.c does. First choice is a converter
 * nobody uses yet, scanned in NID order like look_for_dac(). When the codec
 * has run out of converters the pin shares one that is already playing, the
 * primary (first) path's converter before any other, so a headphone jack on a
 * four-DAC codec rides the front DAC through its output mixer. Returns 1 when
 * the path was filled in. */
static int plan_one_pin(const struct hda_widget *widgets, size_t widget_count,
                        const struct pin_candidate *candidate,
                        struct hda_output_path *path,
                        const struct hda_output_plan *plan)
{
    for (size_t index = 0; index < widget_count; index++) {
        const struct hda_widget *converter = &widgets[index];

        if (!is_analog_converter(converter) || converter_is_claimed(plan, converter->nid)) {
            continue;
        }
        if (route_pin_to_converter(widgets, widget_count, candidate->pin, converter,
                                   &path->route)) {
            path->converter_shared = 0;
            return 1;
        }
    }
    for (unsigned earlier = 0; earlier < plan->path_count; earlier++) {
        const struct hda_route *earlier_route = &plan->paths[earlier].route;
        const struct hda_widget *converter =
            find_widget_by_nid(widgets, widget_count,
                               earlier_route->nodes[earlier_route->count - 1].nid);

        if (converter &&
            route_pin_to_converter(widgets, widget_count, candidate->pin, converter,
                                   &path->route)) {
            path->converter_shared = 1;
            return 1;
        }
    }
    return 0;
}

/* Negative-control hook: reproduces the defect "only the first output pin is
 * routed" for the ALC887 board gate. Production builds never define the macro,
 * so the limit is the whole plan table. */
static unsigned planned_path_limit(void)
{
#ifdef HDA_NEGCTL_ROUTE_FIRST_PIN_ONLY
    return 1;
#else
    return HDA_OUTPUT_PATHS_MAX;
#endif
}

/* Entry point of the planner: classify every pin, order the candidates the way
 * Linux does, then give each one a converter. Doing it in this order is what
 * makes the line-outs claim the converters first and leaves the headphone jack
 * to share the front one when the codec runs out (hda_generic.c assigns DACs
 * line-outs, then speakers, then headphones). */
int hda_output_plan_build(const struct hda_widget *widgets, size_t widget_count,
                          struct hda_output_plan *plan)
{
    struct pin_candidate candidates[HDA_OUTPUT_CANDIDATES_MAX];
    unsigned candidate_count = 0;

    if (!plan) {
        return 0;
    }
    plan->path_count = 0;
    if (!widgets) {
        return 0;
    }
    for (size_t index = 0; index < widget_count && candidate_count < HDA_OUTPUT_CANDIDATES_MAX;
         index++) {
        enum hda_output_role role = HDA_OUTPUT_ROLE_ANY;

        if (!classify_output_pin(&widgets[index], &role)) {
            continue;
        }
        candidates[candidate_count].pin = &widgets[index];
        candidates[candidate_count].role = role;
        candidates[candidate_count].sort_key = candidate_sort_key(&widgets[index], role);
        candidate_count++;
    }
    sort_candidates(candidates, candidate_count);
    reorder_surround_and_center_lfe(candidates, candidate_count);
    for (unsigned index = 0; index < candidate_count && plan->path_count < planned_path_limit();
         index++) {
        struct hda_output_path *path = &plan->paths[plan->path_count];

        path->role = candidates[index].role;
        path->jack_detectable = hda_pin_jack_detectable(candidates[index].pin);
        if (plan_one_pin(widgets, widget_count, &candidates[index], path, plan)) {
            plan->path_count++;
        }
    }
    return (int)plan->path_count;
}

/* The fallback (hda_route_find) yields one route and no role; wrapping it as a
 * one-path plan lets the driver run it through exactly the same setup and
 * poll code, so the fallback cannot rot into a second implementation. */
void hda_output_plan_from_single_route(const struct hda_route *route,
                                       struct hda_output_plan *plan)
{
    struct hda_output_path *path = &plan->paths[0];

    plan->path_count = 1;
    path->role = HDA_OUTPUT_ROLE_ANY;
    path->route = *route;
    path->converter_shared = 0;
    path->jack_detectable = hda_pin_jack_detectable(&route->nodes[0]);
}

/* Linux hda_jack.c is_jack_detectable(): a presence line is only believed when
 * the pin advertises it AND firmware did not flag it as unwired. Believing an
 * unwired line would read a floating input as "headphone plugged" and mute the
 * speakers for good. */
int hda_pin_jack_detectable(const struct hda_widget *pin)
{
    unsigned misc = config_field(pin->cfg, HDA_CONFIG_MISC_SHIFT, HDA_CONFIG_MISC_MASK);

    if (!(pin->pin_caps & HDA_PIN_CAPABILITY_PRESENCE_DETECT)) {
        return 0;
    }
    return !(misc & HDA_CONFIG_MISC_NO_PRESENCE_DETECT);
}

/* The Pin Widget Control value that makes this path's pin drive. Computed here
 * (not inside the verb compiler) because auto-mute needs the same value later
 * to switch the pin back on. */
unsigned hda_output_path_pin_control(const struct hda_output_path *path)
{
    const struct hda_widget *pin = &path->route.nodes[0];
    unsigned control = HDA_PIN_CONTROL_OUTPUT_ENABLE;

    /* A headphone pin drives a low-impedance load through its own amplifier,
     * which HEADPHONE_ENABLE switches on, but only when Pin Capabilities
     * advertise it. HDA_OUTPUT_ROLE_ANY keeps the historical rule of hda_route.c
     * (enable it whenever the pin can) so the fallback behaves as before. */
    if (path->role == HDA_OUTPUT_ROLE_HEADPHONE || path->role == HDA_OUTPUT_ROLE_ANY) {
        if (pin->pin_caps & HDA_PIN_CAPABILITY_HEADPHONE_DRIVE) {
            control |= HDA_PIN_CONTROL_HEADPHONE_ENABLE;
        }
    }
    return control;
}

/* One path's complete bring-up commands. Reusing hda_route's compiler keeps the
 * amplifier, selector and power rules (proved by route_test.c) identical for
 * every path; only the pin control differs by role. */
int hda_output_path_verbs(const struct hda_output_path *path, unsigned format,
                          unsigned stream_tag, struct hda_route_verb *verbs,
                          size_t capacity)
{
    if (!path) {
        return -1;
    }
    return hda_route_verbs_with_pin_control(&path->route, format, stream_tag,
                                            hda_output_path_pin_control(path),
                                            verbs, capacity);
}

/* Negative-control hook: reproduces the defect "the headphone jack does not
 * mute anything". Production builds never define the macro. */
static int automute_is_applied(void)
{
#ifdef HDA_NEGCTL_IGNORE_AUTOMUTE
    return 0;
#else
    return 1;
#endif
}

/* Linux hda_generic.c snd_hda_gen_update_outputs(): line-outs follow "no
 * headphone plugged" (automute_lo); speakers follow "no headphone and no
 * line-out plugged" (automute_speaker); headphone pins never mute. Pure so the
 * policy is tested without a codec, and so the driver cannot grow a second,
 * slightly different copy. */
void hda_output_automute_decide(const struct hda_output_plan *plan,
                                const enum hda_jack_state *jack_states,
                                unsigned char *output_enabled)
{
    int headphone_plugged = 0;
    int line_out_plugged = 0;

    for (unsigned index = 0; index < plan->path_count; index++) {
        if (jack_states[index] != HDA_JACK_PLUGGED) {
            continue;
        }
        if (plan->paths[index].role == HDA_OUTPUT_ROLE_HEADPHONE) {
            headphone_plugged = 1;
        }
        if (plan->paths[index].role == HDA_OUTPUT_ROLE_LINE_OUT) {
            line_out_plugged = 1;
        }
    }
    for (unsigned index = 0; index < plan->path_count; index++) {
        int enabled = 1;

        if (automute_is_applied()) {
            if (plan->paths[index].role == HDA_OUTPUT_ROLE_LINE_OUT) {
                enabled = !headphone_plugged;
            }
            if (plan->paths[index].role == HDA_OUTPUT_ROLE_SPEAKER) {
                enabled = !headphone_plugged && !line_out_plugged;
            }
        }
        output_enabled[index] = (unsigned char)enabled;
    }
}

/* Logs and the panel print roles as words so a photograph of the screen needs
 * no legend. */
const char *hda_output_role_name(enum hda_output_role role)
{
    switch (role) {
    case HDA_OUTPUT_ROLE_LINE_OUT:
        return "line";
    case HDA_OUTPUT_ROLE_SPEAKER:
        return "speaker";
    case HDA_OUTPUT_ROLE_HEADPHONE:
        return "hp";
    case HDA_OUTPUT_ROLE_ANY:
        break;
    }
    return "out";
}

/* The panel must be able to tell "empty" (the jack reported nothing plugged)
 * from "no-detect" (the jack cannot report), so each state has its own word. */
const char *hda_jack_state_name(enum hda_jack_state state)
{
    switch (state) {
    case HDA_JACK_PLUGGED:
        return "plugged";
    case HDA_JACK_EMPTY:
        return "empty";
    case HDA_JACK_NO_DETECT:
        break;
    }
    return "no-detect";
}
