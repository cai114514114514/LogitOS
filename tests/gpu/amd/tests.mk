# AMD family gates follow the same hierarchy as production sources.
-include tests/gpu/amd/bootfb.mk
-include tests/gpu/amd/rv100/engine.mk
-include tests/gpu/amd/rv100/present.mk
-include tests/gpu/amd/polaris/device.mk
-include tests/gpu/amd/polaris/integration.mk
-include tests/gpu/amd/polaris/firmware.mk
-include tests/gpu/amd/polaris/sdma/packet.mk
-include tests/gpu/amd/polaris/smu/toc.mk
-include tests/gpu/amd/polaris/smu/mailbox.mk
-include tests/gpu/amd/polaris/smu/stage.mk
-include tests/gpu/amd/polaris/memory/tests.mk
-include tests/gpu/amd/polaris/firmware/bundle.mk
-include tests/gpu/amd/polaris/sdma/queue.mk
-include tests/gpu/amd/polaris/sdma/engine.mk
-include tests/gpu/amd/polaris/smu/loader.mk
-include tests/gpu/amd/polaris/native/test.mk
-include tests/gpu/amd/polaris/present.mk
-include tests/gpu/amd/polaris/runtime.mk
-include tests/gpu/amd/polaris/desktop/test.mk
-include tests/gpu/amd/polaris/resource/gate.mk

.PHONY: test-amd-host
test-amd-host: test-amd-bootfb-host test-amd-accel-host \
    test-polaris-probe-host test-polaris-integration-host \
    test-polaris-firmware-host test-polaris-sdma-host \
    test-polaris-smu-toc-host test-polaris-smu-mailbox-host test-polaris-smu-stage-host \
    test-polaris-memory-host test-polaris-bundle-host test-polaris-smu-loader-host \
    test-polaris-sdma-queue-host test-polaris-sdma-engine-host test-polaris-native-host \
    test-polaris-present-host test-polaris-runtime-host test-polaris-desktop-host \
    test-polaris-resources-host
