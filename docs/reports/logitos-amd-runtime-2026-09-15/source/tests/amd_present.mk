# Runtime evidence needs a CPU control that keeps the same device and startup
# canary. Use separate BUILD directories: Make does not track changed CFLAGS.
ifeq ($(AMD_PRESENT_DISABLE),1)
$(BUILD)/c/drivers/gpu/amd_accel.o: CFLAGS += -DAMD_PRESENT_DISABLE
endif

.PHONY: test-amd-present-guest test-amd-present-control
test-amd-present-guest: test-amd-accel-host test-fb-native-present-host $(ISO)
	python3 tests/boot/run-amd-rv100-present.py --iso $(ISO) --out $(BUILD)/amd-present-guest

# Pass AMD_PRESENT_CPU_ISO from a separate BUILD with AMD_PRESENT_DISABLE=1.
test-amd-present-control: test-amd-accel-host test-fb-native-present-host $(ISO)
	test -n "$(AMD_PRESENT_CPU_ISO)"
	python3 tests/boot/run-amd-rv100-present.py --iso $(ISO) --cpu-iso $(AMD_PRESENT_CPU_ISO) --out $(BUILD)/amd-present-control
