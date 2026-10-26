# tests/gfx-path-closed-bit.mk -- gfx_close() records closure in the sub[] high
# bit (GFX_SUB_CLOSED); the stroker joins the seam of a closed subpath instead
# of capping it, and a dash that spans the seam is one run.
#
#   make test-gfx-path-closed-bit          the suite (negctl first)
#   make test-gfx-path-closed-bit-negctl   the SAME sources with
#                                          -DGFX_STROKE_IGNORE_CLOSED_BIT: the
#                                          stroker ignores the bit, a gfx_close()d
#                                          rect strokes open again, and the
#                                          rect-join and dash-seam checks must fail
#                                          (and the fill/path_ok checks must not).

.PHONY: test-gfx-path-closed-bit test-gfx-path-closed-bit-negctl

GPCB_CF := -O1 -g -Wall -Wextra $(GFX_INC) -pthread
GPCB_SRCS := tests/unit/gfx_closed_test.c $(GFX_SRC)

$(BUILD)/gfx_closed_test: $(GPCB_SRCS) c/lib/gfx/include/gfx.h c/lib/gfx/include/gfx_path.h
	@mkdir -p $(BUILD)
	$(CC) $(GPCB_CF) -o $@ $(GPCB_SRCS) -lm

test-gfx-path-closed-bit-negctl:
	@mkdir -p $(BUILD)
	$(CC) $(GPCB_CF) -DGFX_STROKE_IGNORE_CLOSED_BIT -o $(BUILD)/gfx_closed_negctl $(GPCB_SRCS) -lm
	@if $(BUILD)/gfx_closed_negctl > $(BUILD)/gfx_closed_negctl.log 2>&1; then \
	    cat $(BUILD)/gfx_closed_negctl.log; \
	    echo "NEGATIVE CONTROL FAILED: the suite passes with the closed bit ignored"; exit 1; \
	elif ! grep -q '^FAIL rect-join' $(BUILD)/gfx_closed_negctl.log || \
	     ! grep -q '^FAIL dash-seam' $(BUILD)/gfx_closed_negctl.log; then \
	    cat $(BUILD)/gfx_closed_negctl.log; \
	    echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no FAIL rect-join / FAIL dash-seam"; exit 1; \
	elif grep -q '^FAIL \(fill-crc\|path-ok\|bit-set\)' $(BUILD)/gfx_closed_negctl.log; then \
	    cat $(BUILD)/gfx_closed_negctl.log; \
	    echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: a non-stroker check broke"; exit 1; \
	else \
	    echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/gfx_closed_negctl.log) check(s) fail with the bit ignored ($$(grep -m1 '^FAIL rect-join' $(BUILD)/gfx_closed_negctl.log))"; \
	fi

test-gfx-path-closed-bit: test-gfx-path-closed-bit-negctl $(BUILD)/gfx_closed_test
	$(BUILD)/gfx_closed_test

ci-host: test-gfx-path-closed-bit
