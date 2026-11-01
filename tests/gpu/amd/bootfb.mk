# AMD/ATI vendor-wide passive boot framebuffer support.  The host fixture
# proves the full-LFB-within-one-BAR ownership rule and zero takeover side
# effects.  The QEMU guest uses its real ati-vga PCI identity; this exercises
# firmware display handoff and device-model binding, not a physical AMD GPU.
# atom.c is linked only for polaris_atom_result_name(): bootfb.c prints that
# name in the [amd-resources] line. The probe itself stays a stub in the test.
# The VBIOS source and status names bootfb.c prints are static inline in
# resource/vbios.h, so they need no object file.
# crtc.c, arena.c and layout.c are the stage B2 display survey (HWHUD builds).
AMD_BOOTFB_SRC := tests/gpu/amd/bootfb_test.c c/drivers/gpu/amd/bootfb.c \
                  c/drivers/gpu/amd/polaris/resource/atom.c \
                  c/drivers/gpu/amd/polaris/display/crtc.c \
                  c/drivers/gpu/amd/polaris/memory/arena.c \
                  c/drivers/gpu/amd/polaris/memory/layout.c
AMD_BOOTFB_INC := -Ic/drivers/gpu -Ic/drivers/core -Ic/kernel/pci \
                  $(KGUI_INC) $(KCORE_INC) $(KMM_INC) -Ic/lib/gfx/include
# Stage B2's display survey is compiled only into HWHUD=1 kernels (LOGIT_HWHUD),
# so every gate builds the glue both ways. The check counts differ by variant:
# the plain build asserts that nothing ran, the HWHUD build asserts what ran.
AMD_BOOTFB_HWHUD := -DLOGIT_HWHUD
AMD_BOOTFB_CHECKS_PLAIN := 70
AMD_BOOTFB_CHECKS_HWHUD := 72
AMD_BOOTFB_EFI := $(BUILD)/amd-bootfb-efi/BOOTX64.EFI
AMD_BOOTFB_ESP := $(BUILD)/amd-bootfb-esp.img

.PHONY: test-amd-bootfb-negctl test-amd-bootfb-host test-amd-bootfb-guest \
        test-amd-bootfb-uefi-guest test-amd-bootfb-guest-all

