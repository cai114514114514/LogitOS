"""Build one incorrect lifecycle transition into an isolated source copy."""
from pathlib import Path
import sys

source = Path("c/kernel/audio/mixer.c").read_text()
mutations = {
    "reinitialize": ("if (g_initialized) {", "if (0) {"),
    "wrong-device": ("if (device == g_dev && g_running) {",
                     "(void)device; if (g_running) {"),
    "active-slot": ("if (g_fill - done < g_dev->periods) {", "if (1) {"),
    "scratch-channels": ("#define SND_MAX_APP_CHANNELS 8u", "#define SND_MAX_APP_CHANNELS 2u"),
}
needle, replacement = mutations[sys.argv[1]]
assert source.count(needle) == 1, "mutation must match exactly once"
source = source.replace(needle, replacement).replace(
    '"../../drivers/core/io_domain.h"', '"c/drivers/core/io_domain.h"')
destination = Path(sys.argv[2])
destination.write_text(source)
test = Path("tests/drivers/audio/playback/test.c").read_text()
Path(sys.argv[3]).write_text(test.replace(
    '"c/kernel/audio/mixer.c"', '"' + str(destination.resolve()) + '"'))
