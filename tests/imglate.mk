# tests/imglate.mk -- images that arrive after the first layout.
#
#   make test-img-late          the gate: host unit test, seconds
#   make test-img-late-negctl   the three sabotages, each of which the suite
#                               MUST fail against
#
# WHY A FRAGMENT AND NOT LINES IN THE MAKEFILE: the reason every other
# tests/*.mk gives -- several agents share this tree and a whole-file Makefile
# write from a concurrent line has silently deleted other people's targets more
# than once. Add
#
#     -include tests/imglate.mk
#
# beside the other fragment includes when picking this up.
#
# WHAT IT COVERS, and what it does not. This is layout.c's half of the fix: the
# decoded-image cache, so a bitmap survives a re-layout and is fetched once; the
# per-call budget, which bounds NEW network work only so the pass can run every
# frame; negative entries, so a broken URL is not re-requested forever; and the
# two bounds, which refuse out loud. browser.c's half -- settle_frame() running
# the pass once a frame and load_late_images() batching the fetch before any
# decode -- needs the network and the event loop and is proven on the device
# with `make scoreboard-1 SITE=bilibili`.
#
# THE BUILD USES A DELIBERATELY TINY IMGCACHE_BYTES (1 MiB against the shipped
# 64 MiB) so the byte bound can be crossed by two 512x512 images instead of by
# 64 MiB of test fixture. The bound is the same code either way; only the number
# is smaller.

.PHONY: test-img-late test-img-late-negctl

IMGL_DIR := $(BUILD)/imglate
IMGL_SRC := tests/unit/img_late_test.c c/apps/browser/layout.c \
            c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
            c/apps/browser/css_extra.c
IMGL_DEF := -DIMGCACHE_BYTES=1048576L

$(IMGL_DIR)/img_late_test: $(IMGL_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a \
                           c/apps/browser/layout.h
	@mkdir -p $(IMGL_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) $(IMGL_DEF) -o $@ $(IMGL_SRC) \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm

test-img-late: $(IMGL_DIR)/img_late_test
	@$(IMGL_DIR)/img_late_test

# Each sabotage is a real previous behaviour or the plausible wrong version of
# the fix, never a `return 0`.
#
#   IMG_NEGCTL_NOCACHE  nothing is remembered. THE BEHAVIOUR BEING REPLACED:
#                       layout_free() frees every bitmap at the top of every
#                       layout_page(), so a re-layout loses the page's pictures
#                       and the next pass refetches from zero. Note what still
#                       passes against it -- the first load, the budget, the
#                       counts -- which is exactly why a suite that only checks
#                       "an image is present after loading" measures nothing.
#   IMG_NEGCTL_NOBOUND  the byte bound never refuses, so the cache grows without
#                       limit. Everything still draws; what is gone is the
#                       refusal, and a bound nobody can observe is not a bound.
#   IMG_NEGCTL_NONEG    no negative entries: a URL that will not fetch or will
#                       not decode is requested again by every pass. Since the
#                       pass now runs once a frame, that is a request storm, and
#                       it is invisible to any assertion about pixels.
IMGL_NEGS := IMG_NEGCTL_NOCACHE IMG_NEGCTL_NOBOUND IMG_NEGCTL_NONEG

test-img-late-negctl: $(BUILD)/libcss_host.a
	@mkdir -p $(IMGL_DIR)
	@bad=0; \
	 for n in $(IMGL_NEGS); do \
	   $(CC) -O2 -w $(BTEST_INC) $(CSS_INC) $(IMGL_DEF) -D$$n \
	       -o $(IMGL_DIR)/negctl_$$n $(IMGL_SRC) \
	       $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm || exit 1; \
	   if $(IMGL_DIR)/negctl_$$n > $(IMGL_DIR)/negctl_$$n.log 2>&1; then \
	     echo "test-img-late-negctl: FAILED -- the suite PASSED with $$n"; \
	     echo "  sabotaged, so nothing in it is measuring that."; bad=1; \
	   else \
	     echo "test-img-late-negctl: ok -- $$n sabotaged, suite fails:"; \
	     grep -m2 'FAIL' $(IMGL_DIR)/negctl_$$n.log | sed 's/^/    /'; \
	   fi; \
	 done; \
	 exit $$bad
