# tests/ol-status-unify.mk -- status-returning variants: gfx_path_status()
# (the overflow latch now records its cause), ol_raster_fill_st() and the
# ol_bitmap_*_st() operations. Each refusal cause has its own code; the legacy
# return values are unchanged.
#
#   make test-ol-status-unify          the suite (negctl first)
#   make test-ol-status-unify-negctl   the SAME sources with -DGFX_STATUS_COLLAPSE:
#                                      every limit refusal reports OL_ARGUMENT and
#                                      the matrix cause reports the overflow code,
#                                      so the 'distinct' checks must fail, and
#                                      nothing else may.

.PHONY: test-ol-status-unify test-ol-status-unify-negctl

OLS_SRC := tests/unit/gfx_status_test.c $(GFX_SRC)
OLS_FLAGS := -O1 -g -Wall -Wextra -DGFX_NCTX=2 $(GFX_INC)
OLS_DEPS := $(OLS_SRC) $(GFX_HEADERS)

$(BUILD)/gfx_status_test: $(OLS_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(OLS_FLAGS) $(OLS_SRC) -lm -o $@

$(BUILD)/gfx_status_negctl: $(OLS_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(OLS_FLAGS) -DGFX_STATUS_COLLAPSE $(OLS_SRC) -lm -o $@

test-ol-status-unify-negctl: $(BUILD)/gfx_status_negctl
	@rc=0; $(BUILD)/gfx_status_negctl > $(BUILD)/gfx_status_negctl.log 2>&1 || rc=$$?; \
	test $$rc -eq 1 || { echo "negctl exited $$rc, expected 1"; cat $(BUILD)/gfx_status_negctl.log; exit 1; }
	@grep -q '^FAIL distinct' $(BUILD)/gfx_status_negctl.log || { echo "negctl did not fail with 'FAIL distinct'"; cat $(BUILD)/gfx_status_negctl.log; exit 1; }
	@test -z "$$(grep '^FAIL ' $(BUILD)/gfx_status_negctl.log | grep -v '^FAIL distinct')" || { echo "negctl failed for another reason"; cat $(BUILD)/gfx_status_negctl.log; exit 1; }
	@echo "ol-status-unify control: collapsed build failed $$(grep -c '^FAIL distinct' $(BUILD)/gfx_status_negctl.log) distinct check(s), no other failures."

test-ol-status-unify: test-ol-status-unify-negctl $(BUILD)/gfx_status_test
	$(BUILD)/gfx_status_test

ci-host: test-ol-status-unify
