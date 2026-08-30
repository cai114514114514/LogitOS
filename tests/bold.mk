# bold gates -- owned by the bold wave-1 agent (2026-08-30). Filled in by its owner.
#
# WHAT LANDED AND WHAT THIS FILE CLOSES. The implementation itself is commit
# 2ec6f9873 (2026-08-28): struct logit_run grew a `bold` field, the kernel's
# face_font()/tl_fonts() select fsroot/fonts/{ui,mono}-bold.ttf for a
# LOGIT_FACE_BOLD run, and browser_paint.c passes css_engine's o->bold
# through gui_text_run_w. What never existed until this file is the pair of
# gates below:
#
#   test-bold-metrics (ci-host)  the vertical-metrics identity between each
#       Regular face and its Bold twin. fsroot/fonts/README.md states the
#       rule ("bold ascent == regular ascent ... nothing else in the tree
#       asserts it"), and the one check that LOOKED like it
#       (tests/unit/font_weight_test.c's "line height is the SAME for both
#       weights") routes through text_line_height(), which only ever reads
#       the REGULAR faces -- it compares four identical numbers and cannot
#       fail. Watched red: a one-unit ascent bump on a COPY of ui-bold.ttf
#       fails the gate naming the field (test-bold-metrics-negctl, a
#       prerequisite of this one).
#
#   test-bold-page (ci-boot)     bold actually RENDERS bold, measured off
#       the screendump: per-way-of-saying-bold (<b>, <strong>,
#       font-weight:700) the run's painted advance and ink density must
#       exceed the regular twin's; bold mono must keep the mono advance; the
#       bold run's ink must sit at the same depth as the regular's. Watched
#       red: the same driver against a kernel built with the bold faces
#       suppressed (test-bold-page-negctl, a prerequisite of this one, per
#       the house rule that a control named on a ci- line runs never while
#       looking fixed).
#
# COST NOTE, honestly: the guest control builds a second kernel and boots a
# second QEMU, so ci-boot pays two boots for this file. That is the price of
# the control being a prerequisite rather than stranded debt (see
# tools/audit_tests.py STRANDED CONTROLS); if it ever needs to move to a
# slower tier, move BOTH halves of the pair, never the positive alone.
#
# ITALIC STAYS REFUSED, and not for lack of a gate: neither vendored source
# carries an ital or slnt axis, so no italic face is derivable, and shearing
# the regular outlines would have to happen inside the glyph rasteriser
# whose correctness test-glyph-agree scores against an independent oracle --
# the argument is in tools/mkfont.py's docstring and is not repeated here.

# CFLAGS is simply-expanded but recipes expand when they RUN, so appending
# from an -include'd fragment still reaches every compile (the same two-token
# footprint tests/clip.mk keeps in the shared Makefile).
ifeq ($(BOLDNOFACE),1)
CFLAGS += -DLOGIT_BOLD_NO_FACE
endif

.PHONY: test-bold-metrics test-bold-metrics-negctl \
        test-bold-page test-bold-page-negctl

# --- host: the metrics identity, on the font bytes --------------------------
test-bold-metrics: test-bold-metrics-negctl
test-bold-metrics: $(FONTS) tests/fixtures/bold/check_font_metrics.py
	python3 tests/fixtures/bold/check_font_metrics.py fsroot/fonts

# The control works on COPIES under $(BUILD); the assets in fsroot/fonts are
# never touched. One unit is the smallest drift a regeneration could really
# introduce, and the check must fail on it AND name the field.
test-bold-metrics-negctl: $(FONTS) tests/fixtures/bold/check_font_metrics.py
	@rm -rf $(BUILD)/boldmctl
	@mkdir -p $(BUILD)/boldmctl/fonts
	@cp $(FONTS) $(BUILD)/boldmctl/fonts/
	@python3 tests/fixtures/bold/check_font_metrics.py \
	    --corrupt-ascent $(BUILD)/boldmctl/fonts/ui-bold.ttf
	@out=`python3 tests/fixtures/bold/check_font_metrics.py $(BUILD)/boldmctl/fonts 2>&1`; \
	 ec=$$?; \
	 if [ $$ec -eq 0 ]; then \
	    echo "CONTROL FAILED: a drifted bold ascent passed the metrics gate"; \
	    echo "$$out"; exit 1; fi; \
	 echo "$$out" | grep -q 'ascent differs between the twins' || { \
	    echo "CONTROL FAILED: exited $$ec but not on the ascent check"; \
	    echo "$$out"; exit 1; }; \
	 echo "control ok: one unit of ascent drift on the bold twin reddens the gate"

ci-host: test-bold-metrics

# --- guest: bold on the glass ------------------------------------------------
test-bold-page: test-bold-page-negctl $(ISO) $(DISK)
	python3 tests/qmp/qmp_bold_page.py $(ISO) $(DISK)

# Same disk as the positive -- the sabotage is kernel-only (the ifdef in
# c/kernel/gui/text.c suppresses the two bold-face loads), and the browser
# binary on the disk is byte-identical in both worlds, so building a second
# disk image would buy nothing. With the bold faces suppressed a bold
# request degrades to exactly the regular font set, which is byte-for-byte
# the pre-bold machine, so every differs-assertion in the driver must fail.
test-bold-page-negctl: $(ISO) $(DISK)
	$(MAKE) BUILD=$(BUILD)/negbold BOLDNOFACE=1 $(BUILD)/negbold/logit.iso
	@mkdir -p $(BUILD)/negbold
	@echo "--- negative control: the same gate against a kernel with no bold faces ---"
	@if python3 tests/qmp/qmp_bold_page.py $(BUILD)/negbold/logit.iso $(DISK) \
	        > $(BUILD)/negbold/ctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: bold rendered as regular and the gate did not notice"; \
	    tail -20 $(BUILD)/negbold/ctl.log; exit 1; \
	else \
	    grep -q '^plain abc' $(BUILD)/negbold/ctl.log || { \
	        echo "NEGATIVE CONTROL FAILED: no measurement line -- the page did not"; \
	        echo "render, which is a plumbing failure, not the control"; \
	        tail -20 $(BUILD)/negbold/ctl.log; exit 1; }; \
	    grep -q 'FAIL: <b>abc</b> paints at least 1px wider' \
	        $(BUILD)/negbold/ctl.log || { \
	        echo "NEGATIVE CONTROL FAILED: the driver exited nonzero but not on the"; \
	        echo "advance differs-assertion -- a plumbing error also exits 1 and would"; \
	        echo "read as a passing control"; \
	        tail -20 $(BUILD)/negbold/ctl.log; exit 1; }; \
	    echo "control ok: with the bold faces suppressed bold renders as regular"; \
	    echo "byte-for-byte, and the differs-assertions go red:"; \
	    grep -E '^(plain abc|FAIL)' $(BUILD)/negbold/ctl.log | head -4; \
	fi

ci-boot: test-bold-page
