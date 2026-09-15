# The real loader/painter/DOM source set, replacing only the existing driver's
# transport fixture. Forms exposes native edit values through the real IDL.
INSERTED_ASYNC_SRC = $(sort $(filter-out tests/unit/script_resource_events_test.c tests/unit/loader_fakebfetch.c,$(SCRIPT_RESOURCE_SRC)) c/apps/browser/js_forms.c) tests/unit/inserted_script_async_test.c tests/unit/inserted_script_held_fake.c
INSERTED_ASYNC_DEPS = $(INSERTED_ASYNC_SRC) $(RUNTIME_SCROLL_DEPS) $(wildcard c/apps/browser/*.inc)
INSERTED_ASYNC_DIR = $(BUILD)/site-general/inserted-script-async
$(INSERTED_ASYNC_DIR)/current: $(INSERTED_ASYNC_DEPS)
	@mkdir -p $(INSERTED_ASYNC_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_IDLE_OBSERVER -o $@ $(INSERTED_ASYNC_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(INSERTED_ASYNC_DIR)/old: $(INSERTED_ASYNC_DEPS)
	@mkdir -p $(INSERTED_ASYNC_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_IDLE_OBSERVER -DBROWSER_INSERTED_SCRIPT_SYNC_WAIT -o $@ $(INSERTED_ASYNC_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(INSERTED_ASYNC_DIR)/load-old: $(INSERTED_ASYNC_DEPS)
	@mkdir -p $(INSERTED_ASYNC_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_IDLE_OBSERVER -DBROWSER_LOAD_BEFORE_SCRIPT_DRAIN -o $@ $(INSERTED_ASYNC_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-inserted-script-async test-inserted-script-async-negctl test-inserted-script-load-negctl
test-inserted-script-load-negctl: $(INSERTED_ASYNC_DIR)/load-old
	@rc=0; $(INSERTED_ASYNC_DIR)/load-old initial-idle > $(INSERTED_ASYNC_DIR)/old-load-idle.log 2>&1 || rc=$$?; \
	 tail -18 $(INSERTED_ASYNC_DIR)/old-load-idle.log; \
	 test $$rc -eq 1 && grep -q '^FAIL: initial load cannot park before lifecycle dispatch' $(INSERTED_ASYNC_DIR)/old-load-idle.log && \
	 grep -q '^ok: ordered scripts and onload chain execute exactly once in order' $(INSERTED_ASYNC_DIR)/old-load-idle.log && \
	 test "$$(grep -c '^FAIL:' $(INSERTED_ASYNC_DIR)/old-load-idle.log)" -eq 3
test-inserted-script-async-negctl: $(INSERTED_ASYNC_DIR)/old
	@for mode in input initial no-timer navigate tab close detach destroy mutate; do \
	 rc=0; $(INSERTED_ASYNC_DIR)/old $$mode > $(INSERTED_ASYNC_DIR)/old-$$mode.log 2>&1 || rc=$$?; \
	 tail -16 $(INSERTED_ASYNC_DIR)/old-$$mode.log; test $$rc -eq 1 || exit 1; \
	 case $$mode in \
	 input|initial) expected=3; pattern='native input painted while script pending'; \
	 grep -q '^ok: native event coordinates come from actual initial paint' $(INSERTED_ASYNC_DIR)/old-$$mode.log || exit 1;; \
	 no-timer) expected=1; pattern='pending script alone retains bounded pump wake';; \
	 detach) expected=4; pattern='detach happened after preparation while request pending';; \
	 destroy) expected=4; pattern='destroyed script neither executes nor dispatches to recycled slot';; \
	 mutate) expected=4; pattern='src mutation happened after preparation while request pending';; \
	 *) expected=2; pattern='native cancellation releases pending script without execution';; esac; \
	 grep -q "^FAIL: $$pattern" $(INSERTED_ASYNC_DIR)/old-$$mode.log && \
	 test "$$(grep -c '^FAIL:' $(INSERTED_ASYNC_DIR)/old-$$mode.log)" -eq $$expected || exit 1; \
	 if test $$mode != no-timer; then grep -q 'fallback=1' $(INSERTED_ASYNC_DIR)/old-$$mode.log || exit 1; fi; done
	@grep -q '^ok: detached script wrapper stays live through execution and load' $(INSERTED_ASYNC_DIR)/old-detach.log
test-inserted-script-async: test-inserted-script-async-negctl test-inserted-script-load-negctl $(INSERTED_ASYNC_DIR)/current
	@for mode in input initial initial-idle no-timer navigate tab close detach destroy mutate; do \
	 rc=0; $(INSERTED_ASYNC_DIR)/current $$mode > $(INSERTED_ASYNC_DIR)/current-$$mode.log 2>&1 || rc=$$?; \
	 tail -18 $(INSERTED_ASYNC_DIR)/current-$$mode.log; test $$rc -eq 0 || exit $$rc; done
ci-host: test-inserted-script-async

# The old detach control expected five incidental failures. With queued work
# yielding between turns it still fails the four actual blocked-loop checks
# (fallback, input, timer, pending detach), while the detached wrapper survives.
# Require that lifetime success as well; do not weaken the named failure gate.

# Burst retention and bounded download overlap use the same production queue.
include tests/inserted_script_queue.mk