# Each mutation is watched failing at exactly the assertion naming the safety
# rule it removes.  The positive gate depends on these controls, so CI cannot
# run the green binary while silently skipping the red apparatus.
test-amd-bootfb-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror -DLOGIT_HOST_TEST \
	    -DAMD_BOOTFB_NEGCTL_START_ONLY $(AMD_BOOTFB_INC) $(AMD_BOOTFB_SRC) \
	    -o $(BUILD)/amd-bootfb-neg-overlap
	@if $(BUILD)/amd-bootfb-neg-overlap >$(BUILD)/amd-bootfb-neg-overlap.log 2>&1; then \
	    cat $(BUILD)/amd-bootfb-neg-overlap.log; exit 1; fi
	@grep -q '^FAIL: partial LFB overlap is refused by full-range containment$$' \
	    $(BUILD)/amd-bootfb-neg-overlap.log
	@grep -q '^AMD_BOOTFB: $(AMD_BOOTFB_CHECKS_PLAIN) checks, 1 failures$$' \
	    $(BUILD)/amd-bootfb-neg-overlap.log
	@echo 'AMD_BOOTFB_NEGCTL: start-only ownership accepted partial overlap (exactly one failure)'
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror -DLOGIT_HOST_TEST \
	    -DAMD_BOOTFB_NEGCTL_ACCEPT_MEM_OFF $(AMD_BOOTFB_INC) $(AMD_BOOTFB_SRC) \
	    -o $(BUILD)/amd-bootfb-neg-memoff
	@if $(BUILD)/amd-bootfb-neg-memoff >$(BUILD)/amd-bootfb-neg-memoff.log 2>&1; then \
	    cat $(BUILD)/amd-bootfb-neg-memoff.log; exit 1; fi
	@grep -q '^FAIL: memory-decode-off display is refused without a config write$$' \
	    $(BUILD)/amd-bootfb-neg-memoff.log
	@grep -q '^AMD_BOOTFB: $(AMD_BOOTFB_CHECKS_PLAIN) checks, 1 failures$$' \
	    $(BUILD)/amd-bootfb-neg-memoff.log
	@echo 'AMD_BOOTFB_NEGCTL: memory-decode-off device bound (exactly one failure)'
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror -DLOGIT_HOST_TEST \
	    -DAMD_BOOTFB_NEGCTL_ACCEPT_MIXED_BAR $(AMD_BOOTFB_INC) $(AMD_BOOTFB_SRC) \
	    -o $(BUILD)/amd-bootfb-neg-mixed-bar
	@if $(BUILD)/amd-bootfb-neg-mixed-bar >$(BUILD)/amd-bootfb-neg-mixed-bar.log 2>&1; then \
	    cat $(BUILD)/amd-bootfb-neg-mixed-bar.log; exit 1; fi
	@grep -q '^FAIL: ambiguous memory-plus-I/O BAR is refused$$' \
	    $(BUILD)/amd-bootfb-neg-mixed-bar.log
	@grep -q '^AMD_BOOTFB: $(AMD_BOOTFB_CHECKS_PLAIN) checks, 1 failures$$' \
	    $(BUILD)/amd-bootfb-neg-mixed-bar.log
	@echo 'AMD_BOOTFB_NEGCTL: mixed memory/I-O BAR was accepted (exactly one failure)'
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror -DLOGIT_HOST_TEST \
	    -DAMD_BOOTFB_NEGCTL_ACCEPT_BAD_PM $(AMD_BOOTFB_INC) $(AMD_BOOTFB_SRC) \
	    -o $(BUILD)/amd-bootfb-neg-bad-pm
	@if $(BUILD)/amd-bootfb-neg-bad-pm >$(BUILD)/amd-bootfb-neg-bad-pm.log 2>&1; then \
	    cat $(BUILD)/amd-bootfb-neg-bad-pm.log; exit 1; fi
	@grep -q '^FAIL: PM capability whose PMCSR crosses config space is refused$$' \
	    $(BUILD)/amd-bootfb-neg-bad-pm.log
	@grep -q '^AMD_BOOTFB: $(AMD_BOOTFB_CHECKS_PLAIN) checks, 1 failures$$' \
	    $(BUILD)/amd-bootfb-neg-bad-pm.log
	@echo 'AMD_BOOTFB_NEGCTL: malformed PM capability was dereferenced (exactly one failure)'
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror -DLOGIT_HOST_TEST $(AMD_BOOTFB_HWHUD) \
	    -DAMD_BOOTFB_NEGCTL_SURVEY_PITCH $(AMD_BOOTFB_INC) $(AMD_BOOTFB_SRC) \
	    -o $(BUILD)/amd-bootfb-neg-survey-pitch
	@if $(BUILD)/amd-bootfb-neg-survey-pitch >$(BUILD)/amd-bootfb-neg-survey-pitch.log 2>&1; then \
	    cat $(BUILD)/amd-bootfb-neg-survey-pitch.log; exit 1; fi
	@grep -q '^FAIL: display survey line carries the GOP CRTC, pitch and BAR5 size$$' \
	    $(BUILD)/amd-bootfb-neg-survey-pitch.log
	@grep -q '^FAIL: display survey line carries the arena$$' \
	    $(BUILD)/amd-bootfb-neg-survey-pitch.log
	@grep -q '^FAIL: the survey times the frame counter in an HWHUD build$$' \
	    $(BUILD)/amd-bootfb-neg-survey-pitch.log
	@grep -q '^AMD_BOOTFB: $(AMD_BOOTFB_CHECKS_HWHUD) checks, 3 failures$$' \
	    $(BUILD)/amd-bootfb-neg-survey-pitch.log
	@echo 'AMD_BOOTFB_NEGCTL: framebuffer size used as pitch refused the GOP CRTC (exactly three failures)'
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror -DLOGIT_HOST_TEST $(AMD_BOOTFB_HWHUD) \
	    -DAMD_BOOTFB_NEGCTL_EXPECT_NO_SURVEY $(AMD_BOOTFB_INC) $(AMD_BOOTFB_SRC) \
	    -o $(BUILD)/amd-bootfb-neg-no-survey
	@if $(BUILD)/amd-bootfb-neg-no-survey >$(BUILD)/amd-bootfb-neg-no-survey.log 2>&1; then \
	    cat $(BUILD)/amd-bootfb-neg-no-survey.log; exit 1; fi
	@grep -q '^FAIL: no display survey line without a proven MC window$$' \
	    $(BUILD)/amd-bootfb-neg-no-survey.log
	@grep -q '^FAIL: no display survey line in a build without HWHUD$$' \
	    $(BUILD)/amd-bootfb-neg-no-survey.log
	@grep -q '^FAIL: a build without HWHUD neither maps BAR5 nor times the frame counter$$' \
	    $(BUILD)/amd-bootfb-neg-no-survey.log
	@grep -q '^FAIL: a build without HWHUD does not even try to map BAR5$$' \
	    $(BUILD)/amd-bootfb-neg-no-survey.log
	@grep -q '^AMD_BOOTFB: $(AMD_BOOTFB_CHECKS_PLAIN) checks, 4 failures$$' \
	    $(BUILD)/amd-bootfb-neg-no-survey.log
	@echo 'AMD_BOOTFB_NEGCTL: a survey that runs where HWHUD is off is caught by the plain-build assertions'

