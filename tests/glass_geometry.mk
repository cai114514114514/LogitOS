# Full production display renderer; GFX_SRC also picks up material source splits.
GLASS_GEOMETRY_SRC := tests/unit/glass_geometry_test.c $(GFX_SRC)
GLASS_GEOMETRY_DEPS := $(GLASS_GEOMETRY_SRC) $(GFX_HEADERS)
GLASS_GEOMETRY_FLAGS := -O1 -g -Wall -Wextra $(GFX_INC)

$(BUILD)/glass_geometry_test: $(GLASS_GEOMETRY_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(GLASS_GEOMETRY_FLAGS) -o $@ $(GLASS_GEOMETRY_SRC) -lm

$(BUILD)/glass_geometry_root_neg: $(GLASS_GEOMETRY_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(GLASS_GEOMETRY_FLAGS) -DGLASS_ROOT_NARROW -o $@ $(GLASS_GEOMETRY_SRC) -lm

$(BUILD)/glass_geometry_crop_neg: $(GLASS_GEOMETRY_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(GLASS_GEOMETRY_FLAGS) -DGLASS_CROP_REBASE -o $@ $(GLASS_GEOMETRY_SRC) -lm

$(BUILD)/glass_geometry_allocation_neg: $(GLASS_GEOMETRY_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(GLASS_GEOMETRY_FLAGS) -DGLASS_FALLBACK_BLUR -o $@ $(GLASS_GEOMETRY_SRC) -lm

$(BUILD)/glass_geometry_square_neg: $(GLASS_GEOMETRY_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(GLASS_GEOMETRY_FLAGS) -DGLASS_SQUARE_NOOP -o $@ $(GLASS_GEOMETRY_SRC) -lm

$(BUILD)/glass_geometry_round_neg: $(GLASS_GEOMETRY_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(GLASS_GEOMETRY_FLAGS) -DGLASS_SQUARE_ROUND -o $@ $(GLASS_GEOMETRY_SRC) -lm

$(BUILD)/glass_geometry_sanitize: $(GLASS_GEOMETRY_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(GLASS_GEOMETRY_FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $@ $(GLASS_GEOMETRY_SRC) -lm

$(BUILD)/glass_geometry_slow: $(GLASS_GEOMETRY_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(GLASS_GEOMETRY_FLAGS) -DGLASS_FIELD_SLOW -o $@ $(GLASS_GEOMETRY_SRC) -lm

.PHONY: test-glass-geometry test-glass-geometry-negctl test-glass-geometry-asan

# Each control has to fail its own oracle: a crop failure cannot stand in for
# a working root test, and a crash is not evidence. The allocation control must
# specifically corrupt pixels, not merely report the injected failure.
test-glass-geometry-negctl: $(BUILD)/glass_geometry_root_neg $(BUILD)/glass_geometry_crop_neg $(BUILD)/glass_geometry_allocation_neg $(BUILD)/glass_geometry_square_neg $(BUILD)/glass_geometry_round_neg
	@for pair in 'root sqrt' 'crop crop' 'allocation allocation-write' 'square square' 'round square'; do \
	  set -- $$pair; binary=$(BUILD)/glass_geometry_$$1_neg; log=$(BUILD)/glass_geometry_$$1_neg.log; \
	  $$binary >$$log 2>&1; status=$$?; \
	  if [ $$status -ne 1 ] || ! grep -q "^FAIL $$2 " $$log; then \
	    cat $$log; echo "glass-geometry-negctl: FAIL -- $$1 did not fail its oracle"; exit 1; \
	  fi; \
	  echo "glass-geometry-negctl: $$1 red as required"; \
	done

test-glass-geometry: test-glass-geometry-negctl $(BUILD)/glass_geometry_test $(BUILD)/glass_geometry_slow
	@$(BUILD)/glass_geometry_test >$(BUILD)/glass_geometry.log 2>&1; status=$$?; \
	 cat $(BUILD)/glass_geometry.log; test $$status -eq 0
	@$(BUILD)/glass_geometry_slow >$(BUILD)/glass_geometry_slow.log 2>&1; status=$$?; \
	 cat $(BUILD)/glass_geometry_slow.log; test $$status -eq 0
	@fast=$$(awk '/^glass-raster:/ { print $$2 }' $(BUILD)/glass_geometry.log); \
	 slow=$$(awk '/^glass-raster:/ { print $$2 }' $(BUILD)/glass_geometry_slow.log); \
	 if [ -z "$$fast" ] || [ "$$fast" != "$$slow" ]; then \
	   echo "glass-geometry: FAIL fast/slow raster $$fast != $$slow"; exit 1; \
	 fi; echo "glass-geometry: fast/slow raster identical ($$fast)"

test-glass-geometry-asan: $(BUILD)/glass_geometry_sanitize
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/glass_geometry_sanitize

ci-host: test-glass-geometry test-glass-geometry-asan
