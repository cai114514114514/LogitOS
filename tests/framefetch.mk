# tests/framefetch.mk -- the fetcher-safety prerequisite for a second
# browsing context (js_frame.c, not yet built as of 2026-08-30).
#
# WHAT THIS GATES, and what it does not. js_worker.c:16-24 raised, and the
# scope pass on adding a second context settled: bfetch (c/apps/browser/
# bfetch.h, implemented in browser_rt.c) is ALREADY safe for a same-runtime,
# single-threaded second context -- there is no second thread, so there is no
# race to lock against, and bfetch_resolve()/bfetch_start_from() already take
# an explicit base and ignore the page's g_base when given one. The one real
# hazard is g_base itself: ONE JAR, and bfetch_set_base() is meant to have
# exactly ONE caller (the top-level page). A frame that called it would
# silently retarget every subsequent PAGE fetch at the frame's own origin.
#
# This fragment is therefore two gates, not one implementation:
#   test-framefetch                    the property itself, proved against
#                                       tests/unit/loader_fakebfetch.c (the
#                                       same fixture js_worker.c's own host
#                                       test already uses): a page fetch, a
#                                       frame fetch on a different origin
#                                       with its own explicit base, then a
#                                       second page fetch that must still
#                                       resolve against the page's base.
#   test-framefetch-negctl-discipline  the SAME file built
#                                       -DFRAME_FETCH_NO_DISCIPLINE, which
#                                       makes the simulated frame call
#                                       bfetch_sync() with a bare relative
#                                       ref (no base of its own) instead of
#                                       resolving explicitly first -- must
#                                       FAIL, and fail by landing on the
#                                       WRONG origin (silently), which is the
#                                       actual danger this whole gate exists
#                                       to name before js_frame.c can reuse
#                                       the mistake.
#   test-framefetch-discipline         the STATIC half: greps any
#                                       c/apps/browser/js_frame*.c for a
#                                       bfetch_set_base() call. SKIPS (by
#                                       name, not silently) until that file
#                                       exists. Its own selftest --
#                                       test-framefetch-discipline-selftest,
#                                       a prerequisite -- proves the grep
#                                       fires on a synthetic violation and
#                                       does not fire on clean code, so this
#                                       is not a control that can only be
#                                       watched passing.
#
# WHAT THIS DOES NOT GATE: real sockets, TLS, HTTP/2 multiplexing, or the
# connection pool -- those are browser_rt.c's own tests (http1_test.c,
# hpool_test.c) and this fragment does not re-test the fake standing in for
# them. It does not gate js_frame.c's DOM, script sink, or scheduler, because
# none of that exists yet -- see tools/check_frame_base_discipline.py's
# SKIPPED message, which is the honest state of that half.

FRAMEFETCH_TEST_SRC := tests/unit/frame_fetch_test.c tests/unit/loader_fakebfetch.c
FRAMEFETCH_CF := -Ic/apps/browser -Itests/unit

.PHONY: test-framefetch test-framefetch-negctl-discipline \
        test-framefetch-discipline test-framefetch-discipline-selftest

$(BUILD)/frame_fetch_test: $(FRAMEFETCH_TEST_SRC)
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -Wall -Wextra -Wno-unused-parameter $(FRAMEFETCH_CF) \
	    -o $@ $(FRAMEFETCH_TEST_SRC)

test-framefetch: $(BUILD)/frame_fetch_test
	@$(BUILD)/frame_fetch_test

# The negctl is a PREREQUISITE of test-framefetch, not a sibling -- same
# reason tests/worker.mk gives: NOT_CI drops every test-*-negctl from the
# suite listing on the ground that a control is "run by its positive
# counterpart", and a control reachable only by hand is a control nobody
# runs. Making it a prerequisite here is what keeps it out of the stranded
# list without a ci- line pretending it ran.
$(BUILD)/frame_fetch_test_negctl_discipline: $(FRAMEFETCH_TEST_SRC)
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -Wall -Wextra -Wno-unused-parameter $(FRAMEFETCH_CF) \
	    -DFRAME_FETCH_NO_DISCIPLINE -o $@ $(FRAMEFETCH_TEST_SRC)

test-framefetch-negctl-discipline: $(BUILD)/frame_fetch_test_negctl_discipline
	@if $(BUILD)/frame_fetch_test_negctl_discipline > $(BUILD)/frame_fetch_negctl.log 2>&1; then \
	    echo "test-framefetch-negctl-discipline: FAILED -- the isolation check PASSED"; \
	    echo "  against a build where the frame fetches with an undisciplined"; \
	    echo "  (implicit-base) call, so it is not measuring the property it claims to."; \
	    exit 1; \
	 else \
	    echo "test-framefetch-negctl-discipline: ok -- the control catches it:"; \
	    grep -E 'FAIL' $(BUILD)/frame_fetch_negctl.log | head -8; \
	 fi

test-framefetch-discipline-selftest:
	@python3 tools/check_frame_base_discipline.py --selftest

test-framefetch-discipline: test-framefetch-discipline-selftest
	@python3 tools/check_frame_base_discipline.py
