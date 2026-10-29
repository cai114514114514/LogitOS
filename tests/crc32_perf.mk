# CRC32 byte-table hot path.  The watched negative control compiles the exact
# former nibble-table loop and must fail only the one-lookup-per-byte invariant.
CRC32_PERF_DIR := $(BUILD)/crc32-perf
CRC32_PERF_SRC := tests/unit/crc32_perf_test.c
CRC32_IMPL := c/drivers/block/crc32.c
CRC32_PERF_FLAGS := -O2 -Wall -Wextra -Werror -Ic/drivers/block

$(CRC32_PERF_DIR)/current: $(CRC32_PERF_SRC) $(CRC32_IMPL) c/drivers/block/crc32.h tests/crc32_perf.mk
	@mkdir -p $(CRC32_PERF_DIR)
	$(CC) $(CRC32_PERF_FLAGS) -DCRC32_TEST_COUNT_LOOKUPS -o $@ $(CRC32_PERF_SRC) $(CRC32_IMPL)

$(CRC32_PERF_DIR)/nibble: $(CRC32_PERF_SRC) $(CRC32_IMPL) c/drivers/block/crc32.h tests/crc32_perf.mk
	@mkdir -p $(CRC32_PERF_DIR)
	$(CC) $(CRC32_PERF_FLAGS) -DCRC32_TEST_COUNT_LOOKUPS -DCRC32_NIBBLE_NEGCTL \
	 -o $@ $(CRC32_PERF_SRC) $(CRC32_IMPL)

$(CRC32_PERF_DIR)/current.o: $(CRC32_IMPL) c/drivers/block/crc32.h tests/crc32_perf.mk
	@mkdir -p $(CRC32_PERF_DIR)
	$(CC) $(CRC32_PERF_FLAGS) -Dcrc32_update=crc32_update_current \
	 -Dcrc32=crc32_current -c -o $@ $(CRC32_IMPL)

$(CRC32_PERF_DIR)/nibble.o: $(CRC32_IMPL) c/drivers/block/crc32.h tests/crc32_perf.mk
	@mkdir -p $(CRC32_PERF_DIR)
	$(CC) $(CRC32_PERF_FLAGS) -DCRC32_NIBBLE_NEGCTL \
	 -Dcrc32_update=crc32_update_nibble -Dcrc32=crc32_nibble \
	 -c -o $@ $(CRC32_IMPL)

$(CRC32_PERF_DIR)/compare: $(CRC32_PERF_SRC) $(CRC32_PERF_DIR)/current.o $(CRC32_PERF_DIR)/nibble.o tests/crc32_perf.mk
	$(CC) $(CRC32_PERF_FLAGS) -DCRC32_PERF_COMPARE -o $@ $(CRC32_PERF_SRC) \
	 $(CRC32_PERF_DIR)/current.o $(CRC32_PERF_DIR)/nibble.o

.PHONY: test-crc32-fast-negctl test-crc32-fast test-crc32-fast-asan bench-crc32
test-crc32-fast-negctl: $(CRC32_PERF_DIR)/nibble
	@rc=0; $< > $(CRC32_PERF_DIR)/nibble.log 2>&1 || rc=$$?; \
	 cat $(CRC32_PERF_DIR)/nibble.log; \
	 test $$rc -eq 1 && \
	 grep -F 'FAIL: one table lookup per byte invariant' $(CRC32_PERF_DIR)/nibble.log && \
	 grep -F 'crc32 fast path: 6 checks, 1 failure' $(CRC32_PERF_DIR)/nibble.log

test-crc32-fast: test-crc32-fast-negctl $(CRC32_PERF_DIR)/current $(CRC32_PERF_DIR)/compare
	@$(CRC32_PERF_DIR)/current
	@$(CRC32_PERF_DIR)/compare | tee $(CRC32_PERF_DIR)/compare.log

test-crc32-fast-asan: test-crc32-fast-negctl
	@mkdir -p $(CRC32_PERF_DIR)
	$(CC) -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
	 -fno-sanitize-recover=all -fno-omit-frame-pointer -Ic/drivers/block \
	 -DCRC32_TEST_COUNT_LOOKUPS -o $(CRC32_PERF_DIR)/sanitize \
	 $(CRC32_PERF_SRC) $(CRC32_IMPL)
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	 $(CRC32_PERF_DIR)/sanitize

bench-crc32: $(CRC32_PERF_DIR)/compare
	@$(CRC32_PERF_DIR)/compare | tee $(CRC32_PERF_DIR)/compare.log

ci-host: test-crc32-fast
