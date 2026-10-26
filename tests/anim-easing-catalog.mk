# tests/anim-easing-catalog.mk -- ol_ease256_ex: integer easing catalog with
# bezier/steps/back/expo/quart/cubic, one per-context unknown-curve counter.
# The control restores the old "bezier and back fall back to linear" behaviour.

AEC_SRC := tests/unit/openlogit_ease_test.c c/lib/gfx/animation/openlogit_anim.c c/lib/gfx/geometry/gfx_math.c
AEC_DEPS := $(AEC_SRC) c/lib/gfx/include/openlogit_anim.h c/lib/gfx/include/openlogit_easing.h
AEC_FLAGS := -O2 -Wall -Wextra -DGFX_NCTX=2 $(GFX_INC)

.PHONY: test-anim-easing-catalog test-anim-easing-catalog-negctl

$(BUILD)/anim_easing_catalog: $(AEC_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(AEC_FLAGS) $(AEC_SRC) -lm -o $@
$(BUILD)/anim_easing_catalog_neg: $(AEC_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(AEC_FLAGS) -DOPENLOGIT_EASE256_LINEAR_FALLBACK $(AEC_SRC) -lm -o $@

test-anim-easing-catalog-negctl: $(BUILD)/anim_easing_catalog_neg
	@if $(BUILD)/anim_easing_catalog_neg > $(BUILD)/anim-easing-catalog-neg.log 2>&1; then \
		cat $(BUILD)/anim-easing-catalog-neg.log; \
		echo "NEGATIVE CONTROL FAILED: the suite passes with linear bezier/back"; exit 1; \
	elif grep -q '^FAIL bezier' $(BUILD)/anim-easing-catalog-neg.log && grep -q '^FAIL back' $(BUILD)/anim-easing-catalog-neg.log; then \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/anim-easing-catalog-neg.log) checks fail (FAIL bezier, FAIL back)"; \
	else \
		cat $(BUILD)/anim-easing-catalog-neg.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON"; exit 1; \
	fi

test-anim-easing-catalog: test-anim-easing-catalog-negctl $(BUILD)/anim_easing_catalog
	$(BUILD)/anim_easing_catalog

ci-host: test-anim-easing-catalog
