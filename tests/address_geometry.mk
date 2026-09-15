ADDRESS_DIR := $(BUILD)/site-general/caret
ADDRESS_SRC := tests/unit/address_geometry_test.c
ADDRESS_DEPS := $(ADDRESS_SRC) c/apps/browser/address_geometry.inc tests/unit/painthost/logit.h
$(ADDRESS_DIR)/address: $(ADDRESS_DEPS)
	@mkdir -p $(ADDRESS_DIR)
	$(CC) -O2 -Itests/unit/painthost -Iinclude/abi $< -o $@
$(ADDRESS_DIR)/address-negctl: $(ADDRESS_DEPS)
	@mkdir -p $(ADDRESS_DIR)
	$(CC) -O2 -DBROWSER_ADDRESS_OLD_GEOMETRY -Itests/unit/painthost -Iinclude/abi $< -o $@
.PHONY: test-address-geometry test-address-geometry-negctl
test-address-geometry-negctl: $(ADDRESS_DIR)/address-negctl
	@rc=0; $< > $(ADDRESS_DIR)/address-negctl.log 2>&1 || rc=$$?; cat $(ADDRESS_DIR)/address-negctl.log; test $$rc -eq 1 && grep -q 'FAIL: mixed UTF8 caret follows characters, not bytes' $(ADDRESS_DIR)/address-negctl.log
test-address-geometry: test-address-geometry-negctl $(ADDRESS_DIR)/address
	@$(ADDRESS_DIR)/address
