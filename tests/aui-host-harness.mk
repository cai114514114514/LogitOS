# Host AUI harness (aui-host-harness): ring-3 aui.c over a mock window-manager
# transport, rendered to PPM and diffed against a golden store.
#
#   tests/unit/aui_host/aui_host.{h,c}     the transport (LOGIT_SYS_HOOK=aui_host_sys)
#   tests/unit/aui_host/aui_host_scenes.c  S1 controls, S2 rows, S3 glass, S4 sheet
#   tests/unit/aui_host/aui_host_test.c    renders 4 scenes x 4 scales x 2 modes
#   tests/unit/aui_host/aui_host_scale_check.c  aui_host_pt == fb_pt, against the real fb.c
#   tests/unit/aui_host/v2_<name>.c        v2 handlers added by adoption items; picked
#                                          up by the wildcard below, never by editing this
#   tests/golden/aui/                      <scene>@<pct>[.v2].ppm.gz; see its README
#   tools/ppm_diff.py                      the comparison
#
# test-aui-host-negctl builds the SAME sources with -DAUI_HOST_NEGCTL_SHIFT (every
# RECT/RRECT lands 1 device px right) and runs the SAME ppm_diff flags; every
# golden comparison must then say "FAIL golden", and a size/format error (exit 2)
# or an unhandled syscall does not count as that.

AUIH_DIR      := tests/unit/aui_host
AUIH_OUT      := $(BUILD)/aui_host
AUIH_GOLD     := tests/golden/aui
AUIH_SRC      := $(AUIH_DIR)/aui_host.c $(AUIH_DIR)/aui_host_scenes.c $(AUIH_DIR)/aui_host_test.c c/apps/gui/aui.c
AUIH_V2_SRC   := $(sort $(wildcard $(AUIH_DIR)/v2_*.c))
AUIH_SCALE_SRC := $(AUIH_DIR)/aui_host_scale_check.c $(AUIH_DIR)/aui_host.c c/kernel/gui/fb/fb.c c/kernel/gui/fb/glass.c
AUIH_CFLAGS   := -O1 -g -Wall -Wextra -DLOGIT_SYS_HOOK=aui_host_sys -I$(AUIH_DIR) -Ic/apps/gui -Ic/apps \
                 $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)

.PHONY: test-aui-host test-aui-host-negctl

