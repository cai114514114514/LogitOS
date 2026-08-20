# Audio test targets (M29).
#
# Kept in its own fragment for the same reason tests/nic.mk is: several agents
# are editing the Makefile at once, and a shared-file edit cannot be committed
# without swallowing theirs. The Makefile pulls this in with one line:
#
#     -include tests/audio.mk
#
# The DRIVERS need no Makefile change at all -- C_SRC globs c/kernel and
# c/drivers, so c/kernel/audio/*.c and c/drivers/audio/*.c link by existing.
# The only thing this fragment adds to the build proper is /bin/sndtest, and it
# adds it self-containedly (see the CLI block below) rather than by editing the
# shared CLI list.

.PHONY: test-audio test-audio-pcm test-audio-pcm-negctl \
        test-audio-wav test-audio-mix test-audio-underrun test-audio-none \
        test-hda-capture-graph test-hda-capture-graph-negctl

# --- /bin/sndtest: the ring-3 instrument -------------------------------------
# `CLI +=` is deferred, and the $(DISK) recipe expands $(CLI) when it RUNS, so
# the program lands under /bin without touching the shared CLI line. CLI_AEX was
# already expanded with :=, so the .aex is added as an explicit prerequisite of
# the disk image instead.
#
# Guarded by $(wildcard) rather than assumed, and that guard has already earned
# itself: a concurrent commit rebuilt the tree from a stale index and dropped
# sndtest.c, at which point this rule made `make build/disk.img` fail for all
# twenty lines with "No rule to make target". A TEST fragment must not be able
# to break the build of the thing it tests. Missing source -> no /bin/sndtest,
# a warning saying so, and everything else still builds.
SNDTEST_SRC := $(wildcard $(CLIDIR)/sndtest.c)
ifneq ($(SNDTEST_SRC),)
CLI += sndtest
$(eval $(call CLI_RULE,sndtest))
$(DISK): $(BUILD)/sndtest.aex
else
$(warning tests/audio.mk: $(CLIDIR)/sndtest.c is missing -- /bin/sndtest will not be built, and the on-device audio targets cannot run)
endif

# --- /bin/rec: the capture-side instrument, same self-contained wiring ------
# Captures from c/drivers/audio/hda.c's input path to a WAV file via
# SYS_SND_CAP_*; see the file's own header for what it checks and writes.
# Same guard-by-$(wildcard) posture as sndtest above and for the identical
# reason -- this fragment must not be able to break `make build/disk.img`
# for anyone else if rec.c is ever missing from their checkout.
REC_SRC := $(wildcard $(CLIDIR)/rec.c)
ifneq ($(REC_SRC),)
CLI += rec
$(eval $(call CLI_RULE,rec))
$(DISK): $(BUILD)/rec.aex
else
$(warning tests/audio.mk: $(CLIDIR)/rec.c is missing -- /bin/rec will not be built, and capture cannot be exercised on device)
endif

# --- host: the PCM layer ------------------------------------------------------
# Compiles the REAL c/kernel/audio/pcm.c -- a test of a copy proves things about
# the copy. Covers conversion, the resampler's cross-period seam, the mix clamp,
# u8 silence, and the ring's wrap.
test-audio-pcm:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -Wall -Wextra -o $(BUILD)/audio_pcm_test \
	    tests/unit/audio_pcm_test.c c/kernel/audio/pcm.c \
	    -Ic/kernel/audio -Iinclude/abi -lm
	@./$(BUILD)/audio_pcm_test

# NEGATIVE CONTROL for the host suite. Replaces the saturating clamp with a bare
# int16 truncation -- the single most audible mixer bug there is, and one that
# no "did it play" check would ever catch. REQUIRED TO FAIL.
test-audio-pcm-negctl:
	@mkdir -p $(BUILD)/audioneg
	@sed -e 's|if (v > 32767)  return 32767;|;|' -e 's|if (v < -32768) return -32768;|;|' c/kernel/audio/pcm.c > $(BUILD)/audioneg/pcm.c
	@if cmp -s c/kernel/audio/pcm.c $(BUILD)/audioneg/pcm.c; then \
	    echo "FAIL: the sabotage matched nothing -- clamp16 was reworded, fix this target"; exit 1; fi
	@cp c/kernel/audio/snd.h $(BUILD)/audioneg/
	@$(CC) -O2 -w -o $(BUILD)/audio_pcm_negctl tests/unit/audio_pcm_test.c \
	    $(BUILD)/audioneg/pcm.c -I$(BUILD)/audioneg -Iinclude/abi -lm
	@if ./$(BUILD)/audio_pcm_negctl >$(BUILD)/audioneg/out.txt 2>&1; then \
	    echo "FAIL: with the mix clamp removed the suite still PASSED --"; \
	    echo "      the clamp assertions cannot fail, so they prove nothing."; \
	    exit 1; \
	 else \
	    echo "PASS (negative control): clamp removed -> the suite failed, as required"; \
	    grep 'FAIL' $(BUILD)/audioneg/out.txt | sed 's/^/       /'; \
	 fi

