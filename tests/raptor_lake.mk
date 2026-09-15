# Core i7-14700KF / Raptor Lake-S CPU-platform acceptance.  Host fixtures use
# real architectural shapes but are not physical-hardware evidence.  Each
# positive run depends on controls that reintroduce the six silent bugs this
# gate is meant to catch: uniform-SMT core math, CPUID.0B preference, and a
# broadened undocumented model/stepping allowlist, plus a permissive malformed
# topology parser, fabricated hybrid legacy topology, and HFI geometry decoded
# without its base capability bit.
RAPTOR_HOST_BIN := $(BUILD)/raptor-lake-platform-test
RAPTOR_CONTROL_DIR := $(BUILD)/raptor-lake-controls
RAPTOR_HYBRID_LEGACY_CONTROL := $(RAPTOR_CONTROL_DIR)/hybrid-legacy
RAPTOR_HFI_UNGATED_CONTROL := $(RAPTOR_CONTROL_DIR)/hfi-ungated

$(RAPTOR_HOST_BIN): tests/unit/raptor_lake_platform_test.c \
                    c/kernel/cpu/cpu_platform.c c/kernel/cpu/cpu_platform.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror $(KCPU_INC) \
	    tests/unit/raptor_lake_platform_test.c c/kernel/cpu/cpu_platform.c -o $@

.PHONY: test-raptor-lake-host test-raptor-lake-controls
test-raptor-lake-host: test-raptor-lake-controls $(RAPTOR_HOST_BIN)
	python3 tests/unit/raptor_lake_policy.py --check-report
	$(RAPTOR_HOST_BIN)

