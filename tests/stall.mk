# The silent-stall instrument: what is a page still WAITING for at settle?
#
#   make probe-stall                    the whole real-page corpus, both modes
#   make probe-stall STALL_FIX="tests/fixtures/frameworks/*"   one corpus
#   make test-stall-control             the observer-effect control, asserted
#
# WHY THIS FRAGMENT EXISTS AT ALL. Every other instrument in this tree reports
# something that HAPPENED -- probe-webapi reports the globals a page reached for
# and missed, the scoreboard reports painted pixels and text runs, the console
# reports exceptions. All of them are blind to the failure a modern application
# page actually has here: NOTHING GOES WRONG. No exception, no failed request,
# no missing subresource; the shell renders and the content never arrives. This
# is the instrument for that class. See c/apps/browser/js_stall.h.
#
# It rides on $(BUILD)/webapi_probe rather than a binary of its own, and that is
# a decision rather than laziness: that binary already loads the committed
# corpus through the browser's own DOM, its own module loader and its own
# js_page event loop, and a second page driver would be a second definition of
# "settled" -- one jar, two doors, on the one value this whole measurement is
# relative to.

.PHONY: probe-stall probe-stall-frameworks test-stall-control

# The real-page corpus and the framework corpus are separate runs on purpose.
# The first says what real sites are parked on; the second says whether the
# cause is a FRAMEWORK-independent property of this browser, because seven
# independent build systems agreeing is evidence and one site is an anecdote.
STALL_FIX  ?= $(sort $(dir $(wildcard tests/fixtures/webapi/*/index.html)))
STALL_FW   ?= $(sort $(dir $(wildcard tests/fixtures/frameworks/*/index.html)))

# BOTH MODES, ALWAYS, and never one of them alone.
#
#   --stall-bare  the native promise/async census only. It walks the runtime's
#                 GC object list, takes no reference and calls nothing, so it
#                 CANNOT perturb what it measures. Ground truth.
#   --stall       ... plus the trackers that attribute a pending thing to an
#                 origin, an observer or a callback that never ran. This half
#                 registers a settlement handler on every fetch promise, which
#                 is itself a promise reaction.
#
# Printing only the armed run would publish a census the instrument had a hand
# in. Printing both makes the instrument's own contribution a visible number
# instead of a caveat in a comment.
probe-stall: webapi-link-check $(BUILD)/webapi_probe
	@echo "== BARE (native census; no observer effect) =============="
	@$(BUILD)/webapi_probe --stall-bare $(STALL_FIX) 2>&1 | grep -E 'STALL|^  [a-z0-9-]+ +\.\.\.   ' || true
	@echo
	@echo "== ARMED (census + attribution) =========================="
	@$(BUILD)/webapi_probe --stall $(STALL_FIX) 2>&1 | grep -E 'STALL|^  [a-z0-9-]+ +\.\.\.   ' || true

probe-stall-frameworks: webapi-link-check $(BUILD)/webapi_probe
	@$(BUILD)/webapi_probe --stall $(STALL_FW) 2>&1 | grep -E 'STALL|^  [a-z0-9-]+ +\.\.\.   ' || true

# THE CONTROL, AND IT IS WATCHABLE FAILING.
#
# The claim being controlled is precisely: the bare census is not changed by
# anything except the trackers' own fetch reactions. So on a corpus where every
# fetch has settled by the settle point, the two runs must produce a BYTE-
# IDENTICAL promise/async census -- and if the trackers were perturbing the
# runtime in some way nobody accounted for, the diff below prints it.
#
# To watch it go red: add a `.then(function(){})` to any promise in
# c/apps/browser/js_stall.c's prelude that is not already tracked, rebuild, and
# `pending_awaited` moves in the armed run only. That is the whole failure mode
# this exists to catch, and it is one line away at all times.
test-stall-control: webapi-link-check $(BUILD)/webapi_probe
	@rm -f $(BUILD)/stall-bare.txt $(BUILD)/stall-armed.txt
	@$(BUILD)/webapi_probe --stall-bare $(STALL_FIX) 2>&1 \
	    | grep -E '(promise|async|async-at) ' > $(BUILD)/stall-bare.txt || true
	@$(BUILD)/webapi_probe --stall $(STALL_FIX) 2>&1 \
	    | grep -E '(promise|async|async-at) ' > $(BUILD)/stall-armed.txt || true
	@test -s $(BUILD)/stall-bare.txt || { \
	    echo "test-stall-control: the BARE run produced no census at all."; \
	    echo "  That is the instrument failing, not the browser. Run:"; \
	    echo "    $(BUILD)/webapi_probe --stall-bare $(firstword $(STALL_FIX))"; \
	    exit 1; }
	@echo "test-stall-control: pending_awaited deltas (the documented effect):"
	@paste $(BUILD)/stall-bare.txt $(BUILD)/stall-armed.txt \
	  | sed -n 's/.*pending_awaited=\([0-9]*\).*pending_awaited=\([0-9]*\).*/  bare=\1 armed=\2/p' \
	  | sort | uniq -c || true
# THE COMPARISON MASKS THE ONE FIELD THAT IS ALLOWED TO MOVE, AND NOTHING ELSE.
#
# The first version of this rule diffed the two files and then excused any
# changed line that CONTAINED the string `pending_awaited`. Every promise line
# contains it -- it is one of six fields on the line -- so the excuse matched
# everything and the control passed while printing a diff in which
# promise_total, pending and rejected had all moved. It was watched doing
# exactly that (a deliberate stray `new Promise` in the prelude) before this
# rule was rewritten. Masking the FIELD, not the line, is the difference
# between a control and a decoration.
	@sed 's/pending_awaited=[0-9]*/pending_awaited=X/' $(BUILD)/stall-bare.txt  > $(BUILD)/stall-bare.msk
	@sed 's/pending_awaited=[0-9]*/pending_awaited=X/' $(BUILD)/stall-armed.txt > $(BUILD)/stall-armed.msk
	@if diff -u $(BUILD)/stall-bare.msk $(BUILD)/stall-armed.msk > $(BUILD)/stall-diff.txt; then \
	    echo "test-stall-control: ok -- with pending_awaited masked, the two censuses are identical"; \
	    echo "  ($$(grep -c . $(BUILD)/stall-bare.txt) census lines, every other field equal)"; \
	  else \
	    echo "test-stall-control: RED. The armed trackers moved something they must not."; \
	    echo "  Only pending_awaited may differ between the two runs (js_stall.h names why)."; \
	    cat $(BUILD)/stall-diff.txt; \
	    exit 1; \
	  fi