test-amd-bootfb-host: test-amd-bootfb-negctl
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror \
	    -DLOGIT_HOST_TEST $(AMD_BOOTFB_INC) $(AMD_BOOTFB_SRC) \
	    -o $(BUILD)/amd-bootfb-host
	@$(BUILD)/amd-bootfb-host
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror \
	    -DLOGIT_HOST_TEST $(AMD_BOOTFB_HWHUD) $(AMD_BOOTFB_INC) $(AMD_BOOTFB_SRC) \
	    -o $(BUILD)/amd-bootfb-host-hwhud
	@$(BUILD)/amd-bootfb-host-hwhud

# Keep this loader build local to the gate.  loader.c consumes the public boot
# ABI and the standalone build's documented EFI_CPPFLAGS seam is how callers
# supply that include root; sharing tests/uefi.mk's image would also let a stale
# unrelated image make this driver gate look green.
$(AMD_BOOTFB_EFI): c/boot/efi/build.sh c/boot/efi/loader.c \
                   c/boot/efi/trampoline.S include/abi/logit_boot.h
	@mkdir -p $(dir $@)
	@EFI_OUT=$@ EFI_CPPFLAGS="-I$(CURDIR)/include" bash c/boot/efi/build.sh

$(AMD_BOOTFB_ESP): $(AMD_BOOTFB_EFI) $(KERNEL) tools/mkesp.py
	@python3 tools/mkesp.py $@ --efi $(AMD_BOOTFB_EFI) --kernel $(KERNEL)

# No vendor alias or test-only kernel match is involved: QEMU exposes
# 1002:5046.  Keep the UEFI half separate because it first has to build the
# independent loader; a loader ABI failure is not evidence against this PCI
# driver and must not erase the BIOS result.
test-amd-bootfb-guest: test-amd-bootfb-host $(ISO)
	@python3 tests/boot/run-amd-bootfb.py --firmware bios --iso $(ISO) \
	    --out $(BUILD)/amd-bootfb-guest

test-amd-bootfb-uefi-guest: test-amd-bootfb-host $(ISO) $(AMD_BOOTFB_ESP)
	@python3 tests/boot/run-amd-bootfb.py --firmware uefi --iso $(ISO) \
	    --esp $(AMD_BOOTFB_ESP) --out $(BUILD)/amd-bootfb-guest

test-amd-bootfb-guest-all: test-amd-bootfb-guest test-amd-bootfb-uefi-guest

ci-host: test-amd-bootfb-host
