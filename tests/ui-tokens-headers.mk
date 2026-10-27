# tests/ui-tokens-headers.mk -- the freestanding design headers in
# c/include/ui (design_language.md section 3, 5.2, 5.7, appendix A; P3):
# logit_tokens.h, logit_elevation.h, logit_motion.h, logit_apps.h, logit_fmt.h.
#
# Nothing in the build includes them yet (W0), so kernel.elf and every .aex
# stay bit-identical; these gates prove the headers themselves:
#   test-ui-tokens-syntax  each header, alone and all together, compiles with
#                          -fsyntax-only -Werror under the KERNEL flags
#                          (CFLAGS) and the RING-3 flags (UCFLAGS);
#   test-ui-tokens         ui_tokens_test.c: WCAG contrast of every ratio the
#                          design claims, the "≈" composites, derive() per
#                          swatch, materials / elevation / motion mirrors,
#                          app lookup, launch args, the title rule;
#   test-ui-fmt            ui_fmt_test.c: inputs to exact UTF-8 strings.
# Each control uses the same sources and flags as its positive and differs
# only by its -D; it must exit 1 with its own FAIL line and no other.

.PHONY: test-ui-tokens test-ui-tokens-negctl test-ui-tokens-syntax test-ui-fmt test-ui-fmt-negctl

UITOK_HDRS := c/include/ui/logit_tokens.h c/include/ui/logit_elevation.h \
              c/include/ui/logit_motion.h c/include/ui/logit_apps.h c/include/ui/logit_fmt.h
# -Ic/include locally (ui-prewire adds it globally in the same wave); the
# gfx include dirs only so the test can assert the openlogit_* mirrors.
UITOK_HOST_FLAGS = -O2 -Wall -Wextra -Werror -std=c11 -Ic/include $(GFX_INC)
UITOK_SRC := tests/unit/ui_tokens_test.c
UIFMT_SRC := tests/unit/ui_fmt_test.c
UITOK_DIR = $(BUILD)/ui-tokens-headers

# Kernel and ring-3 flags as the real TUs get them, minus dependency output.
UITOK_KFLAGS = $(filter-out -MMD -MP -g,$(CFLAGS)) -Ic/include -Werror
UITOK_UFLAGS = $(filter-out -MMD -MP -g,$(UCFLAGS)) -Ic/include -Werror

test-ui-tokens-syntax: $(UITOK_HDRS)
	@mkdir -p $(UITOK_DIR)
	@set -e; for h in $(UITOK_HDRS) all; do \
		tu=$(UITOK_DIR)/syntax_$$(basename $$h .h).c; \
		if [ $$h = all ]; then \
			for x in $(UITOK_HDRS); do echo "#include \"ui/$$(basename $$x)\""; done > $$tu; \
		else echo "#include \"ui/$$(basename $$h)\"" > $$tu; fi; \
		$(CC) $(UITOK_KFLAGS) -fsyntax-only $$tu || { echo "FAIL syntax kernel $$h"; exit 1; }; \
		$(CC) $(UITOK_UFLAGS) -fsyntax-only $$tu || { echo "FAIL syntax ring3 $$h"; exit 1; }; \
	done; \
	echo "ui-tokens-headers: $(words $(UITOK_HDRS)) headers + all-in-one compile -Werror under CFLAGS and UCFLAGS"

$(UITOK_DIR)/ui_tokens_test: $(UITOK_SRC) $(UITOK_HDRS)
	@mkdir -p $(dir $@)
	@$(CC) $(UITOK_HOST_FLAGS) $(UITOK_SRC) -lm -o $@

$(UITOK_DIR)/ui_tokens_negctl: $(UITOK_SRC) $(UITOK_HDRS)
	@mkdir -p $(dir $@)
	@$(CC) $(UITOK_HOST_FLAGS) -DUI_TOKENS_NEGCTL $(UITOK_SRC) -lm -o $@

test-ui-tokens-negctl: $(UITOK_DIR)/ui_tokens_negctl
	@rc=0; $(UITOK_DIR)/ui_tokens_negctl > $(UITOK_DIR)/ui_tokens_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(UITOK_DIR)/ui_tokens_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL contrast label_tertiary' $(UITOK_DIR)/ui_tokens_negctl.log || { cat $(UITOK_DIR)/ui_tokens_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL contrast label_tertiary' line"; exit 1; }; \
	if grep '^FAIL' $(UITOK_DIR)/ui_tokens_negctl.log | grep -qv '^FAIL contrast label_tertiary'; then \
		grep '^FAIL' $(UITOK_DIR)/ui_tokens_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: label.tertiary L #BDBDC6: $$(grep -m1 '^FAIL contrast label_tertiary' $(UITOK_DIR)/ui_tokens_negctl.log)"

test-ui-tokens: test-ui-tokens-negctl test-ui-tokens-syntax $(UITOK_DIR)/ui_tokens_test
	@$(UITOK_DIR)/ui_tokens_test

$(UITOK_DIR)/ui_fmt_test: $(UIFMT_SRC) $(UITOK_HDRS)
	@mkdir -p $(dir $@)
	@$(CC) $(UITOK_HOST_FLAGS) $(UIFMT_SRC) -o $@

$(UITOK_DIR)/ui_fmt_negctl: $(UIFMT_SRC) $(UITOK_HDRS)
	@mkdir -p $(dir $@)
	@$(CC) $(UITOK_HOST_FLAGS) -DUI_FMT_NEGCTL $(UIFMT_SRC) -o $@

test-ui-fmt-negctl: $(UITOK_DIR)/ui_fmt_negctl
	@rc=0; $(UITOK_DIR)/ui_fmt_negctl > $(UITOK_DIR)/ui_fmt_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(UITOK_DIR)/ui_fmt_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL fmt-size' $(UITOK_DIR)/ui_fmt_negctl.log || { cat $(UITOK_DIR)/ui_fmt_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL fmt-size' line"; exit 1; }; \
	if grep '^FAIL' $(UITOK_DIR)/ui_fmt_negctl.log | grep -qv '^FAIL fmt-size'; then \
		grep '^FAIL' $(UITOK_DIR)/ui_fmt_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: no space before KB: $$(grep -c '^FAIL fmt-size' $(UITOK_DIR)/ui_fmt_negctl.log) size cases fail, first: $$(grep -m1 '^FAIL fmt-size' $(UITOK_DIR)/ui_fmt_negctl.log)"

test-ui-fmt: test-ui-fmt-negctl $(UITOK_DIR)/ui_fmt_test
	@$(UITOK_DIR)/ui_fmt_test

ci-host: test-ui-tokens test-ui-fmt
