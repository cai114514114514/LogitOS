#!/usr/bin/env bash
# The site scoreboard, as one command that needs no Makefile.
#
#   bash tests/boot/run-sites.sh [iso] [disk]
#
# The Makefile in this tree is edited by several lines at once and a fragment's
# `-include` line has been deleted by a whole-file overwrite more than once, so
# this exists as the entry point that cannot be taken away: it runs the driver
# directly. `make scoreboard` (tests/sites.mk) does the same thing with the
# tree's usual defaults.
#
# It is NOT a gate. It exits 0 whether every site worked or none of them did;
# the exit code is about whether the measurement was taken. Read the table.
#
# Knobs (all optional):
#   SITE_JOBS=5        how many QEMUs boot at once (one site per boot, always)
#   SITE_REPEAT=2      runs per site; disagreement across runs is recorded FLAKY
#   SITE_LABEL=$(date) the snapshot name under tests/scoreboard/
#   SITE_ONLY=a,b      only these corpus rows
#   SITE_LOAD=240      seconds a page gets to reach `load done`
#   SITE_PAINT=75      seconds it then gets to finish painting and running
#   SITE_PCAP=1        keep a pcap per boot (large; for diagnosing one site)
set -u

ISO=${1:-build/logit.iso}
DISK=${2:-build/disk.img}
cd "$(dirname "$0")/../.." || exit 1

for f in "$ISO" "$DISK"; do
    if [ ! -s "$f" ]; then
        echo "run-sites: $f is missing or empty." >&2
        echo "  If a build is running in this worktree, wait for it: build/disk.img" >&2
        echo "  is deleted and rewritten by \`make\`, and a boot started in that" >&2
        echo "  window dies before executing an instruction." >&2
        exit 2
    fi
done

exec python3 tests/qmp/sites_run.py \
    --iso "$ISO" --disk "$DISK" \
    --jobs "${SITE_JOBS:-5}" \
    --repeat "${SITE_REPEAT:-2}" \
    --label "${SITE_LABEL:-$(date +%F)}" \
    ${SITE_ONLY:+--only "$SITE_ONLY"}
