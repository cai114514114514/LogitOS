#ifndef HDA_STATUS_H
#define HDA_STATUS_H

/* What the hardware panel (HWHUD=1) may ask the HDA driver about itself.
 *
 * WHY A SEPARATE HEADER. The panel is the only instrument on a machine with no
 * serial cable, so the driver has to publish enough for one photograph to
 * answer "which jack, which pin, is it driving, is the DMA engine moving".
 * Keeping the answer a plain struct means the panel never reaches into driver
 * internals and the host gates can fill it with chosen values. */

#include <stdint.h>
#include "hda_output_paths.h"

/* The last codec transaction that did not complete, so the panel can say
 * "err cmd-timeout@1b" instead of leaving the cause to a serial log nobody can
 * read. NONE means no command has failed since the controller came up. */
enum hda_codec_error {
    HDA_CODEC_ERROR_NONE = 0,
    HDA_CODEC_ERROR_COMMAND_TIMEOUT = 1,
    HDA_CODEC_ERROR_POWER_REFUSED = 2,
    HDA_CODEC_ERROR_POWER_TIMEOUT = 3,
    HDA_CODEC_ERROR_OUTPUT_SETUP_FAILED = 4
};

/* One routed output pin and the two facts a person needs about it: what its
 * jack says and whether the pin is currently driving (Pin Widget Control
 * output-enable, read back from the codec rather than assumed). */
struct hda_pin_status {
    enum hda_output_role role;
    unsigned nid;
    enum hda_jack_state jack;
    int output_enabled;
};

struct hda_controller_status {
    /* PCI location of the HDA function (bus, device/slot, function). */
    unsigned bus;
    unsigned slot;
    unsigned function;

    /* A GPU's HDMI/DP audio function: enumerated for identity only, output
     * waits for display-engine programming (GPU stage A). */
    int is_display_function;

    int bound;                         /* the driver owns this function */
    const char *not_bound_reason;      /* static text; set when !bound */

    int codec_present;
    uint32_t codec_vendor_and_device;  /* PARAM_VENDOR: vendor 31:16, device 15:0 */

    unsigned pin_count;
    struct hda_pin_status pins[HDA_OUTPUT_PATHS_MAX];

    int stream_running;                /* output stream descriptor RUN bit */
    int position_advancing;            /* LPIB moved since the previous status read */
    uint64_t bytes_played;             /* monotonic count folded from LPIB */
    uint64_t interrupts;               /* completed-period interrupts handled */
    const char *interrupt_mode;        /* how the controller's interrupt is wired:
                                        * "msix", "msi", "intx" or "polled" (none).
                                        * Zero interrupts with a moving LPIB is a
                                        * different fault from none being wired. */

    enum hda_codec_error last_error;
    unsigned last_error_nid;
};

/* Fill `status` for the index-th HDA function the driver has met, in probe
 * order. Returns 0, or -1 when there is no such function. Reading the analog
 * controller's status also refreshes its jack sense, so call it from thread
 * context only (it sends codec verbs). */
int hda_status_get(unsigned index, struct hda_controller_status *status);

/* Short stable name of the error for the panel: "none", "cmd-timeout", ... */
const char *hda_codec_error_name(enum hda_codec_error error);

/* Make every routed output jack drive regardless of jack sense while nonzero
 * (the boot test tone uses it so the tone is heard on whichever jack the
 * speakers are in, even if a floating front-panel line reads "plugged"). */
void hda_automute_suspend(int suspended);

#endif
