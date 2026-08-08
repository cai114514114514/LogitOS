#!/usr/bin/env bash
# What one aui frame costs, measured on the machine at 1920x1200 under TCG.
#
# This toolkit is linked into every GUI app and redrawn every frame, and a
# compositor line is separately fighting for frame rate -- so the number matters
# and must not be estimated. Gallery times its own repaint with
# CLOCK_MONOTONIC (monotonic_ms() steps in 10 ms and cannot see a frame at all)
# and prints `[aui] page N frames=.. avg_us=.. max_us=..` on the serial console
# every two seconds. This boots, opens it, walks the pages, and prints them.
#
# Page 1 (Shapes) is the expensive one on purpose: it is nothing but rasterized
# geometry -- rounded fills, a stroked ring, gradients, four elevations of
# 8-slice shadow, five alpha composites -- so it is the WORST case, not the
# typical one. Page 0 (Controls) is what a real app's frame looks like.
set -u
ISO="${1:?usage: run-aui-bench.sh <iso> <disk.img>}"
DISK="${2:?usage: run-aui-bench.sh <iso> <disk.img>}"
exec python3 "$(dirname "$0")/../qmp/qmp_aui_bench.py" --iso "$ISO" --disk "$DISK" \
     --xres 1920 --yres 1200