test-raptor-lake-controls:
	@mkdir -p $(RAPTOR_CONTROL_DIR)
	@python3 tests/unit/raptor_lake_policy.py uniform-smt $(RAPTOR_CONTROL_DIR)/uniform-smt.c
	@$(CC) -std=c11 -O2 -Wall -Wextra -Werror $(KCPU_INC) \
	    tests/unit/raptor_lake_platform_test.c $(RAPTOR_CONTROL_DIR)/uniform-smt.c \
	    -o $(RAPTOR_CONTROL_DIR)/uniform-smt
	@if $(RAPTOR_CONTROL_DIR)/uniform-smt > $(RAPTOR_CONTROL_DIR)/uniform-smt.log 2>&1; then \
	    cat $(RAPTOR_CONTROL_DIR)/uniform-smt.log; \
	    echo "NEGCTL FAIL: uniform SMT inference passed"; exit 1; \
	else \
	    grep -F "FAIL: hybrid topology refuses uniform-SMT physical core inference" \
	        $(RAPTOR_CONTROL_DIR)/uniform-smt.log >/dev/null || exit 1; \
	    echo "NEGCTL RED: uniform SMT inference rejected"; \
	fi
	@python3 tests/unit/raptor_lake_policy.py leaf-b-first $(RAPTOR_CONTROL_DIR)/leaf-b-first.c
	@$(CC) -std=c11 -O2 -Wall -Wextra -Werror $(KCPU_INC) \
	    tests/unit/raptor_lake_platform_test.c $(RAPTOR_CONTROL_DIR)/leaf-b-first.c \
	    -o $(RAPTOR_CONTROL_DIR)/leaf-b-first
	@if $(RAPTOR_CONTROL_DIR)/leaf-b-first > $(RAPTOR_CONTROL_DIR)/leaf-b-first.log 2>&1; then \
	    cat $(RAPTOR_CONTROL_DIR)/leaf-b-first.log; \
	    echo "NEGCTL FAIL: CPUID.0B preference passed"; exit 1; \
	else \
	    grep -F "FAIL: CPUID.1F is preferred over legacy CPUID.0B" \
	        $(RAPTOR_CONTROL_DIR)/leaf-b-first.log >/dev/null || exit 1; \
	    echo "NEGCTL RED: CPUID.0B preference rejected"; \
	fi
	@python3 tests/unit/raptor_lake_policy.py model-range $(RAPTOR_CONTROL_DIR)/model-range.c
	@$(CC) -std=c11 -O2 -Wall -Wextra -Werror $(KCPU_INC) \
	    tests/unit/raptor_lake_platform_test.c $(RAPTOR_CONTROL_DIR)/model-range.c \
	    -o $(RAPTOR_CONTROL_DIR)/model-range
	@if $(RAPTOR_CONTROL_DIR)/model-range > $(RAPTOR_CONTROL_DIR)/model-range.log 2>&1; then \
	    cat $(RAPTOR_CONTROL_DIR)/model-range.log; \
	    echo "NEGCTL FAIL: undocumented model stepping passed"; exit 1; \
	else \
	    grep -F "FAIL: Raptor Lake-S generation follows documented model/stepping range" \
	        $(RAPTOR_CONTROL_DIR)/model-range.log >/dev/null || exit 1; \
	    echo "NEGCTL RED: undocumented model stepping rejected"; \
	fi
	@python3 tests/unit/raptor_lake_policy.py malformed-topology \
	    $(RAPTOR_CONTROL_DIR)/malformed-topology.c
	@$(CC) -std=c11 -O2 -Wall -Wextra -Werror $(KCPU_INC) \
	    tests/unit/raptor_lake_platform_test.c \
	    $(RAPTOR_CONTROL_DIR)/malformed-topology.c \
	    -o $(RAPTOR_CONTROL_DIR)/malformed-topology
	@if $(RAPTOR_CONTROL_DIR)/malformed-topology \
	        > $(RAPTOR_CONTROL_DIR)/malformed-topology.log 2>&1; then \
	    cat $(RAPTOR_CONTROL_DIR)/malformed-topology.log; \
	    echo "NEGCTL FAIL: permissive malformed topology passed"; exit 1; \
	else \
	    grep -F "FAIL: duplicate CPUID.1F Core level falls back" \
	        $(RAPTOR_CONTROL_DIR)/malformed-topology.log >/dev/null || exit 1; \
	    echo "NEGCTL RED: permissive malformed topology rejected"; \
	fi
	@$(CC) -std=c11 -O2 -Wall -Wextra -Werror $(KCPU_INC) \
	    -DLOGIT_RAPTOR_NEGCTL_KEEP_HYBRID_LEGACY_TOPOLOGY \
	    tests/unit/raptor_lake_platform_test.c c/kernel/cpu/cpu_platform.c \
	    -o $(RAPTOR_HYBRID_LEGACY_CONTROL)
	@if $(RAPTOR_HYBRID_LEGACY_CONTROL) \
	        > $(RAPTOR_HYBRID_LEGACY_CONTROL).log 2>&1; then \
	    cat $(RAPTOR_HYBRID_LEGACY_CONTROL).log; \
	    echo "NEGCTL FAIL: hybrid legacy topology passed"; exit 1; \
	else \
	    grep -F "FAIL: hybrid CPU without extended topology refuses legacy core identity" \
	        $(RAPTOR_HYBRID_LEGACY_CONTROL).log >/dev/null || exit 1; \
	    test "$$(grep -c '^FAIL:' $(RAPTOR_HYBRID_LEGACY_CONTROL).log)" -eq 1 || exit 1; \
	    echo "NEGCTL RED: fabricated hybrid legacy topology rejected"; \
	fi
	@$(CC) -std=c11 -O2 -Wall -Wextra -Werror $(KCPU_INC) \
	    -DLOGIT_RAPTOR_NEGCTL_DECODE_HFI_WITHOUT_CAP \
	    tests/unit/raptor_lake_platform_test.c c/kernel/cpu/cpu_platform.c \
	    -o $(RAPTOR_HFI_UNGATED_CONTROL)
	@if $(RAPTOR_HFI_UNGATED_CONTROL) \
	        > $(RAPTOR_HFI_UNGATED_CONTROL).log 2>&1; then \
	    cat $(RAPTOR_HFI_UNGATED_CONTROL).log; \
	    echo "NEGCTL FAIL: ungated HFI geometry passed"; exit 1; \
	else \
	    grep -F "FAIL: absent HFI capability gates stray Thread Director geometry" \
	        $(RAPTOR_HFI_UNGATED_CONTROL).log >/dev/null || exit 1; \
	    test "$$(grep -c '^FAIL:' $(RAPTOR_HFI_UNGATED_CONTROL).log)" -eq 1 || exit 1; \
	    echo "NEGCTL RED: HFI geometry without capability rejected"; \
	fi
