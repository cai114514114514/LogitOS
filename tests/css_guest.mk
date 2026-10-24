# Host fault controls are prerequisites of the guest positives as well. Each
# run gets its own output directory, QMP socket and HTTP port, so simultaneous
# agents cannot satisfy one another's readiness checks or overwrite evidence.
CSS_GUEST_DIR := $(BUILD)/css-guest
.PHONY: test-css-guest test-css-components-guest test-css-background-guest test-css-gradients-guest test-css-layers-guest
test-css-components-guest: test-css-layout test-css-nesting test-css-background-image $(ISO) $(DISK)
	@mkdir -p $(CSS_GUEST_DIR)
	@css_out="$(CSS_GUEST_DIR)/components-$$(python3 -c 'import uuid; print(uuid.uuid4().hex)')"; \
	python3 tests/qmp/qmp_css_components.py $(ISO) $(DISK) --out "$$css_out"
test-css-background-guest: test-css-background $(ISO) $(DISK)
	@mkdir -p $(CSS_GUEST_DIR)
	@css_out="$(CSS_GUEST_DIR)/background-$$(python3 -c 'import uuid; print(uuid.uuid4().hex)')"; \
	python3 tests/qmp/qmp_css_background.py $(ISO) $(DISK) --out "$$css_out"
test-css-gradients-guest: test-css-gradient-parse test-css-gradients $(ISO) $(DISK)
	@mkdir -p $(CSS_GUEST_DIR)
	@css_out="$(CSS_GUEST_DIR)/gradients-$$(python3 -c 'import uuid; print(uuid.uuid4().hex)')"; \
	python3 tests/qmp/qmp_css_background.py $(ISO) $(DISK) --case gradients --out "$$css_out"
test-css-layers-guest: test-css-background-layers $(ISO) $(DISK)
	@mkdir -p $(CSS_GUEST_DIR)
	@css_out="$(CSS_GUEST_DIR)/layers-$$(python3 -c 'import uuid; print(uuid.uuid4().hex)')"; \
	python3 tests/qmp/qmp_css_background.py $(ISO) $(DISK) --case layers --out "$$css_out"
test-css-guest: test-css-components-guest test-css-background-guest test-css-gradients-guest test-css-layers-guest
ci-boot: test-css-guest
