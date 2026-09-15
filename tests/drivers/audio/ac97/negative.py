"""Mutate real production behavior; literal host oracles must observe failure."""
import pathlib
import sys
source = pathlib.Path(sys.argv[1]).read_text()
if len(sys.argv) > 3 and sys.argv[3] == "channel-readback":
    needle = "AC97_WARM_RESET | channel_mask"
    replacement = "AC97_WARM_RESET"
elif len(sys.argv) > 3 and sys.argv[3] == "capture-only":
    needle = "if (card.sound_registered)\n        snd_init();"
    replacement = "snd_init();"
elif len(sys.argv) > 3 and sys.argv[3] == "capture":
    needle = "snd_capture_period_elapsed(&owner->input);"
    replacement = "(void)owner;"
elif len(sys.argv) > 3 and sys.argv[3] == "position":
    needle = "if (controller->terminal_awaiting_fetch &&"
    replacement = "if (0 && controller->terminal_awaiting_fetch &&"
else:
    needle = "(AC97_PERIOD_BYTES / 2);"
    replacement = "(AC97_PERIOD_BYTES / 4);"
assert source.count(needle) == 1
pathlib.Path(sys.argv[2]).write_text(source.replace(needle, replacement))
