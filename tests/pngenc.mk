# tests/pngenc.mk -- the PNG ENCODER (rust/src/pngenc.rs).
#
# In its own fragment for the reason tests/canvas.mk gives: several agents edit
# the top-level Makefile at once, and a fragment is the only way to add targets
# without a commit sweeping up somebody else's half-finished work.
#
# THE SHAPE OF THIS FRAGMENT IS ITS ARGUMENT: four targets, and the two
# controls are PREREQUISITES of the positives rather than names on a `ci-host:`
# line. CLAUDE.md rule 5 -- "a control that cannot be watched failing is worse
# than no control" -- plus the 61 stranded ones in
# tests/audit-stranded.baseline, whose fix is exactly the line
# `test-X: test-X-negctl`. Naming a control on `ci-host:` instead satisfies the
# audit and still runs it never, which is worse because it looks fixed.
#
#   test-pngenc          oracle 1 (round-trip through rust/src/png.rs)
#                        + oracle 2 (python3 zlib/binascii/Pillow)
#   test-pngenc-negctl   both oracles, against an encoder built with each of
#                        two deliberate defects, and the ASYMMETRY between
#                        which oracle catches which is the assertion
#
# WHY THERE ARE TWO ORACLES, IN ONE MEASURED SENTENCE. rust/src/png.rs is an
# independent, already-gated implementation of the other direction, so the
# round-trip is a real differential test -- but it does not look at chunk CRCs
# (`grep -ic crc rust/src/png.rs` is 0). A copy of the encoder's own output
# with every chunk CRC zeroed decodes as `png_decode -> 0, 37x11`, perfectly.
# So an encoder with the wrong CRC polynomial scores 12/12 on oracle 1 and is
# refused by libpng, by every browser, and by python3's binascii.crc32.
# `test-pngenc-negctl` turns that sentence into a switch you can watch.
.PHONY: test-pngenc test-pngenc-negctl

# tests/unit/rust_host_shim.c supplies img_register_anim (weakly) and
# deliberately not img_register; pngenc_test.c supplies the other half itself,
# because it decodes nothing through c/lib/image/img.c. See that shim's header
# for why the asymmetry is right rather than an oversight.
PNGENC_SRC = tests/unit/pngenc_test.c tests/unit/rust_host_shim.c
PNGENC_DIR = $(BUILD)/pngenc

