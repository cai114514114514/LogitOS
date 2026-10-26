# tests/fb-clip-stack.mk -- fb_clip_push/pop/depth (fb-clip-stack).
#
# Host test linking fb.c + glass.c + $(GFX_SRC) as test-fb-clip does, built with
# -DGFX_NCTX=2 (fb.c then defines gfx_ctx_hook; the test stubs
# sched_current_thread()). Every check judges painted pixels.
#
# THE CONTROL: -DFB_CLIP_STACK_NEGCTL_DISPLAY_ONLY makes push write only the
# bound display's target. It must exit 1 and fail ONLY 'FAIL rebind' (the clip
# is lost when fb_sdk() rebinds from the surface).

FCS_SRC := tests/unit/fb_clip_stack_test.c c/kernel/gui/fb/fb.c \
           c/kernel/gui/fb/glass.c
FCS_FLAGS := -O2 -Wall -Wextra -DGFX_NCTX=2

.PHONY: test-fb-clip-stack test-fb-clip-stack-negctl

test-fb-clip-stack-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(FCS_FLAGS) -DFB_CLIP_STACK_NEGCTL_DISPLAY_ONLY -o $(BUILD)/fb_clip_stack_negctl $(FCS_SRC) $(GFX_SRC) $(HOST_INCDIRS) -lm
	@$(BUILD)/fb_clip_stack_negctl > $(BUILD)/fb_clip_stack_negctl.log 2>&1; rc=$$?; \
	if [ $$rc -eq 0 ]; then \
		cat $(BUILD)/fb_clip_stack_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: a display-only clip passes the rebind check"; exit 1; \
	elif [ $$rc -ne 1 ]; then \
		cat $(BUILD)/fb_clip_stack_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: exit $$rc (crash, not a verdict)"; exit 1; \
	elif ! grep -q '^FAIL rebind' $(BUILD)/fb_clip_stack_negctl.log; then \
		cat $(BUILD)/fb_clip_stack_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL rebind'"; exit 1; \
	elif grep '^FAIL' $(BUILD)/fb_clip_stack_negctl.log | grep -v '^FAIL rebind' | grep -q .; then \
		cat $(BUILD)/fb_clip_stack_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: a check other than rebind failed"; exit 1; \
	else \
		echo "negative control ok: $$(grep '^FAIL rebind' $(BUILD)/fb_clip_stack_negctl.log | head -1)"; \
	fi

test-fb-clip-stack: test-fb-clip-stack-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(FCS_FLAGS) -o $(BUILD)/fb_clip_stack_test $(FCS_SRC) $(GFX_SRC) $(HOST_INCDIRS) -lm
	@$(BUILD)/fb_clip_stack_test

ci-host: test-fb-clip-stack
