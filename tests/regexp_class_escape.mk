REGEXP_CLASS_DIR = $(BUILD)/site-general/regexp-class
REGEXP_CLASS_SRC = tests/unit/regexp_class_escape_test.c $(QJS_SRC)
REGEXP_CLASS_DEPS = $(REGEXP_CLASS_SRC) tests/unit/js_syntax_test.c
$(REGEXP_CLASS_DIR)/current: $(REGEXP_CLASS_DEPS)
	@mkdir -p $(REGEXP_CLASS_DIR)
	$(CC) -O2 -w $(JS_INC) -DCONFIG_VERSION='"host"' -o $@ $(REGEXP_CLASS_SRC) -lm
$(REGEXP_CLASS_DIR)/old: $(REGEXP_CLASS_DEPS)
	@mkdir -p $(REGEXP_CLASS_DIR)
	$(CC) -O2 -w $(JS_INC) -DCONFIG_VERSION='"host"' -DLRE_CLASS_HYPHEN_LEGACY -o $@ $(REGEXP_CLASS_SRC) -lm
.PHONY: test-regexp-class-escape test-regexp-class-escape-negctl
test-regexp-class-escape-negctl: $(REGEXP_CLASS_DIR)/old
	@rc=0; $< > $(REGEXP_CLASS_DIR)/old.log 2>&1 || rc=$$?; cat $(REGEXP_CLASS_DIR)/old.log; test $$rc -eq 1 && grep -q 'FAIL: site editor character class' $(REGEXP_CLASS_DIR)/old.log
test-regexp-class-escape: test-regexp-class-escape-negctl $(REGEXP_CLASS_DIR)/current
	@$(REGEXP_CLASS_DIR)/current
ci-host: test-regexp-class-escape

REGEXP_INTERRUPT_SRC = tests/unit/regexp_interrupt_test.c $(QJS_SRC)
$(REGEXP_CLASS_DIR)/interrupt: $(REGEXP_INTERRUPT_SRC)
	@mkdir -p $(REGEXP_CLASS_DIR)
	$(CC) -O2 -w $(JS_INC) -DCONFIG_VERSION='"host"' -o $@ $(REGEXP_INTERRUPT_SRC) -lm
$(REGEXP_CLASS_DIR)/interrupt-old: $(REGEXP_INTERRUPT_SRC)
	@mkdir -p $(REGEXP_CLASS_DIR)
	$(CC) -O2 -w $(JS_INC) -DCONFIG_VERSION='"host"' -DLRE_UNINTERRUPTIBLE_LEGACY -o $@ $(REGEXP_INTERRUPT_SRC) -lm
$(REGEXP_CLASS_DIR)/interrupt-san: $(REGEXP_INTERRUPT_SRC)
	@mkdir -p $(REGEXP_CLASS_DIR)
	$(CC) -O1 -g -w -fsanitize=address,undefined $(JS_INC) -DCONFIG_VERSION='"host"' -o $@ $(REGEXP_INTERRUPT_SRC) -lm
.PHONY: test-regexp-interrupt test-regexp-interrupt-negctl
test-regexp-interrupt-negctl: $(REGEXP_CLASS_DIR)/interrupt-old
	@rc=0; $< > $(REGEXP_CLASS_DIR)/interrupt-old.log 2>&1 || rc=$$?; cat $(REGEXP_CLASS_DIR)/interrupt-old.log; test $$rc -eq 1 && grep -q 'FAIL: native regexp services cancellation' $(REGEXP_CLASS_DIR)/interrupt-old.log
test-regexp-interrupt: test-regexp-interrupt-negctl $(REGEXP_CLASS_DIR)/interrupt $(REGEXP_CLASS_DIR)/interrupt-san
	@$(REGEXP_CLASS_DIR)/interrupt
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(REGEXP_CLASS_DIR)/interrupt-san
ci-host: test-regexp-interrupt

REGEXP_PROGRESS_SRC = tests/unit/regexp_progress_test.c $(QJS_SRC)
$(REGEXP_CLASS_DIR)/progress: $(REGEXP_PROGRESS_SRC) tests/unit/js_syntax_test.c third_party/quickjs/libregexp-opcode.h
	@mkdir -p $(REGEXP_CLASS_DIR)
	$(CC) -O2 -w $(JS_INC) -DCONFIG_VERSION='"host"' -o $@ $(REGEXP_PROGRESS_SRC) -lm
$(REGEXP_CLASS_DIR)/progress-old: $(REGEXP_PROGRESS_SRC) tests/unit/js_syntax_test.c third_party/quickjs/libregexp-opcode.h
	@mkdir -p $(REGEXP_CLASS_DIR)
	$(CC) -O2 -w $(JS_INC) -DCONFIG_VERSION='"host"' -DLRE_LAZY_PROGRESS_LEGACY -o $@ $(REGEXP_PROGRESS_SRC) -lm
$(REGEXP_CLASS_DIR)/progress-san: $(REGEXP_PROGRESS_SRC) tests/unit/js_syntax_test.c third_party/quickjs/libregexp-opcode.h
	@mkdir -p $(REGEXP_CLASS_DIR)
	$(CC) -O1 -g -w -fsanitize=address,undefined $(JS_INC) -DCONFIG_VERSION='"host"' -o $@ $(REGEXP_PROGRESS_SRC) -lm
.PHONY: test-regexp-progress test-regexp-progress-negctl
test-regexp-progress-negctl: $(REGEXP_CLASS_DIR)/progress-old
	@rc=0; $< > $(REGEXP_CLASS_DIR)/progress-old.log 2>&1 || rc=$$?; cat $(REGEXP_CLASS_DIR)/progress-old.log; test $$rc -eq 1 && test "$$(grep -c 'FAIL: nullable repetition has progress guard' $(REGEXP_CLASS_DIR)/progress-old.log)" -eq 4
test-regexp-progress: test-regexp-progress-negctl $(REGEXP_CLASS_DIR)/progress $(REGEXP_CLASS_DIR)/progress-san
	@$(REGEXP_CLASS_DIR)/progress
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(REGEXP_CLASS_DIR)/progress-san
ci-host: test-regexp-progress
