# GTX 1050/1050 Ti passive boot-display support.  The host gate proves exact
# IDs, D0/BAR/LFB ownership checks, and zero writes.  The guest is explicitly
# synthetic: QEMU stdvga is admitted only by NVIDIA_PASCAL_QEMU_TEST so BIOS
# LFB and UEFI GOP can exercise the complete declarative binding path without
# pretending QEMU emulates a GP107.
NV_PASCAL_SRC := tests/unit/nvidia_pascal_test.c c/drivers/gpu/nvidia_pascal.c
NV_PASCAL_INC := -Ic/drivers/gpu -Ic/drivers/core -Ic/kernel/pci -Ic/kernel/gui -Ic/kernel/core
NV_PASCAL_ESP := $(BUILD)/esp.img

ifeq ($(PASCALVERIFY),1)
CFLAGS += -DNVIDIA_PASCAL_QEMU_TEST
endif

.PHONY: test-nvidia-pascal-negctl test-nvidia-pascal-host \
        test-nvidia-pascal-synthetic-guest

test-nvidia-pascal-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror -DLOGIT_HOST_TEST \
	    -DNVIDIA_PASCAL_NEGCTL_BROAD_ID $(NV_PASCAL_INC) $(NV_PASCAL_SRC) \
	    -o $(BUILD)/nvidia-pascal-neg-broad
	@if $(BUILD)/nvidia-pascal-neg-broad >$(BUILD)/nvidia-pascal-neg-broad.log 2>&1; then \
	    cat $(BUILD)/nvidia-pascal-neg-broad.log; exit 1; fi
	@grep -q '^FAIL: adjacent MX150 ID is not accepted as GTX 1050$$' $(BUILD)/nvidia-pascal-neg-broad.log
	@grep -q '^NV_BOOTFB: 56 checks, 1 failures$$' $(BUILD)/nvidia-pascal-neg-broad.log
	@echo 'NV_BOOTFB_NEGCTL: broad Pascal ID rule misidentified MX150 (exactly one failure)'
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror -DLOGIT_HOST_TEST \
	    -DNVIDIA_PASCAL_NEGCTL_ACCEPT_D3 $(NV_PASCAL_INC) $(NV_PASCAL_SRC) \
	    -o $(BUILD)/nvidia-pascal-neg-d3
	@if $(BUILD)/nvidia-pascal-neg-d3 >$(BUILD)/nvidia-pascal-neg-d3.log 2>&1; then \
	    cat $(BUILD)/nvidia-pascal-neg-d3.log; exit 1; fi
	@grep -q '^FAIL: D3 GPU is refused instead of being woken by an unsafe config write$$' $(BUILD)/nvidia-pascal-neg-d3.log
	@grep -q '^NV_BOOTFB: 56 checks, 1 failures$$' $(BUILD)/nvidia-pascal-neg-d3.log
	@echo 'NV_BOOTFB_NEGCTL: accepting D3 violated passive-probe contract (exactly one failure)'

test-nvidia-pascal-host: test-nvidia-pascal-negctl
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror \
	    -DLOGIT_HOST_TEST $(NV_PASCAL_INC) $(NV_PASCAL_SRC) \
	    -o $(BUILD)/nvidia-pascal-host
	@$(BUILD)/nvidia-pascal-host

# Use a dedicated BUILD because object rules do not track CFLAGS.  A product
# image built without PASCALVERIFY contains no QEMU ID and can bind only the
# twelve audited 10de IDs.
test-nvidia-pascal-synthetic-guest: test-nvidia-pascal-host $(ISO) $(NV_PASCAL_ESP)
	@test "$(PASCALVERIFY)" = 1 || { \
	    echo 'Use PASCALVERIFY=1 with an independent BUILD; this is a synthetic test kernel'; \
	    exit 1; }
	@python3 tests/boot/run-nvidia-pascal.py --iso $(ISO) --esp $(NV_PASCAL_ESP) \
	    --out $(BUILD)/nvidia-pascal-guest

ci-host: test-nvidia-pascal-host
