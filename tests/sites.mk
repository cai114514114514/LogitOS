# ============================ the site scoreboard =============================
#
# "Does the real web work" as a NUMBER, taken the same way every day, so the
# answer tomorrow is comparable with the answer today. Not a pass/fail gate:
# none of these targets fail because a site failed. They fail only if the
# measurement could not be taken.
#
# Its own fragment for the reason every other tests/*.mk here says: this tree is
# worked on by several lines at once and a whole-file Makefile edit from a
# concurrent line cannot delete a file it does not open.
#
# WHY IT IS NOT PART OF `make test`
# The corpus is eighteen LIVE sites. They change under you, they rate-limit,
# they go down, and a CI gate that goes red because bilibili shipped a new
# bundle teaches nobody anything. `make scoreboard` is run deliberately, its
# output is committed as a dated snapshot, and `make scoreboard-diff` between
# two snapshots is the only thing here anyone should read as a result.
#
# WHAT ONE ROW COSTS: one whole QEMU boot. Never two sites in one boot -- see
# the header of tests/qmp/qmp_site.py for the two false failures that bought
# that rule. Boots run in parallel (SITE_JOBS, default 5); a site never does.

SCOREBOARD_DATE ?= $(shell date +%F)
SCOREBOARD_JOBS ?= 5
SCOREBOARD_REPEAT ?= 2

.PHONY: scoreboard scoreboard-1 scoreboard-diff scoreboard-quick

# The full corpus, every site twice (a site whose two runs disagree is recorded
# FLAKY and is not scored -- the live web earns that).
scoreboard: $(ISO) $(DISK)
	python3 tests/qmp/sites_run.py --iso $(ISO) --disk $(DISK) \
	    --jobs $(SCOREBOARD_JOBS) --repeat $(SCOREBOARD_REPEAT) \
	    --label $(SCOREBOARD_DATE)

# One pass, no repeats -- for when you are watching a fix land, not publishing.
scoreboard-quick: $(ISO) $(DISK)
	python3 tests/qmp/sites_run.py --iso $(ISO) --disk $(DISK) \
	    --jobs $(SCOREBOARD_JOBS) --repeat 1 --label $(SCOREBOARD_DATE)

# One site, by its name in tests/qmp/sites_corpus.tsv:  make scoreboard-1 SITE=bing
scoreboard-1: $(ISO) $(DISK)
	@test -n "$(SITE)" || { echo "usage: make scoreboard-1 SITE=<name from tests/qmp/sites_corpus.tsv>"; exit 2; }
	python3 tests/qmp/sites_run.py --iso $(ISO) --disk $(DISK) \
	    --only $(SITE) --jobs 1 --repeat 1 --label $(SCOREBOARD_DATE)

# THE PRODUCT. A snapshot on its own is a list of complaints; the delta between
# two is the only form in which this line's work is visible.
#   make scoreboard-diff FROM=tests/scoreboard/2026-08-08.json TO=tests/scoreboard/2026-08-09.json
scoreboard-diff:
	@test -n "$(FROM)" -a -n "$(TO)" || { echo "usage: make scoreboard-diff FROM=<a.json> TO=<b.json>"; exit 2; }
	python3 tests/qmp/sites_run.py --diff $(FROM) $(TO)
