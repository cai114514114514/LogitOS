# Keep output private while sharing only the established host CSS archive.
ABSOLUTE_AUTO_HEIGHT_DIR := $(BUILD)/site-general/layout/absolute-auto-height
ABSOLUTE_AUTO_HEIGHT_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/absolute_auto_height_test.c
ABSOLUTE_AUTO_HEIGHT_DEPS = $(ABSOLUTE_AUTO_HEIGHT_SRC) tests/unit/intrinsic_test.c $(HTML_PARSER_SRC) c/apps/browser/layout.h c/apps/browser/css.h c/apps/browser/layout_flex.c c/apps/browser/layout_grid.c $(BUILD)/libcss_host.a tests/absolute_auto_height.mk
.PHONY: test-absolute-auto-height test-absolute-auto-height-negctl
$(ABSOLUTE_AUTO_HEIGHT_DIR)/test: $(ABSOLUTE_AUTO_HEIGHT_DEPS)
	@mkdir -p $(ABSOLUTE_AUTO_HEIGHT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(ABSOLUTE_AUTO_HEIGHT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(ABSOLUTE_AUTO_HEIGHT_DIR)/negctl: $(ABSOLUTE_AUTO_HEIGHT_DEPS)
	@mkdir -p $(ABSOLUTE_AUTO_HEIGHT_DIR)
	@$(CC) -O2 -w -DLAYOUT_ABS_AUTO_HEIGHT_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(ABSOLUTE_AUTO_HEIGHT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-absolute-auto-height-negctl: $(ABSOLUTE_AUTO_HEIGHT_DIR)/negctl
	@$(ABSOLUTE_AUTO_HEIGHT_DIR)/negctl > $(ABSOLUTE_AUTO_HEIGHT_DIR)/negctl.log 2>&1; rc=$$?; cat $(ABSOLUTE_AUTO_HEIGHT_DIR)/negctl.log; test $$rc -eq 1 && grep -q 'FAIL: absolute percent uses auto containing block used height' $(ABSOLUTE_AUTO_HEIGHT_DIR)/negctl.log
$(ABSOLUTE_AUTO_HEIGHT_DIR)/whitespace-negctl: $(ABSOLUTE_AUTO_HEIGHT_DEPS)
	@mkdir -p $(ABSOLUTE_AUTO_HEIGHT_DIR)
	@$(CC) -O2 -w -DLAYOUT_FLEX_RUN_SWALLOW_OOF $(BTEST_INC) $(CSS_INC) -o $@ $(ABSOLUTE_AUTO_HEIGHT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-absolute-image-whitespace-negctl test-absolute-auto-height-asan
test-absolute-image-whitespace-negctl: $(ABSOLUTE_AUTO_HEIGHT_DIR)/whitespace-negctl
	@rc=0; $< > $(ABSOLUTE_AUTO_HEIGHT_DIR)/whitespace-negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: whitespace cannot consume absolute flex image layer' $(ABSOLUTE_AUTO_HEIGHT_DIR)/whitespace-negctl.log
test-absolute-auto-height: test-absolute-auto-height-negctl test-absolute-image-whitespace-negctl $(ABSOLUTE_AUTO_HEIGHT_DIR)/test
	@$(ABSOLUTE_AUTO_HEIGHT_DIR)/test
test-absolute-auto-height-asan: test-absolute-auto-height-negctl test-absolute-image-whitespace-negctl
	@mkdir -p $(ABSOLUTE_AUTO_HEIGHT_DIR)
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(BTEST_INC) $(CSS_INC) -o $(ABSOLUTE_AUTO_HEIGHT_DIR)/asan $(ABSOLUTE_AUTO_HEIGHT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@ASAN_OPTIONS=detect_leaks=0 $(ABSOLUTE_AUTO_HEIGHT_DIR)/asan

IMAGE_INTRINSIC_SRC = $(filter-out tests/unit/absolute_auto_height_test.c,$(ABSOLUTE_AUTO_HEIGHT_SRC)) tests/unit/image_intrinsic_test.c
IMAGE_INTRINSIC_DEPS = $(ABSOLUTE_AUTO_HEIGHT_DEPS) tests/unit/image_intrinsic_test.c
$(ABSOLUTE_AUTO_HEIGHT_DIR)/image-intrinsic: $(IMAGE_INTRINSIC_DEPS)
	@mkdir -p $(ABSOLUTE_AUTO_HEIGHT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(IMAGE_INTRINSIC_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(ABSOLUTE_AUTO_HEIGHT_DIR)/image-intrinsic-negctl: $(IMAGE_INTRINSIC_DEPS)
	@mkdir -p $(ABSOLUTE_AUTO_HEIGHT_DIR)
	@$(CC) -O2 -w -DLAYOUT_IMAGE_INTRINSIC_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(IMAGE_INTRINSIC_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(ABSOLUTE_AUTO_HEIGHT_DIR)/image-self-negctl: $(IMAGE_INTRINSIC_DEPS)
	@mkdir -p $(ABSOLUTE_AUTO_HEIGHT_DIR)
	@$(CC) -O2 -w -DLAYOUT_IMAGE_NO_SELF $(BTEST_INC) $(CSS_INC) -o $@ $(IMAGE_INTRINSIC_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-image-intrinsic test-image-intrinsic-negctl test-image-self-negctl test-image-intrinsic-asan
test-image-intrinsic-negctl: $(ABSOLUTE_AUTO_HEIGHT_DIR)/image-intrinsic-negctl
	@rc=0; $< > $(ABSOLUTE_AUTO_HEIGHT_DIR)/image-intrinsic-negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: flex picture max-content comes from decoded ratio not 24px' $(ABSOLUTE_AUTO_HEIGHT_DIR)/image-intrinsic-negctl.log && \
	 grep -F 'FAIL: following title uses decoded image height before placement' $(ABSOLUTE_AUTO_HEIGHT_DIR)/image-intrinsic-negctl.log
test-image-self-negctl: $(ABSOLUTE_AUTO_HEIGHT_DIR)/image-self-negctl
	@rc=0; $< > $(ABSOLUTE_AUTO_HEIGHT_DIR)/image-self-negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: exactly one image item for replaced element' $(ABSOLUTE_AUTO_HEIGHT_DIR)/image-self-negctl.log
test-image-intrinsic: test-image-intrinsic-negctl test-image-self-negctl $(ABSOLUTE_AUTO_HEIGHT_DIR)/image-intrinsic
	@$(ABSOLUTE_AUTO_HEIGHT_DIR)/image-intrinsic
test-image-intrinsic-asan: test-image-intrinsic-negctl test-image-self-negctl
	@mkdir -p $(ABSOLUTE_AUTO_HEIGHT_DIR)
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(BTEST_INC) $(CSS_INC) -o $(ABSOLUTE_AUTO_HEIGHT_DIR)/image-intrinsic-asan $(IMAGE_INTRINSIC_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@ASAN_OPTIONS=detect_leaks=0 $(ABSOLUTE_AUTO_HEIGHT_DIR)/image-intrinsic-asan
ci-host: test-image-intrinsic

# Guest integration control: keep decoded sizing but remove its frame/CSSOM
# invalidation consumer. Track the switch as an object prerequisite so returning
# to an ordinary disk build cannot silently retain the disabled consumer.
.PHONY: image-geometry-flags-force
$(BUILD)/image-geometry.flags: image-geometry-flags-force
	@mkdir -p $(dir $@)
	@echo '$(if $(filter 1,$(BROWSER_IMAGE_GEOMETRY_NEGCTL)),off,on)' > $@.tmp
	@cmp -s $@.tmp $@ || cp $@.tmp $@
	@rm -f $@.tmp
$(BUILD)/jsobj/c/apps/browser/browser.o: $(BUILD)/image-geometry.flags
$(BUILD)/jsobj/c/apps/browser/browser.o: JS_CF += $(if $(filter 1,$(BROWSER_IMAGE_GEOMETRY_NEGCTL)),-DBROWSER_IMAGE_GEOMETRY_NO_FLUSH,)

# Native integration needs immutable positive/control disks supplied explicitly;
# never boot the mutable disk while another build replaces it. The negative is
# a prerequisite, not just a ci label: pixels resizing while the following title
# stays at its old y must be watched failing before the positive can run. These
# are guest correctness checks, not a live-site or host wall-clock benchmark.
IMAGE_INTRINSIC_GUEST_ISO ?=
IMAGE_INTRINSIC_GUEST_DISK ?=
IMAGE_INTRINSIC_GUEST_NEG_DISK ?=
.PHONY: test-image-intrinsic-guest test-image-intrinsic-guest-negctl
test-image-intrinsic-guest-negctl:
	@test -f "$(IMAGE_INTRINSIC_GUEST_ISO)" && test -f "$(IMAGE_INTRINSIC_GUEST_NEG_DISK)" || { echo 'ERROR: set IMAGE_INTRINSIC_GUEST_ISO and immutable IMAGE_INTRINSIC_GUEST_NEG_DISK built with BROWSER_IMAGE_GEOMETRY_NEGCTL=1'; exit 1; }
	@mkdir -p $(ABSOLUTE_AUTO_HEIGHT_DIR)
	@rc=0; python3 tools/perf/browser_load.py --iso "$(IMAGE_INTRINSIC_GUEST_ISO)" --disk "$(IMAGE_INTRINSIC_GUEST_NEG_DISK)" --out $(ABSOLUTE_AUTO_HEIGHT_DIR)/guest-negctl --rounds 1 --cases image-intrinsic-passive > $(ABSOLUTE_AUTO_HEIGHT_DIR)/guest-negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F "AssertionError: ('late dimensions did not reflow passive page'" $(ABSOLUTE_AUTO_HEIGHT_DIR)/guest-negctl.log
test-image-intrinsic-guest: test-image-intrinsic-guest-negctl
	@test -f "$(IMAGE_INTRINSIC_GUEST_DISK)" || { echo 'ERROR: set immutable production IMAGE_INTRINSIC_GUEST_DISK'; exit 1; }
	python3 tools/perf/browser_load.py --iso "$(IMAGE_INTRINSIC_GUEST_ISO)" --disk "$(IMAGE_INTRINSIC_GUEST_DISK)" --out $(ABSOLUTE_AUTO_HEIGHT_DIR)/guest --rounds 1 --cases image-intrinsic,image-intrinsic-passive
