# VP8 INTER-frame (video) decoder -- rust/src/vp8_inter.rs, feature
# "vp8-interframe" (default OFF; see rust/Cargo.toml's own comment on why: a
# video decoder does not belong in ring 0, so the kernel's $(RUST_LIB) build
# never turns this feature on and never sees vp8_inter.rs or its tables).
#
# THIS WAS THE MISSING GATE. vp8_inter.rs's own module doc comment names
# "tests/vp8.mk" as where the vp8_video_had_frac_mv negative-control split
# lives; before this file existed, that comment cited a gate that was never
# written -- worse than no comment, because it reads as evidence of coverage
# that does not exist. See rust/src/vp8_inter.rs and
# tests/unit/vp8_video_gen.py's own module docs for what this corpus targets:
# plain P-frames first (MV decode + motion compensation with no reference
# bookkeeping beyond ordinary refresh_last), then a stream with a REAL
# invisible (shown=0) alt-ref frame, because golden/altref handling is where
# inter decoders go wrong first, and a corpus with no hidden frame cannot
# find that class of bug at all.
#
# The oracle is ffmpeg's own vp8 decoder on the identical IVF bytes, and the
# bar is BYTE EQUALITY, matching the discipline test-webp-vp8 already holds
# the key-frame decoder to -- VP8 reconstruction is exactly specified integer
# arithmetic, so there is no tolerance to hide a misread rule in.
#
# Needs ffmpeg (built with libvpx). Without it there is no reference and the
# gate would be checking the decoder against itself, so it refuses to run
# rather than passing vacuously (see vp8_video_gen.py).
#
# Builds the interframe feature into ITS OWN cargo target-dir
# ($(BUILD)/rust_vp8inter), never $(RUST_LIB_HOST) -- so a feature-gated
# build used only by this gate can never be mistaken for, or clobber, the
# default (feature-OFF) host library every other Rust-backed test links,
# including under a concurrent `make`.
VP8_INTER_LIB := $(BUILD)/rust_vp8inter/release/liblogit_rust.a
$(VP8_INTER_LIB): $(RUST_SRC)
	@if [ -z "$(RUST_BIN)" ]; then \
	    echo "error: rustup/cargo not found (RUST_BIN is empty)."; \
	    echo "       install rustup (https://rustup.rs), then: rustup target add x86_64-unknown-none"; \
	    exit 1; \
	fi
	cd rust && RUSTC="$(RUST_BIN)/rustc" "$(RUST_BIN)/cargo" build --release \
	    --features vp8-interframe --target-dir ../$(BUILD)/rust_vp8inter

test-vp8-video: $(VP8_INTER_LIB)
	@mkdir -p $(BUILD)/vp8video
	@python3 tests/unit/vp8_video_gen.py $(BUILD)/vp8video
	@$(CC) -O2 -Wall -Wextra -w -o $(BUILD)/vp8_video_test tests/unit/vp8_video_test.c \
	    $(IMG_HOST_SRC) $(VP8_INTER_LIB) $(IMG_HOST_INC) -lm
	@$(BUILD)/vp8_video_test $(BUILD)/vp8video

# Negative control: the sub-pixel interpolator's vertical 6-tap pass runs
# BEFORE the horizontal one (vp8-sixtap-swap, rust/Cargo.toml) -- VP8's
# spec order is horizontal-then-vertical and the two are observably
# different because the intermediate result is rounded/clamped to u8 between
# passes. Inert on any case with no fractional-pel motion vector, which is
# why test-vp8-video's own "frac_mv=1" report matters: a corpus that never
# exercises sub-pixel MC would make this control pass for the wrong reason
# (nothing for the swap to break), not because the swap is harmless.
#
# It reddens 2 of the 4 cases, NAMED rather than left as a bare count:
# motion_p (28/30 frames) and altref_hidden (71/72 frames) fail, because both
# have genuine two-dimensional sub-pixel motion, where swapping pass order
# changes the intermediate rounding on both axes. basic_p and odd_dims stay
# GREEN under this control -- not because the control is weak, but because
# their motion in this corpus happens to be 1-D-dominant sub-pixel (offset on
# only one axis at a time), so there is nothing for a pass-ORDER swap to
# disturb: a single 1-D filter pass gives the same result run before or after
# an identity pass on the other axis. Two cases named and explained is
# stronger evidence than "N of 4 failed" would be on its own -- it is what
# lets a reader confirm the control is discriminating on the real property
# (2-D sub-pixel motion) rather than passing or failing by chance.
test-vp8-video-negctl: $(VP8_INTER_LIB)
	@mkdir -p $(BUILD)/vp8video
	@python3 tests/unit/vp8_video_gen.py $(BUILD)/vp8video >/dev/null
	@(cd rust && "$(RUST_BIN)/cargo" build --release --features vp8-interframe,vp8-sixtap-swap \
	    --target-dir ../$(BUILD)/rust_vp8interctl >/dev/null 2>&1) || exit 1
	@$(CC) -O2 -w -o $(BUILD)/vp8_video_ctl tests/unit/vp8_video_test.c \
	    $(IMG_HOST_SRC) $(BUILD)/rust_vp8interctl/release/liblogit_rust.a $(IMG_HOST_INC) -lm
	@if $(BUILD)/vp8_video_ctl $(BUILD)/vp8video >$(BUILD)/vp8_video_negctl.log 2>&1; then \
	    echo "FAIL: --features vp8-sixtap-swap still passes -- the control proves nothing"; \
	    exit 1; \
	 fi
	@echo "ok: --features vp8-sixtap-swap -> $$(tail -1 $(BUILD)/vp8_video_negctl.log)"