test-aui-host-negctl:
	@if [ "$(GOLDEN_UPDATE)" = 1 ]; then echo "negative control skipped: GOLDEN_UPDATE=1 is rewriting the goldens"; exit 0; fi; \
	  rm -rf $(AUIH_OUT)/neg && mkdir -p $(AUIH_OUT)/neg/render; \
	  $(CC) $(AUIH_CFLAGS) -DAUI_HOST_NEGCTL_SHIFT -o $(AUIH_OUT)/neg/aui_host_test $(AUIH_SRC) $(AUIH_V2_SRC) $(AUI_TL_SRC) $(GFX_SRC) -lm || exit 1; \
	  $(CC) -O2 -Wall -Wextra -DAUI_HOST_NEGCTL_SHIFT $(HOST_INCDIRS) -o $(AUIH_OUT)/neg/scale_check $(AUIH_SCALE_SRC) $(GFX_SRC) -lm || exit 1; \
	  python3 tools/ppm_diff.py --selftest > /dev/null || exit 1; \
	  $(AUIH_OUT)/neg/aui_host_test $(AUIH_OUT)/neg/render > $(AUIH_OUT)/neg/render.log 2>&1 \
	    || { cat $(AUIH_OUT)/neg/render.log; echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: the harness itself failed (crash or unhandled syscall)"; exit 1; }; \
	  n=0; bad=0; wrong=0; : > $(AUIH_OUT)/neg/diff.log; \
	  for f in $(AUIH_OUT)/neg/render/*.ppm; do b=$$(basename $$f); n=$$((n+1)); \
	    python3 tools/ppm_diff.py $$f $(AUIH_GOLD)/$$b.gz --tol 0 --max-bad 0 --diff-out $(AUIH_OUT)/neg/diff_$$b >> $(AUIH_OUT)/neg/diff.log; rc=$$?; \
	    if [ $$rc = 1 ]; then bad=$$((bad+1)); else wrong=$$((wrong+1)); fi; \
	  done; \
	  if [ $$n = 0 ]; then echo "NEGATIVE CONTROL FAILED: nothing was rendered"; exit 1; fi; \
	  if [ $$wrong != 0 ]; then cat $(AUIH_OUT)/neg/diff.log; echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: $$wrong comparison(s) passed or hit a size/format error"; exit 1; fi; \
	  if [ "$$(grep -c '^FAIL golden' $(AUIH_OUT)/neg/diff.log)" != "$$n" ]; then cat $(AUIH_OUT)/neg/diff.log; echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: not every comparison said FAIL golden"; exit 1; fi; \
	  echo "negative control ok: $$bad/$$n renders fail the golden diff with the 1 px shift ($$(grep -m1 '^FAIL golden' $(AUIH_OUT)/neg/diff.log | cut -c1-90))"

test-aui-host: test-aui-host-negctl
	@mkdir -p $(AUIH_OUT)
	@$(CC) $(AUIH_CFLAGS) -o $(AUIH_OUT)/aui_host_test $(AUIH_SRC) $(AUIH_V2_SRC) $(AUI_TL_SRC) $(GFX_SRC) -lm
	@$(CC) -O2 -Wall -Wextra $(HOST_INCDIRS) -o $(AUIH_OUT)/scale_check $(AUIH_SCALE_SRC) $(GFX_SRC) -lm
	@$(AUIH_OUT)/scale_check
	@python3 tools/ppm_diff.py --selftest
	@rm -rf $(AUIH_OUT)/render && mkdir -p $(AUIH_OUT)/render
	@$(AUIH_OUT)/aui_host_test $(AUIH_OUT)/render
	@if [ "$(GOLDEN_UPDATE)" = 1 ]; then \
	  mkdir -p $(AUIH_GOLD); changed=0; \
	  for f in $(AUIH_OUT)/render/*.ppm; do b=$$(basename $$f); \
	    python3 tools/ppm_diff.py gzip $$f $(AUIH_OUT)/render/$$b.gz; \
	    if ! cmp -s $(AUIH_OUT)/render/$$b.gz $(AUIH_GOLD)/$$b.gz; then \
	      echo "golden changed: $$b.gz"; cp $(AUIH_OUT)/render/$$b.gz $(AUIH_GOLD)/$$b.gz; changed=$$((changed+1)); fi; \
	  done; \
	  for g in $(AUIH_GOLD)/*.ppm.gz; do b=$$(basename $$g .gz); \
	    [ -f $(AUIH_OUT)/render/$$b ] || { echo "golden removed (no longer rendered): $$b.gz"; rm -f $$g; changed=$$((changed+1)); }; \
	  done; \
	  echo "GOLDEN_UPDATE: $$changed golden(s) changed"; \
	else \
	  fail=0; n=0; \
	  for f in $(AUIH_OUT)/render/*.ppm; do b=$$(basename $$f); n=$$((n+1)); \
	    python3 tools/ppm_diff.py $$f $(AUIH_GOLD)/$$b.gz --tol 0 --max-bad 0 --diff-out $(AUIH_OUT)/diff_$$b || fail=1; \
	  done; \
	  ng=$$(ls $(AUIH_GOLD)/*.ppm.gz 2>/dev/null | wc -l | tr -d ' '); \
	  if [ "$$ng" != "$$n" ]; then echo "FAIL golden store: $$n scenes rendered but $$ng goldens committed"; fail=1; fi; \
	  [ $$fail = 0 ] && echo "ok   aui-host: $$n renders match their goldens (tol 0)"; \
	  exit $$fail; \
	fi

ci-host: test-aui-host
