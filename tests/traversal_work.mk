# Native operation counts settle the scaling claim; guest profiling settles
# elapsed speed. Derive the real runtime composition from the selector gate.
TRAVERSAL_WORK_SRC = $(filter-out tests/unit/selectors_test.c,$(SELECTORS_SRC)) tests/unit/traversal_work_test.c
TRAVERSAL_WORK_DEP = $(TRAVERSAL_WORK_SRC) tests/unit/dom_iface_test.c $(HTML_PARSER_SRC) $(QJS_SRC) $(wildcard c/apps/browser/js_*.inc) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
TRAVERSAL_WORK_CF = -O2 -w $(SELECTORS_CF) -DJSDOM_TRAVERSAL_PROFILE
$(BUILD)/wiring/traversal_work_test: $(TRAVERSAL_WORK_DEP)
	@mkdir -p $(BUILD)/wiring
	$(CC) $(TRAVERSAL_WORK_CF) -o $@ $(TRAVERSAL_WORK_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-traversal-work test-traversal-work-negctl
test-traversal-work-negctl: $(TRAVERSAL_WORK_DEP)
	@mkdir -p $(BUILD)/wiring
	@set -e; for defect in JSDOM_FRAGMENT_RESCAN_PARENT SELECT_COLLECTION_WALK; do \
	  bin=$(BUILD)/wiring/traversal_work_$$defect; \
	  $(CC) $(TRAVERSAL_WORK_CF) -D$$defect -o $$bin $(TRAVERSAL_WORK_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm; \
	  if $$bin > $$bin.log 2>&1; then echo "FAIL: $$defect control stayed green"; exit 1; fi; \
	  case $$defect in JSDOM_FRAGMENT_RESCAN_PARENT) expected='fragment named-access work is bounded by inserted nodes';; \
	    SELECT_COLLECTION_WALK) expected='query traversal avoids a child collection per visited element';; esac; \
	  grep "^FAIL $$expected" $$bin.log; \
	done
test-traversal-work: test-traversal-work-negctl $(BUILD)/wiring/traversal_work_test
	$(BUILD)/wiring/traversal_work_test