test-pngenc: $(RUST_LIB_HOST) test-pngenc-negctl
	@mkdir -p $(PNGENC_DIR)
	@rm -f $(PNGENC_DIR)/*.png
	@$(CC) -O2 -w -o $(BUILD)/pngenc_test $(PNGENC_SRC) $(RUST_LIB_HOST)
	@$(BUILD)/pngenc_test $(PNGENC_DIR)
	@python3 tests/unit/pngenc_ext_test.py $(PNGENC_DIR)

# THE NEGATIVE CONTROLS, and the point is which oracle each one reddens.
#
#   pngenc-bad-crc       chunk CRCs use CRC-32C (Castagnoli) instead of PNG's
#                        CRC-32/ISO-HDLC. MUST leave oracle 1 GREEN and redden
#                        oracle 2. If oracle 1 goes red here, something now
#                        checks chunk CRCs and this fragment's whole
#                        justification for a second oracle has changed -- which
#                        is worth failing over, so it does.
#   pngenc-always-final  every stored deflate block sets BFINAL. Identical
#                        output below 65536 raw bytes and a silent TRUNCATION
#                        above it, so it MUST redden oracle 1 -- and it is the
#                        proof that the 200x100 and 256x257 cases in
#                        tests/unit/pngenc_test.c are load-bearing rather than
#                        decorative padding of a case list.
#
# Each variant builds into its OWN --target-dir. Sharing rust/target with the
# real build would leave a sabotaged liblogit_rust.a sitting where every other
# gate in the tree links from, which is a bug this fragment would MANUFACTURE
# in somebody else's target -- the "a sweep that manufactures bugs" shape.
test-pngenc-negctl:
	@if [ -z "$(RUST_BIN)" ]; then \
	    echo "test-pngenc-negctl: SKIPPED -- rustup/cargo not found (RUST_BIN is empty)."; \
	    echo "  This is a SKIP and not a pass: nothing below ran. Settle it with"; \
	    echo "  'rustup default stable', then re-run 'make test-pngenc'."; \
	    exit 1; \
	fi
	@mkdir -p $(BUILD)
	@# --- pngenc-bad-crc: oracle 1 must PASS, oracle 2 must FAIL -----------
	@cd rust && RUSTC="$(RUST_BIN)/rustc" "$(RUST_BIN)/cargo" build --release -q \
	    --features pngenc-bad-crc \
	    --target-dir ../$(BUILD)/rust-negctl-badcrc 2>../$(BUILD)/negctl-badcrc.cargo.log
	@rm -rf $(BUILD)/negctl-badcrc && mkdir -p $(BUILD)/negctl-badcrc
	@$(CC) -O2 -w -o $(BUILD)/pngenc_badcrc $(PNGENC_SRC) \
	    $(BUILD)/rust-negctl-badcrc/release/liblogit_rust.a
	@if $(BUILD)/pngenc_badcrc $(BUILD)/negctl-badcrc > $(BUILD)/negctl-badcrc.o1.log 2>&1; then \
	    echo "test-pngenc-negctl: ok (1a) -- a CRC-32C polynomial is INVISIBLE to the"; \
	    echo "  round-trip oracle, which is why the external one exists."; \
	 else \
	    echo "test-pngenc-negctl: FAILED (1a) -- the round-trip caught a wrong chunk"; \
	    echo "  CRC. Something now verifies chunk CRCs, so the argument for oracle 2"; \
	    echo "  in tests/unit/pngenc_ext_test.py's header is stale. Re-measure it:"; \
	    grep '^FAIL:' $(BUILD)/negctl-badcrc.o1.log | head -4; exit 1; \
	 fi
	@if python3 tests/unit/pngenc_ext_test.py $(BUILD)/negctl-badcrc \
	       > $(BUILD)/negctl-badcrc.o2.log 2>&1; then \
	    echo "test-pngenc-negctl: FAILED (1b) -- the EXTERNAL oracle passed a file"; \
	    echo "  whose every chunk CRC is computed with the wrong polynomial. Nothing"; \
	    echo "  in this repository is checking PNG's CRC field."; exit 1; \
	 else \
	    echo "test-pngenc-negctl: ok (1b) -- the external oracle rejects it:"; \
	    grep '^FAIL:' $(BUILD)/negctl-badcrc.o2.log | head -3; \
	 fi
	@# --- pngenc-always-final: oracle 1 must FAIL, and only on the big ones -
	@cd rust && RUSTC="$(RUST_BIN)/rustc" "$(RUST_BIN)/cargo" build --release -q \
	    --features pngenc-always-final \
	    --target-dir ../$(BUILD)/rust-negctl-final 2>../$(BUILD)/negctl-final.cargo.log
	@rm -rf $(BUILD)/negctl-final && mkdir -p $(BUILD)/negctl-final
	@$(CC) -O2 -w -o $(BUILD)/pngenc_final $(PNGENC_SRC) \
	    $(BUILD)/rust-negctl-final/release/liblogit_rust.a
	@if $(BUILD)/pngenc_final $(BUILD)/negctl-final > $(BUILD)/negctl-final.o1.log 2>&1; then \
	    echo "test-pngenc-negctl: FAILED (2) -- BFINAL on every stored block passed"; \
	    echo "  the round-trip, so no case in tests/unit/pngenc_test.c produces more"; \
	    echo "  than 65535 raw bytes and the block-splitting path is untested."; exit 1; \
	 else \
	    echo "test-pngenc-negctl: ok (2) -- BFINAL on every block reddens exactly the"; \
	    echo "  cases whose raw stream exceeds one 65535-byte block:"; \
	    grep '^FAIL:' $(BUILD)/negctl-final.o1.log | head -4; \
	 fi

# Named on the suite so it runs, with its control as a prerequisite of the
# positive above so the control runs too -- the two halves of not being in
# tests/audit-stranded.baseline.
ci-host: test-pngenc
