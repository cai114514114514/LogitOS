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

.PHONY: scoreboard scoreboard-1 scoreboard-diff scoreboard-quick test-sites-merge test-sites-merge-negctl

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
# BOXES=--boxes adds the display-list dump to that one site's serial log.
# Not on the full scoreboard and not on by default: the list of a real page is
# thousands of lines on the same serial console every other measurement here
# arrives on, and an instrument that floods the log it writes to has replaced
# the thing it was measuring. Use it when the question is "how wide does
# layout think that box is" -- e.g.
#   make scoreboard-1 SITE=stripe BOXES=--boxes
scoreboard-1: $(ISO) $(DISK)
	@test -n "$(SITE)" || { echo "usage: make scoreboard-1 SITE=<name from tests/qmp/sites_corpus.tsv>"; exit 2; }
	python3 tests/qmp/sites_run.py --iso $(ISO) --disk $(DISK) \
	    --only $(SITE) --jobs 1 --repeat 1 --label $(SCOREBOARD_DATE) $(BOXES)

# THE PRODUCT. A snapshot on its own is a list of complaints; the delta between
# two is the only form in which this line's work is visible.
#   make scoreboard-diff FROM=tests/scoreboard/2026-08-08.json TO=tests/scoreboard/2026-08-09.json
scoreboard-diff:
	@test -n "$(FROM)" -a -n "$(TO)" || { echo "usage: make scoreboard-diff FROM=<a.json> TO=<b.json>"; exit 2; }
	python3 tests/qmp/sites_run.py --diff $(FROM) $(TO)

# The merge rule, gated. A HOST test with no QEMU in it: merge_repeats is pure
# bookkeeping over records, and the bug it had was pure bookkeeping too -- it
# kept the WORST verdict of N runs, so a HARNESS record (guest {}, pixels {})
# evicted a complete measurement sitting in the same pass. Three sites
# published a row of dashes that way in tests/scoreboard/0820-g4b.
test-sites-merge:
	@python3 tests/unit/sites_merge_test.py

# NEGATIVE CONTROL: the shipped-until-2026-08-25 rule, restored on a switch so
# it can be watched discarding a measurement instead of that behaviour living
# only in a commit message. It is the PLAUSIBLE WRONG RULE, not a mutilation --
# ranking every run by severity is what anybody would write, and it is right
# until one of the runs measured nothing. The count is asserted because "some
# checks failed" would also be satisfied by a merge rule broken another way.
test-sites-merge-negctl:
	@out=$$(SITES_MERGE_WORST_WINS=1 python3 tests/unit/sites_merge_test.py 2>&1); n=$$(echo "$$out" | grep -c "^  FAIL"); echo "$$out" | tail -2; if [ "$$n" -ne 3 ]; then echo "CONTROL BROKEN: wanted exactly 3 reddened checks, got $$n"; exit 1; fi; echo "negative control ok: worst-wins reddens exactly the 3 checks about discarding a measurement"
