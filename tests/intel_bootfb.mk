# Intel firmware-framebuffer observer.  The host gate covers old and new IDs
# through the vendor+class rule, proves exact LFB-to-BAR ownership, and watches
# every probe path remain free of writes, MMIO mapping, BME/DMA and IRQ setup.
INTEL_BOOTFB_SRC := tests/unit/intel_bootfb_test.c c/drivers/gpu/intel_bootfb.c
INTEL_BOOTFB_INC := -Ic/drivers/gpu -Ic/drivers/core -Ic/kernel/pci \
                    $(KGUI_INC) $(KCORE_INC)

.PHONY: test-intel-bootfb-negctl test-intel-bootfb-host

test-intel-bootfb-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror -DLOGIT_HOST_TEST \
	    -DINTEL_BOOTFB_NEGCTL_ACCEPT_FOREIGN $(INTEL_BOOTFB_INC) $(INTEL_BOOTFB_SRC) \
	    -o $(BUILD)/intel-bootfb-neg-foreign
	@if $(BUILD)/intel-bootfb-neg-foreign >$(BUILD)/intel-bootfb-neg-foreign.log 2>&1; then \
	    cat $(BUILD)/intel-bootfb-neg-foreign.log; exit 1; fi
	@grep -q '^FAIL: a foreign display vendor is never claimed by the Intel probe$$' \
	    $(BUILD)/intel-bootfb-neg-foreign.log
	@grep -q '^INTEL_BOOTFB: 31 checks, 1 failures$$' \
	    $(BUILD)/intel-bootfb-neg-foreign.log
	@echo 'INTEL_BOOTFB_NEGCTL: foreign display was claimed (exactly one failure)'
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror -DLOGIT_HOST_TEST \
	    -DINTEL_BOOTFB_NEGCTL_ACCEPT_D3 $(INTEL_BOOTFB_INC) $(INTEL_BOOTFB_SRC) \
	    -o $(BUILD)/intel-bootfb-neg-d3
	@if $(BUILD)/intel-bootfb-neg-d3 >$(BUILD)/intel-bootfb-neg-d3.log 2>&1; then \
	    cat $(BUILD)/intel-bootfb-neg-d3.log; exit 1; fi
	@grep -q '^FAIL: D3 Intel GPU is refused rather than woken$$' \
	    $(BUILD)/intel-bootfb-neg-d3.log
	@grep -q '^INTEL_BOOTFB: 31 checks, 1 failures$$' \
	    $(BUILD)/intel-bootfb-neg-d3.log
	@echo 'INTEL_BOOTFB_NEGCTL: D3 display was accepted (exactly one failure)'
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror -DLOGIT_HOST_TEST \
	    -DINTEL_BOOTFB_NEGCTL_ACCEPT_UNAVAILABLE $(INTEL_BOOTFB_INC) $(INTEL_BOOTFB_SRC) \
	    -o $(BUILD)/intel-bootfb-neg-unavailable
	@if $(BUILD)/intel-bootfb-neg-unavailable >$(BUILD)/intel-bootfb-neg-unavailable.log 2>&1; then \
	    cat $(BUILD)/intel-bootfb-neg-unavailable.log; exit 1; fi
	@grep -q '^FAIL: unreachable PCI command register is refused$$' \
	    $(BUILD)/intel-bootfb-neg-unavailable.log
	@grep -q '^INTEL_BOOTFB: 31 checks, 1 failures$$' \
	    $(BUILD)/intel-bootfb-neg-unavailable.log
	@echo 'INTEL_BOOTFB_NEGCTL: all-ones PCI command was accepted (exactly one failure)'
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror -DLOGIT_HOST_TEST \
	    -DINTEL_BOOTFB_NEGCTL_SKIP_LFB_OWNER $(INTEL_BOOTFB_INC) $(INTEL_BOOTFB_SRC) \
	    -o $(BUILD)/intel-bootfb-neg-owner
	@if $(BUILD)/intel-bootfb-neg-owner >$(BUILD)/intel-bootfb-neg-owner.log 2>&1; then \
	    cat $(BUILD)/intel-bootfb-neg-owner.log; exit 1; fi
	@grep -q '^FAIL: LFB outside every BAR is refused$$' $(BUILD)/intel-bootfb-neg-owner.log
	@grep -q '^FAIL: partially overlapping LFB is refused unless fully contained$$' \
	    $(BUILD)/intel-bootfb-neg-owner.log
	@grep -q '^FAIL: corrupt BAR marked as both memory and I/O is refused$$' \
	    $(BUILD)/intel-bootfb-neg-owner.log
	@grep -q '^INTEL_BOOTFB: 31 checks, 3 failures$$' \
	    $(BUILD)/intel-bootfb-neg-owner.log
	@echo 'INTEL_BOOTFB_NEGCTL: unowned LFB produced the three expected failures'

test-intel-bootfb-host: test-intel-bootfb-negctl
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror \
	    -DLOGIT_HOST_TEST $(INTEL_BOOTFB_INC) $(INTEL_BOOTFB_SRC) \
	    -o $(BUILD)/intel-bootfb-host
	@$(BUILD)/intel-bootfb-host

ci-host: test-intel-bootfb-host
