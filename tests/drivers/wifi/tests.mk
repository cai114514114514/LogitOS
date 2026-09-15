WIFI_CRYPTO_SRC := \
    c/drivers/net/wifi/security/derivation.c \
    c/drivers/net/wifi/security/keywrap.c \
    c/drivers/net/wifi/security/ccmp.c \
    c/crypto/hash/sha1.c \
    c/crypto/aead/aesgcm.c \
    c/crypto/aead/aes_dispatch.c \
    c/crypto/aead/aes_ni.c
WIFI_STATION_SRC := \
    tests/drivers/wifi/station_test.c \
    c/drivers/net/wifi/station.c \
    c/drivers/net/wifi/management/scan.c \
    c/drivers/net/wifi/management/association.c \
    c/drivers/net/wifi/management/receive.c \
    c/drivers/net/wifi/security/rsn.c \
    c/drivers/net/wifi/security/handshake.c \
    c/drivers/net/wifi/security/keydata.c \
    c/drivers/net/wifi/security/group.c \
    c/drivers/net/wifi/data/frame.c \
    $(WIFI_CRYPTO_SRC)
WIFI_TEST_FLAGS := \
    -std=c11 \
    -O1 \
    -g \
    -Wall \
    -Wextra \
    -Werror \
    -fsanitize=address,undefined \
    -Ic/drivers/net/wifi \
    -Ic/crypto \
    -Ic/crypto/aead \
    -Ic/kernel/cpu \
    -I$(BUILD)/oracle
.PHONY: test-wifi-station-negctl test-wifi-station-host
$(BUILD)/oracle/oracle.h: tests/drivers/wifi/oracle.py
	@python3 $< $@
test-wifi-station-negctl: $(BUILD)/oracle/oracle.h
	@mkdir -p $(BUILD)
	@$(CC) $(WIFI_TEST_FLAGS) -DWIFI_NEGCTL_MIC $(WIFI_STATION_SRC) -o $(BUILD)/station-control
	@if $(BUILD)/station-control >$(BUILD)/station-control.log 2>&1; then cat $(BUILD)/station-control.log; exit 1; fi
	@grep -q 'FAIL line .*wifi_ccm_open' $(BUILD)/station-control.log
	@grep -q '^WIFI_STATION: 312 checks, 4 failures$$' $(BUILD)/station-control.log
	@echo 'WIFI_NEGCTL: unauthenticated CCMP plaintext and PN advancement rejected'
test-wifi-station-host: test-wifi-station-negctl
	@$(CC) $(WIFI_TEST_FLAGS) $(WIFI_STATION_SRC) -o $(BUILD)/station
	@$(BUILD)/station
ci-host: test-wifi-station-host

WIFI_ADAPTER_FLAGS := $(WIFI_TEST_FLAGS) -DLOGIT_NET_HOST \
    -Ic/drivers/core -Ic/drivers/net -Ic/net/core -Ic/kernel/pci -Itests/unit/pcistub
WIFI_ADAPTER_SRC := tests/drivers/wifi/adapter_test.c c/drivers/net/wifi/adapter/netdev.c \
    $(filter-out tests/drivers/wifi/station_test.c,$(WIFI_STATION_SRC))
.PHONY: test-wifi-adapter-host
test-wifi-adapter-host: test-wifi-station-host
	@$(CC) $(WIFI_ADAPTER_FLAGS) $(WIFI_ADAPTER_SRC) -o $(BUILD)/adapter
	@$(BUILD)/adapter
ci-host: test-wifi-adapter-host

.PHONY: test-wifi-rekey-negctl test-wifi-adapter-negctl
test-wifi-rekey-negctl: $(BUILD)/oracle/oracle.h
	@$(CC) $(WIFI_TEST_FLAGS) -DWIFI_NEGCTL_REINSTALL $(WIFI_STATION_SRC) -o $(BUILD)/rekey-control
	@if $(BUILD)/rekey-control >$(BUILD)/rekey-control.log 2>&1; then cat $(BUILD)/rekey-control.log; exit 1; fi
	@grep -q 'group_keys\[2\].replay\[16\] == 1' $(BUILD)/rekey-control.log
	@grep -q '^WIFI_STATION: 312 checks, 2 failures$$' $(BUILD)/rekey-control.log
	@echo 'WIFI_REKEY_NEGCTL: resetting installed GTK replay windows is detected'
test-wifi-adapter-negctl: $(BUILD)/oracle/oracle.h
	@$(CC) $(WIFI_ADAPTER_FLAGS) -DWIFI_ADAPTER_NEGCTL_LINK $(WIFI_ADAPTER_SRC) -o $(BUILD)/adapter-control
	@if $(BUILD)/adapter-control >$(BUILD)/adapter-control.log 2>&1; then cat $(BUILD)/adapter-control.log; exit 1; fi
	@grep -q 'NETIF_F_RUNNING' $(BUILD)/adapter-control.log
	@grep -q '^WIFI_ADAPTER: 53 checks, 3 failures$$' $(BUILD)/adapter-control.log
	@echo 'WIFI_ADAPTER_NEGCTL: fabricated running link is detected'
test-wifi-station-host: test-wifi-rekey-negctl
test-wifi-adapter-host: test-wifi-adapter-negctl
