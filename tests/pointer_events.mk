# Share the actual transparent-box consumer pipeline; focus remains independent.
POINTER_EVENTS_DIR := $(BUILD)/site-general/layout/pointer-events
POINTER_EVENTS_SRC = $(filter-out tests/unit/transparent_box_hit_test.c,$(TRANSPARENT_BOX_HIT_SRC)) tests/unit/pointer_events_test.c c/apps/browser/focus.c
POINTER_EVENTS_DEPS = $(POINTER_EVENTS_SRC) tests/unit/transparent_box_hit_test.c tests/unit/cssom_test.c c/apps/browser/css.h c/apps/browser/element_scroll_wiring.inc c/apps/browser/text_paint_wiring.inc $(QJS_SRC) $(BUILD)/libcss_host.a
$(POINTER_EVENTS_DIR)/test: $(POINTER_EVENTS_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(ELEMENT_SCROLL_FLAGS) -o $@ $(POINTER_EVENTS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
$(POINTER_EVENTS_DIR)/legacy: $(POINTER_EVENTS_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(ELEMENT_SCROLL_FLAGS) -DCSS_POINTER_HIT_LEGACY -o $@ $(POINTER_EVENTS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
$(POINTER_EVENTS_DIR)/all-legacy: $(POINTER_EVENTS_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(ELEMENT_SCROLL_FLAGS) -DCSS_POINTER_ALL_HIT_LEGACY -o $@ $(POINTER_EVENTS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-pointer-events-all-negctl test-pointer-events-sanitize
test-pointer-events-all-negctl: $(POINTER_EVENTS_DIR)/all-legacy
	@rc=0; $< > $(POINTER_EVENTS_DIR)/all-legacy.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -F 'FAIL all dialog escapes none wrapper native target' $(POINTER_EVENTS_DIR)/all-legacy.log && grep -F 'FAIL all dialog escapes none wrapper CSSOM target' $(POINTER_EVENTS_DIR)/all-legacy.log
.PHONY: test-pointer-events test-pointer-events-negctl
test-pointer-events-negctl: $(POINTER_EVENTS_DIR)/legacy
	@rc=0; $< > $(POINTER_EVENTS_DIR)/legacy.log 2>&1 || rc=$$?; cat $(POINTER_EVENTS_DIR)/legacy.log; test $$rc -eq 1 && grep -q 'FAIL none overlay passes through blank region native target' $(POINTER_EVENTS_DIR)/legacy.log && grep -q 'FAIL inherited none excludes descendant text CSSOM target' $(POINTER_EVENTS_DIR)/legacy.log
test-pointer-events: test-pointer-events-negctl test-pointer-events-all-negctl $(POINTER_EVENTS_DIR)/test
	@$(POINTER_EVENTS_DIR)/test
test-pointer-events-sanitize: test-pointer-events
	@$(CC) $(ELEMENT_SCROLL_FLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -o $(POINTER_EVENTS_DIR)/sanitize $(POINTER_EVENTS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
	@ASAN_OPTIONS=detect_leaks=0 $(POINTER_EVENTS_DIR)/sanitize

# Immutable pre-fix/current disks are explicit inputs. Never turn a missing
# artifact, failed boot, or a page that never ran into an expected red control.
POINTER_ALL_GUEST_ISO ?=
POINTER_ALL_GUEST_DISK ?=
POINTER_ALL_GUEST_NEG_DISK ?=
.PHONY: test-pointer-all-modal-guest test-pointer-all-modal-guest-negctl
test-pointer-all-modal-guest-negctl:
	@test -f "$(POINTER_ALL_GUEST_ISO)" && test -f "$(POINTER_ALL_GUEST_NEG_DISK)" || { echo 'ERROR: set POINTER_ALL_GUEST_ISO and immutable POINTER_ALL_GUEST_NEG_DISK'; exit 1; }
	@mkdir -p $(POINTER_EVENTS_DIR)
	@rc=0; python3 tools/perf/browser_load.py --iso "$(POINTER_ALL_GUEST_ISO)" --disk "$(POINTER_ALL_GUEST_NEG_DISK)" --out $(POINTER_EVENTS_DIR)/modal-guest-negctl --rounds 1 --cases pointer-all-modal > $(POINTER_EVENTS_DIR)/modal-guest-negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'guest marker missing: MODAL-ALL ACTION action=1' $(POINTER_EVENTS_DIR)/modal-guest-negctl.log && \
	 grep -F 'MODAL-ALL SHADE action=0 close=0 reopen=0 outside=2' $(POINTER_EVENTS_DIR)/modal-guest-negctl/serial.txt
test-pointer-all-modal-guest: test-pointer-all-modal-guest-negctl test-pointer-events
	@test -f "$(POINTER_ALL_GUEST_DISK)" || { echo 'ERROR: set immutable POINTER_ALL_GUEST_DISK'; exit 1; }
	python3 tools/perf/browser_load.py --iso "$(POINTER_ALL_GUEST_ISO)" --disk "$(POINTER_ALL_GUEST_DISK)" --out $(POINTER_EVENTS_DIR)/modal-guest --rounds 1 --cases pointer-all-modal
