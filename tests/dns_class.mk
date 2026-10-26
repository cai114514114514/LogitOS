# DNS's Internet-class contract uses the same parser for A, AAAA and CNAME.
# Reuse DNS_INC and the existing modeled transport rather than copy its stubs.
# Host execution proves packet parsing/cache behavior, not a working NIC or
# guest UDP/TCP delivery; the ordinary on-device network gate covers that side.

.PHONY: test-dns-class test-dns-class-negctl

DNS_CLASS_FLAGS = -Wall -Wextra -DLOGIT_NET_HOST $(DNS_INC)

test-dns-class: test-dns-class-negctl
	@mkdir -p $(BUILD)
	@$(CC) -O2 $(DNS_CLASS_FLAGS) tests/unit/dns_class_test.c \
		-o $(BUILD)/dns_class_test
	@$(BUILD)/dns_class_test
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(DNS_CLASS_FLAGS) tests/unit/dns_class_test.c -o $(BUILD)/dns_class_sanitize
	@$(BUILD)/dns_class_sanitize

# Watch the original bug fail in both address families, alias traversal, and
# the public async API. Compilation or an unrelated crash is not a control.
test-dns-class-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 $(DNS_CLASS_FLAGS) -DDNS_NO_CLASS_MATCH \
		tests/unit/dns_class_test.c -o $(BUILD)/dns_class_negctl
	@status=0; $(BUILD)/dns_class_negctl >$(BUILD)/dns_class_negctl.log 2>&1 || status=$$?; \
	if [ "$$status" -ne 1 ] || \
	   ! grep -q 'non-IN address refused (type 1 class 3)' $(BUILD)/dns_class_negctl.log || \
	   ! grep -q 'non-IN address refused (type 28 class 3)' $(BUILD)/dns_class_negctl.log || \
	   ! grep -q 'non-IN CNAME cannot redirect an IN query' $(BUILD)/dns_class_negctl.log || \
	   ! grep -q 'async IN query rejects a CH answer' $(BUILD)/dns_class_negctl.log; then \
		cat $(BUILD)/dns_class_negctl.log; \
		echo "DNS class negative control failed: expected class-specific assertion failures"; \
		exit 1; \
	fi; \
	echo "DNS class negative control: $$(grep -c '^FAIL' $(BUILD)/dns_class_negctl.log) expected failures"

# Ordinary resolver validation must run this control and regression too.
test-dns: test-dns-class
ci-host: test-dns-class