# --- on device: the captured WAV ---------------------------------------------
# QEMU's `wav` audiodev writes what the guest actually played to a file, which
# turns "did it play" into an artefact that can be checked sample by sample.
# QEMU_AUDIO is a variable rather than an edit to the shared QEMU_* lines so
# other agents' tests are unaffected; the harness composes it per run.
QEMU_AUDIO ?= -device intel-hda -device hda-output,audiodev=snd0

test-audio-wav: $(ISO) $(DISK)
	@bash tests/boot/run-audio-wav-test.sh $(ISO) $(DISK) ramp

test-audio-mix: $(ISO) $(DISK)
	@bash tests/boot/run-audio-wav-test.sh $(ISO) $(DISK) mix

test-audio-underrun: $(ISO) $(DISK)
	@bash tests/boot/run-audio-wav-test.sh $(ISO) $(DISK) underrun

# A machine with no sound card must boot and degrade to silence, not hang or
# fault. This is the case real hardware hits first.
test-audio-none: $(ISO) $(DISK)
	@bash tests/boot/run-audio-none-test.sh $(ISO) $(DISK)

# --- host: the CAPTURE widget-graph search -----------------------------------
# See tests/unit/hda_capture_graph_test.c's own header before reading this as
# "hda.c was tested" -- it is a PORT of try_capture_path()'s algorithm against
# a modelled widget graph, not the compiled driver, because that function
# talks to real MMIO (the CORB/RIRB command bus) with no host build of its
# own the way c/kernel/audio/pcm.c has. This is the fallback the capture work
# was scoped to use when a real device-side capture signal is not available in
# this environment (see the report: QEMU here has no audio backend that can
# feed the guest real input, "wav" being record/output-only).
test-hda-capture-graph:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -Wall -Wextra -o $(BUILD)/hda_capture_graph_test \
	    tests/unit/hda_capture_graph_test.c
	@./$(BUILD)/hda_capture_graph_test

# NEGATIVE CONTROL, watched to fail exactly twice: reinstates the "guess the
# first ADC and the first pin" fallback find_output_path uses on the OUTPUT
# side, which the file's own header names as the specific wrong
# implementation this gate exists to catch on the capture side. Reddens only
# the no-edge case's two assertions (24 checks total, 22 survive) -- every
# other case still finds its path by the SAME graph edge the fallback would
# have skipped past, so a fallback that only ever fires when the real search
# already failed cannot be seen by any check that passes on its own.
test-hda-capture-graph-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DHDA_CAP_NEGCTL_GUESS -o $(BUILD)/hda_capture_graph_negctl \
	    tests/unit/hda_capture_graph_test.c
	@if ./$(BUILD)/hda_capture_graph_negctl >$(BUILD)/hda_capture_graph_negctl.out 2>&1; then \
	    echo "FAIL: with the guess-fallback reinstated the suite still PASSED -- it cannot see the bug"; \
	    exit 1; \
	 else \
	    n=$$(grep -c '^FAIL' $(BUILD)/hda_capture_graph_negctl.out); \
	    if [ "$$n" != "2" ]; then \
	        echo "FAIL: expected exactly 2 reddened checks, got $$n"; exit 1; \
	    fi; \
	    echo "PASS (negative control): guess-fallback reinstated -> exactly 2 checks failed, as required"; \
	    grep '^FAIL' $(BUILD)/hda_capture_graph_negctl.out | sed 's/^/       /'; \
	 fi

test-audio: test-audio-pcm test-audio-pcm-negctl test-audio-wav test-audio-mix \
            test-audio-underrun test-audio-none \
            test-hda-capture-graph test-hda-capture-graph-negctl
