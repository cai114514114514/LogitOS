#!/usr/bin/env bash
# Simulate every recorded trace and print the gap between this kernel's clock
# and Belady's MIN.
#
#   tools/mmtrace/report.sh <mmsim> <trace dir> [--full]
#
# Memory is swept as a FRACTION of each workload's own footprint, not as a list
# of frame counts. A replacement policy is invisible when memory is larger than
# the working set (nothing is ever evicted, every policy is optimal, the ratio
# is exactly 1.00 and it means nothing) and nearly invisible when memory is far
# smaller (everything thrashes, every policy converges). The interesting band
# is memory somewhat below the working set, and where that band is depends on
# the workload, so the sweep has to be relative.
set -u
SIM="${1:?usage: report.sh <mmsim> <trace dir> [--full]}"
DIR="${2:?usage: report.sh <mmsim> <trace dir> [--full]}"
FULL="${3:-}"

FRACS="${MMSIM_FRACS:-5,10,25,50,75,90}"
shopt -s nullglob
TRACES=("$DIR"/*.mmt)
[ "${#TRACES[@]}" -gt 0 ] || { echo "no traces in $DIR -- run make mmtrace-video first"; exit 1; }

for t in "${TRACES[@]}"; do
    echo
    echo "############################################################"
    echo "## $(basename "$t")  ($(du -h "$t" | cut -f1) on disk)"
    echo "############################################################"
    "$SIM" --trace "$t" --space top --frac "$FRACS" ${MMSIM_EXTRA:-}
    if [ "$FULL" = "--full" ]; then
        echo
        echo "--- where the clock loses, over time (memory = 50% of footprint) ---"
        "$SIM" --trace "$t" --space top --frac 50 --windows 20
    fi
done
