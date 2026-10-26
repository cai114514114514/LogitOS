# Cache lifetime uses the actual DNS parser and async state machine. A modeled
# clock makes expiry exact, including time spent waiting for another family.
# This host gate does not claim guest UDP delivery or resolver interoperability.

DNS_TTL_FLAGS = -Wall -Wextra -DLOGIT_NET_HOST $(DNS_INC)
DNS_TTL_CONTROLS := alias lifetime zero age
DNS_TTL_TARGETS := $(addprefix test-dns-ttl-,$(addsuffix -negctl,$(DNS_TTL_CONTROLS)))

DNS_TTL_MACRO_alias := DNS_NO_ALIAS_TTL
DNS_TTL_MACRO_lifetime := DNS_EXTEND_TTL
DNS_TTL_MACRO_zero := DNS_ZERO_TTL_UNSET
DNS_TTL_MACRO_age := DNS_RESET_CACHE_AGE
DNS_TTL_ORACLE_alias := short CNAME limits cache lifetime keeps TTL 1 (type 28,
DNS_TTL_ORACLE_lifetime := one second TTL at expiry sends a new query
DNS_TTL_ORACLE_zero := zero TTL survives family merge (first type 1, zero first 1)
DNS_TTL_ORACLE_age := received TTL at expiry sends a new query

.PHONY: test-dns-ttl test-dns-ttl-negctl $(DNS_TTL_TARGETS)

test-dns-ttl: test-dns-ttl-negctl
	@mkdir -p $(BUILD)
	@$(CC) -O2 $(DNS_TTL_FLAGS) tests/unit/dns_ttl_test.c -o $(BUILD)/dns_ttl_test
	@$(BUILD)/dns_ttl_test
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(DNS_TTL_FLAGS) tests/unit/dns_ttl_test.c -o $(BUILD)/dns_ttl_sanitize
	@$(BUILD)/dns_ttl_sanitize

test-dns-ttl-negctl: $(DNS_TTL_TARGETS)

# Each historical defect must fail its own behavioral oracle. A build failure,
# signal, or unrelated failure cannot satisfy the expected exit status + text.
$(DNS_TTL_TARGETS): test-dns-ttl-%-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 $(DNS_TTL_FLAGS) -D$(DNS_TTL_MACRO_$*) \
		tests/unit/dns_ttl_test.c -o $(BUILD)/dns_ttl_$*_negctl
	@control_rc=0; \
	$(BUILD)/dns_ttl_$*_negctl >$(BUILD)/dns_ttl_$*_negctl.log 2>&1 || control_rc=$$?; \
	if [ "$$control_rc" -ne 1 ] || \
	   ! grep -Fq '$(DNS_TTL_ORACLE_$*)' $(BUILD)/dns_ttl_$*_negctl.log; then \
		cat $(BUILD)/dns_ttl_$*_negctl.log; \
		echo "DNS TTL $* control failed: expected its specific assertion and exit 1"; \
		exit 1; \
	fi; \
	echo "DNS TTL $* control: $$(grep -c '^FAIL' $(BUILD)/dns_ttl_$*_negctl.log) expected failures"

test-dns: test-dns-ttl
ci-host: test-dns-ttl
