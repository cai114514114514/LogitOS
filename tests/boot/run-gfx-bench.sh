#!/usr/bin/env bash
# What one frame costs on the machine, at three resolutions, with the rendering
# engine's share separated from gui_clear and from text.
#
# The separation is the point. The toolkit line measured 24-27 ms for a
# full-window repaint and found the dominant cost was aui_begin()'s
# unconditional gui_clear plus the kernel's glyph work -- NOT the rasterized
# primitives. Reporting a frame total alone would credit Open Logit with a cost
# it does not pay, and would hide the one it does.
#
# The gallery here is built against aui.c with -DAUI_COST, which brackets every
# drawing syscall with CLOCK_MONOTONIC. That costs a syscall per draw call, so
# the absolute total is inflated and only the RATIO is meaningful; `make
# bench-aui` reports the uninstrumented total at 1920x1200.
set -u
ISO="${1:?usage: run-gfx-bench.sh <iso> <disk.img>}"
DISK="${2:?usage: run-gfx-bench.sh <iso> <disk.img>}"
exec python3 "$(dirname "$0")/../qmp/qmp_gfx_bench.py" "$ISO" "$DISK"
