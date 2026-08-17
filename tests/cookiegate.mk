# The cookie jar's SameSite rule and its eviction order, each watched failing.
#
#   make test-cookie-jar             the jar's own rules (216 checks)
#   make test-cookie-jar-negctl      three collapses of that rule, each of
#                                    which MUST fail, and must fail alone
#   make test-cookie-cors-negctl     the transport door wired the way it
#                                    shipped -- the bug itself, on a switch
#
# WHY A FRAGMENT RATHER THAN MORE LINES IN test-browser. cookie_test.c already
# built and ran inside test-browser's recipe with no target of its own, which
# meant there was no name to hang a negative control off. A control needs to be
# invocable separately from the thing it controls, or the only way to run it is
# to edit the source of the positive gate -- which is how a control ends up
# being run once, by hand, on the day it was written.
#
# THE THREE JAR CONTROLS ARE NOT VARIATIONS ON ONE IDEA. Two of them are
# opposites, and that is the point: CK_REQ_CROSS_SITE_NAV was added because a
# two-valued enum cannot say "cross-site, but a navigation", so collapsing it
# in EITHER direction has to break something, and something different.
#
#   COOKIE_NAV_IS_SAME_SITE     a navigation permits everything
#                               -> the Strict cell fails, alone
#   COOKIE_NAV_IS_CROSS_SITE    a navigation permits only None (pre-fix)
#                               -> the Lax/unset cell fails, alone
#   COOKIE_NO_EVICT_PREFERENCE  pure LRU, the old evict_lru
#                               -> the HttpOnly-survives cell fails, alone
#   COOKIE_NOFIT_IS_EMPTY       "nothing fit" reported as "no cookies", the
#                               old fold -> the two CK_E_NOFIT cells fail
#
# The last one is the exception to "alone" and says so: the ambiguity it
# restores is asserted from both ends deliberately -- once where nothing fits
# and once at the transport cap that used to be 1024 -- so it reddens two.
#
# "Alone" is checked, not assumed: each control's failure count must be exactly
# 1. A control that reddens the whole file proves the file runs, not that the
# property is carried by the assertion it was aimed at.

COOKIE_JAR_SRC := tests/unit/cookie_test.c c/net/http/cookies.c

.PHONY: test-cookie-jar test-cookie-jar-negctl test-cookie-cors-negctl

test-cookie-jar:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(BTEST_INC) -o $(BUILD)/cookie_jar_test $(COOKIE_JAR_SRC)
	@$(BUILD)/cookie_jar_test

test-cookie-jar-negctl:
	@mkdir -p $(BUILD)
	@rc=0; for spec in COOKIE_NAV_IS_SAME_SITE:1 COOKIE_NAV_IS_CROSS_SITE:1 \
	                   COOKIE_NO_EVICT_PREFERENCE:1 COOKIE_NOFIT_IS_EMPTY:2; do \
	    d=$${spec%%:*}; want=$${spec##*:}; \
	    $(CC) -O2 -w $(BTEST_INC) -D$$d -o $(BUILD)/cookie_jar_negctl $(COOKIE_JAR_SRC) || exit 1; \
	    out=`$(BUILD)/cookie_jar_negctl 2>&1`; \
	    n=`printf '%s\n' "$$out" | grep -c '^FAIL' || true`; \
	    if [ "$$n" -eq "$$want" ]; then \
	        printf '  %-28s OK   fails %s: %s\n' "$$d" "$$n" \
	            "`printf '%s\n' "$$out" | grep '^FAIL' | head -1`"; \
	    else \
	        printf '  %-28s BAD  expected exactly %s failure(s), got %s\n' "$$d" "$$want" "$$n"; \
	        printf '%s\n' "$$out" | grep '^FAIL' | sed 's/^/       /'; rc=1; \
	    fi; \
	 done; \
	 if [ $$rc -ne 0 ]; then echo "test-cookie-jar-negctl: FAIL"; exit 1; fi; \
	 echo "test-cookie-jar-negctl: OK -- four collapses, each reddening exactly its own"

# The transport door, built the way it shipped: webapi_cookie_line() calling
# cookie_header(), which is CK_REQ_SAME_SITE wired in. Every cross-site
# subresource then carries the target's session, so test_transport_samesite's
# four REFUSALS go red -- the three subresource ones and the navigation's
# "still no Strict" -- while both assertions that a cookie IS carried stay
# green, because over-permitting cannot break those. Measured, not predicted:
# the recipe prints the failures rather than counting them, so the shape of
# the redness is on screen every time it runs.
test-cookie-cors-negctl: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' \
	    -DWEBAPI_HOST -DWEBAPI_COOKIE_ALWAYS_SAME_SITE \
	    -o $(BUILD)/cookie_cors_negctl tests/unit/cookie_cors_test.c $(STREAM_TEST_SRC) \
	    $(QJS_SRC) $(RUST_LIB_HOST) -lm
	@if $(BUILD)/cookie_cors_negctl >$(BUILD)/cookie_cors_negctl.log 2>&1; then \
	    echo "test-cookie-cors-negctl: FAIL -- the control PASSED, so the"; \
	    echo "  transport door's SameSite is not carried by these assertions"; \
	    exit 1; \
	 fi
	@echo "test-cookie-cors-negctl: OK -- the shipped wiring reddens:"
	@grep '^FAIL' $(BUILD)/cookie_cors_negctl.log | sed 's/^/    /'
