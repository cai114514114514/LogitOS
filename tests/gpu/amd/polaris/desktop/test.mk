AMD_DESKTOP_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Ic/drivers/gpu -Ic/kernel/gui -Ic/kernel/core -Ic/kernel/diag
AMD_DESKTOP_SRC := c/drivers/gpu/amd/polaris/desktop.c
AMD_DESKTOP_TEST := tests/gpu/amd/polaris/desktop/test.c
.PHONY: test-polaris-desktop-host test-polaris-desktop-negctl
# The control mutates a build-directory COPY of the production adapter. The
# exact seam assertion fails loudly if source formatting/structure moves it.
test-polaris-desktop-negctl:
	@mkdir -p $(BUILD)/desktop
	@python3 -c 'from pathlib import Path; p=Path("$(AMD_DESKTOP_SRC)").read_text(); old="stride * 4, x"; assert p.count(old)==1; Path("$(BUILD)/desktop/negative.c").write_text(p.replace(old,"stride, x"))'
	@$(CC) $(AMD_DESKTOP_FLAGS) $(AMD_DESKTOP_TEST) $(BUILD)/desktop/negative.c -o $(BUILD)/desktop/negative
	@if $(BUILD)/desktop/negative success >$(BUILD)/desktop/negative.log 2>&1; then cat $(BUILD)/desktop/negative.log; exit 1; fi
	@grep -q 'FAIL line .*last_stride==256' $(BUILD)/desktop/negative.log
	@grep -q '^DESKTOP success: .* checks, 1 failures' $(BUILD)/desktop/negative.log
	@echo 'DESKTOP_NEGCTL: pixel/byte stride regression rejected'

test-polaris-desktop-host: test-polaris-desktop-negctl
	@$(CC) $(AMD_DESKTOP_FLAGS) $(AMD_DESKTOP_TEST) $(AMD_DESKTOP_SRC) -o $(BUILD)/desktop/test
	@for scenario in success no-boot geometry lease bind address translation runtime; do $(BUILD)/desktop/test $$scenario || exit 1; done
ci-host: test-polaris-desktop-host
