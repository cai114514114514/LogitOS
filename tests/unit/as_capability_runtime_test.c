/* SPDX-License-Identifier: MIT */
#include "native.h"
#include <string.h>

static void scan_capability(void *slot)
{
    at_gc_mark(*(AtCap **)slot);
}

int main(void)
{
    at_caps_set(AS_CAP_FS_READ, "/usr//as/./lib/..");
    AtCap *saved = at_caps_value();
    if (!saved || at_cap_bits(saved) != AS_CAP_FS_READ || strcmp(at_cap_path(saved), "/usr/as")) {
        return 1;
    }
    void *frame = at_gc_frame();
    AtRoot root;
    at_gc_root(&root, &saved, scan_capability);

    /* A captured snapshot does not override a later launcher restriction.
     * Requiring multiple rights means all of them, not any one of them. */
    at_caps_set(AS_CAP_NET, NULL);
    if (!at_caps_have(AS_CAP_NET) || at_caps_have(AS_CAP_FS_READ) ||
        at_caps_have(AS_CAP_NET | AS_CAP_RAW)) {
        return 2;
    }
    at_gc_collect();
    if (at_cap_bits(saved) != AS_CAP_FS_READ || strcmp(at_cap_path(saved), "/usr/as")) {
        return 3;
    }

    /* Invalid launcher paths must not become unrestricted versions of the
     * requested grant. Losing a prefix is a denial, never an attenuation. */
    at_caps_set(AS_CAP_FS_READ, "relative");
    if (at_caps_have(AS_CAP_FS_READ) || at_caps_have(AS_CAP_NET)) {
        return 4;
    }
    char oversized[4098];
    memset(oversized, 'x', sizeof oversized - 1);
    oversized[0] = '/';
    oversized[sizeof oversized - 1] = 0;
    at_caps_set(AS_CAP_FS_READ, oversized);
    if (at_caps_have(AS_CAP_FS_READ)) {
        return 5;
    }

    at_caps_set(0, NULL);
    at_gc_restore(frame);
    at_gc_collect();
    return 0;
}
