"""Compile the real capture service with one specified incorrect behavior."""
from pathlib import Path
import sys

source = Path("c/kernel/audio/capture.c").read_text()
mutations = {
    "active-slot": ("? done - g_capdev->periods + 1 : 0;",
                    "? done - g_capdev->periods : 0;"),
    "wrong-device": ("if (d != g_capdev || !g_cap_running) return;",
                     "(void)d; if (!g_cap_running) return;"),
}
needle, replacement = mutations[sys.argv[1]]
assert source.count(needle) == 1, "mutation must match once"
# Preserve quoted includes' resolution while keeping generated code outside
# the production tree. The test still links the real PCM implementation.
source = source.replace(needle, replacement).replace(
    '"../../drivers/core/io_domain.h"', '"c/drivers/core/io_domain.h"')
destination = Path(sys.argv[2])
destination.write_text(source)
test = Path("tests/drivers/audio/framework/capture_test.c").read_text()
Path(sys.argv[3]).write_text(test.replace(
    '"c/kernel/audio/capture.c"', '"' + str(destination.resolve()) + '"'))
