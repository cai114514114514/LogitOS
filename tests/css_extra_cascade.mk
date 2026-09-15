# Author extension cascade through real LibCSS matched specificity.
XCASCADE_DIR := $(BUILD)/css-extra-cascade
XCASCADE_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/css_extra_cascade_test.c
XCASCADE_DEPS = c/apps/browser/css_extra_cascade.inc c/apps/browser/css_inset_math.inc $(XCASCADE_SRC) $(HTML_PARSER_SRC) c/apps/browser/css.h $(BUILD)/libcss_host.a
.PHONY: test-css-extra-cascade test-css-extra-cascade-negctl
$(XCASCADE_DIR)/test: $(XCASCADE_DEPS)
	@mkdir -p $(XCASCADE_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(XCASCADE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(XCASCADE_DIR)/negctl: $(XCASCADE_DEPS)
	@mkdir -p $(XCASCADE_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DCSS_NEGCTL_EXTRA_CASCADE -o $@ $(XCASCADE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-css-extra-cascade-negctl: $(XCASCADE_DIR)/negctl
	@set +e; $(XCASCADE_DIR)/negctl > $(XCASCADE_DIR)/negctl.log 2>&1; rc=$$?; set -e; cat $(XCASCADE_DIR)/negctl.log; \
	 test $$rc -eq 1 && grep -q 'FAIL: ID specificity beats later class' $(XCASCADE_DIR)/negctl.log && \
	 grep -q 'FAIL: stylesheet important beats normal inline' $(XCASCADE_DIR)/negctl.log || { echo 'selector negative control did not fail as expected'; exit 1; }
test-css-extra-cascade: test-css-extra-cascade-negctl $(XCASCADE_DIR)/test
	@$(XCASCADE_DIR)/test

MEDIA_REGIONS_SRC = $(filter-out tests/unit/css_extra_cascade_test.c,$(XCASCADE_SRC)) tests/unit/css_media_regions_test.c
MEDIA_REGIONS_DEPS = $(XCASCADE_DEPS) tests/unit/css_media_regions_test.c tests/css_extra_cascade.mk
.PHONY: test-css-media-regions test-css-media-regions-negctl
$(XCASCADE_DIR)/media_regions: $(MEDIA_REGIONS_DEPS)
	@mkdir -p $(XCASCADE_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(MEDIA_REGIONS_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(XCASCADE_DIR)/media_regions_negctl: $(MEDIA_REGIONS_DEPS)
	@mkdir -p $(XCASCADE_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DCSS_MEDIA_LEGACY_REGIONS -o $@ $(MEDIA_REGIONS_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-css-media-regions-negctl: $(XCASCADE_DIR)/media_regions_negctl
	@rc=0; $< > $(XCASCADE_DIR)/media_regions_negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: inactive outer media ends at its own closing brace' $(XCASCADE_DIR)/media_regions_negctl.log && \
	 grep -F 'FAIL: media after 512 earlier groups does not become unconditional' $(XCASCADE_DIR)/media_regions_negctl.log
test-css-media-regions: test-css-media-regions-negctl test-media-region-instrument $(XCASCADE_DIR)/media_regions
	@$(XCASCADE_DIR)/media_regions
ci-host: test-css-media-regions
.PHONY: test-css-media-regions-asan
test-css-media-regions-asan: test-css-media-regions-negctl
	@mkdir -p $(XCASCADE_DIR)
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(BTEST_INC) $(CSS_INC) -o $(XCASCADE_DIR)/media_regions_asan $(MEDIA_REGIONS_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@ASAN_OPTIONS=detect_leaks=0 $(XCASCADE_DIR)/media_regions_asan

.PHONY: test-media-region-instrument
test-media-region-instrument:
	@python3 tests/unit/media_region_instrument_test.py

MEDIA_RANGE_SRC = $(filter-out tests/unit/css_extra_cascade_test.c,$(XCASCADE_SRC)) tests/unit/media_range_test.c
MEDIA_RANGE_PARSER = third_party/css/libcss/src/parse/mq.c
MEDIA_RANGE_DEPS = $(XCASCADE_DEPS) tests/unit/media_range_test.c tests/unit/intrinsic_test.c tests/css_extra_cascade.mk $(MEDIA_RANGE_PARSER)
$(XCASCADE_DIR)/media_range: $(MEDIA_RANGE_DEPS)
	@mkdir -p $(XCASCADE_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(MEDIA_RANGE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(XCASCADE_DIR)/media_range_negctl: $(MEDIA_RANGE_DEPS)
	@mkdir -p $(XCASCADE_DIR)
	@$(CC) -O2 -w -D_ALIGNED= -DWITHOUT_ICONV_FILTER -DCSS_MQ_RANGE_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(MEDIA_RANGE_SRC) $(HTML_PARSER_SRC) $(MEDIA_RANGE_PARSER) $(BUILD)/libcss_host.a -lm
# A wrong IDENT value makes every name-first query false, masking the second
# defect at equality boundaries. This second control keeps the correct value
# but restores only logical-complement inversion; both controls must go red.
$(XCASCADE_DIR)/media_range_complement: $(MEDIA_RANGE_DEPS)
	@mkdir -p $(XCASCADE_DIR)
	@$(CC) -O2 -w -D_ALIGNED= -DWITHOUT_ICONV_FILTER -DCSS_MQ_RANGE_COMPLEMENT $(BTEST_INC) $(CSS_INC) -o $@ $(MEDIA_RANGE_SRC) $(HTML_PARSER_SRC) $(MEDIA_RANGE_PARSER) $(BUILD)/libcss_host.a -lm
.PHONY: test-media-range test-media-range-negctl test-media-range-complement-negctl test-media-range-asan
test-media-range-negctl: $(XCASCADE_DIR)/media_range_negctl
	@rc=0; $< > $(XCASCADE_DIR)/media_range_negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL range (width>=1126px)' $(XCASCADE_DIR)/media_range_negctl.log && \
	 grep -F 'FAIL: range selects actual desktop display declaration' $(XCASCADE_DIR)/media_range_negctl.log
test-media-range-complement-negctl: $(XCASCADE_DIR)/media_range_complement
	@rc=0; $< > $(XCASCADE_DIR)/media_range_complement.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL range (width>=1126px)' $(XCASCADE_DIR)/media_range_complement.log && \
	 grep -F 'FAIL range (width<1126px)' $(XCASCADE_DIR)/media_range_complement.log
test-media-range: test-media-range-negctl test-media-range-complement-negctl $(XCASCADE_DIR)/media_range
	@$(XCASCADE_DIR)/media_range
test-media-range-asan: test-media-range-negctl test-media-range-complement-negctl
	@mkdir -p $(XCASCADE_DIR)
	@$(CC) -O1 -g -w -D_ALIGNED= -DWITHOUT_ICONV_FILTER -fsanitize=address,undefined -fno-omit-frame-pointer $(BTEST_INC) $(CSS_INC) -o $(XCASCADE_DIR)/media_range_asan $(MEDIA_RANGE_SRC) $(HTML_PARSER_SRC) $(MEDIA_RANGE_PARSER) $(BUILD)/libcss_host.a -lm
	@ASAN_OPTIONS=detect_leaks=0 $(XCASCADE_DIR)/media_range_asan
ci-host: test-media-range
