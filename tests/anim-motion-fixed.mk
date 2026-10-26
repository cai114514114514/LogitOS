# tests/anim-motion-fixed.mk -- integer spring / premultiplied mix / rect24 lerp.
# The control builds the SAME sources and flags plus -DOL_SPRING_DURATION_CUTOFF
# (stop at a fixed 260 ms); the soft spring settles much later, so the snap
# check must fail with 'FAIL snap'. It is a prerequisite of the positive target.

AMF_CF  := -O2 -Wall -Wextra $(GFX_INC)
AMF_SRC := tests/unit/openlogit_motion_test.c c/lib/gfx/animation/openlogit_motion.c

.PHONY: test-anim-motion-fixed test-anim-motion-fixed-negctl

test-anim-motion-fixed-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(AMF_CF) -DOL_SPRING_DURATION_CUTOFF -o $(BUILD)/anim_motion_negctl $(AMF_SRC) -lm
	@if $(BUILD)/anim_motion_negctl > $(BUILD)/anim_motion_negctl.log 2>&1; then \
		cat $(BUILD)/anim_motion_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: a fixed 260 ms cutoff passes the suite"; exit 1; \
	elif ! grep -q '^FAIL snap' $(BUILD)/anim_motion_negctl.log; then \
		cat $(BUILD)/anim_motion_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL snap'"; exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL snap' $(BUILD)/anim_motion_negctl.log) snap check(s) fail under the cutoff"; \
	fi

test-anim-motion-fixed: test-anim-motion-fixed-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(AMF_CF) -o $(BUILD)/anim_motion_test $(AMF_SRC) -lm
	@$(BUILD)/anim_motion_test

ci-host: test-anim-motion-fixed
