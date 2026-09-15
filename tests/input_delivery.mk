ifeq ($(strip $(INTERACTION_RUNTIME_SRC)),)
$(error tests/input_delivery.mk must be included after tests/interaction_runtime.mk)
endif
# Prerequisites expand when this rule is read, not when its recipe runs. An
# earlier include silently linked current sources only on the first compile.
INPUT_DELIVERY_SRC = $(filter-out tests/unit/interaction_runtime_test.c,$(INTERACTION_RUNTIME_SRC)) tests/unit/input_delivery_test.c
INPUT_DELIVERY_DEPS = $(INPUT_DELIVERY_SRC) $(filter-out tests/unit/interaction_runtime_test.c,$(INTERACTION_RUNTIME_DEPS)) tests/input_delivery.mk
.PHONY: test-input-delivery test-input-delivery-negctl test-input-trace-navigation-negctl test-input-paint-negctl
$(BUILD)/input_delivery_test: $(INPUT_DELIVERY_DEPS)
	$(CC) $(INTERACTION_RUNTIME_CF) -o $@ $(INPUT_DELIVERY_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/input_delivery_nofocus: $(INPUT_DELIVERY_DEPS)
	$(CC) $(INTERACTION_RUNTIME_CF) -DBROWSER_NO_FOCUS -o $@ $(INPUT_DELIVERY_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/input_delivery_nav: $(INPUT_DELIVERY_DEPS)
	$(CC) $(INTERACTION_RUNTIME_CF) -DBROWSER_INPUT_TRACE_NAVIGATES -o $@ $(INPUT_DELIVERY_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/input_delivery_static_paint: $(INPUT_DELIVERY_DEPS)
	$(CC) $(INTERACTION_RUNTIME_CF) -DPAINT_CONTROL_STATIC_SIGNATURE -o $@ $(INPUT_DELIVERY_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-input-delivery-negctl: $(BUILD)/input_delivery_nofocus
	@rc=0; $< > $(BUILD)/input_delivery_nofocus.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: native mouse focuses pointer-events override textarea' $(BUILD)/input_delivery_nofocus.log && \
	 grep -F 'FAIL: native key default edits and fires input' $(BUILD)/input_delivery_nofocus.log
test-input-trace-navigation-negctl: $(BUILD)/input_delivery_nav
	@rc=0; $< > $(BUILD)/input_delivery_nav.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: input diagnostic preserves live page and runtime' $(BUILD)/input_delivery_nav.log
test-input-paint-negctl: $(BUILD)/input_delivery_static_paint
	@rc=0; $< > $(BUILD)/input_delivery_static_paint.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: native edit paints and submits changed control frame' $(BUILD)/input_delivery_static_paint.log && \
	 grep -F 'FAIL: native backspace submits restored placeholder frame' $(BUILD)/input_delivery_static_paint.log && \
	 grep -F 'FAIL: caret-only left movement submits unchanged text frame' $(BUILD)/input_delivery_static_paint.log
test-input-delivery: test-input-delivery-negctl test-input-trace-navigation-negctl test-input-paint-negctl $(BUILD)/input_delivery_test
	@$(BUILD)/input_delivery_test
ci-host: test-input-delivery

EVENT_FAIRNESS_SRC = $(filter-out tests/unit/input_delivery_test.c,$(INPUT_DELIVERY_SRC)) tests/unit/event_fairness_test.c
EVENT_FAIRNESS_DEPS = $(filter-out tests/unit/input_delivery_test.c,$(INPUT_DELIVERY_DEPS)) tests/unit/event_fairness_test.c
.PHONY: test-event-fairness test-event-fairness-negctl
$(BUILD)/event_fairness_test: $(EVENT_FAIRNESS_DEPS)
	$(CC) $(INTERACTION_RUNTIME_CF) -o $@ $(EVENT_FAIRNESS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/event_fairness_negctl: $(EVENT_FAIRNESS_DEPS)
	$(CC) $(INTERACTION_RUNTIME_CF) -DBROWSER_UNBOUNDED_EVENT_BURST -o $@ $(EVENT_FAIRNESS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-event-fairness-negctl: $(BUILD)/event_fairness_negctl
	@rc=0; $< > $(BUILD)/event_fairness_negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: slow input burst paints before queue becomes empty' $(BUILD)/event_fairness_negctl.log && \
	 grep -F 'FAIL: same-tick event burst yields before queue becomes empty' $(BUILD)/event_fairness_negctl.log
test-event-fairness: test-event-fairness-negctl $(BUILD)/event_fairness_test
	@$(BUILD)/event_fairness_test
ci-host: test-event-fairness

# The IRQ-facing queue is separate from the browser fixture above: this gate
# drives the production kernel source and checks the response bound BEFORE an
# event reaches a window. Each positive depends on mutations that restore one
# old failure at a time, so a green stress run cannot merely be a weak fixture.
INPUT_QUEUE_SRC = tests/unit/input_queue_test.c c/kernel/gui/input_queue.c
INPUT_QUEUE_DEPS = $(INPUT_QUEUE_SRC) c/kernel/gui/input_queue.h tests/input_delivery.mk
INPUT_QUEUE_CF = -O2 -Wall -Wextra -Ic/kernel/gui
.PHONY: test-input-queue test-input-queue-negctl test-evq-priority-negctl
$(BUILD)/input_queue_test: $(INPUT_QUEUE_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(INPUT_QUEUE_CF) -o $@ $(INPUT_QUEUE_SRC)
$(BUILD)/input_queue_no_coalesce: $(INPUT_QUEUE_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(INPUT_QUEUE_CF) -DINPUTQ_NEGCTL_NO_COALESCE -o $@ $(INPUT_QUEUE_SRC)
$(BUILD)/input_queue_unbounded: $(INPUT_QUEUE_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(INPUT_QUEUE_CF) -DINPUTQ_NEGCTL_UNBOUNDED_DRAIN -o $@ $(INPUT_QUEUE_SRC)
$(BUILD)/input_queue_drop_critical: $(INPUT_QUEUE_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(INPUT_QUEUE_CF) -DINPUTQ_NEGCTL_DROP_CRITICAL -o $@ $(INPUT_QUEUE_SRC)
$(BUILD)/input_queue_ack_dropped_edge: $(INPUT_QUEUE_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(INPUT_QUEUE_CF) -DINPUTQ_NEGCTL_ACK_DROPPED_EDGE -o $@ $(INPUT_QUEUE_SRC)
test-input-queue-negctl: $(BUILD)/input_queue_no_coalesce $(BUILD)/input_queue_unbounded $(BUILD)/input_queue_drop_critical $(BUILD)/input_queue_ack_dropped_edge
	@rc=0; $(BUILD)/input_queue_no_coalesce > $(BUILD)/input_queue_no_coalesce.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: pointer flood retains exactly the newest position' $(BUILD)/input_queue_no_coalesce.log
	@rc=0; $(BUILD)/input_queue_unbounded > $(BUILD)/input_queue_unbounded.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: one WM pass has a fixed input-work bound' $(BUILD)/input_queue_unbounded.log
	@rc=0; $(BUILD)/input_queue_drop_critical > $(BUILD)/input_queue_drop_critical.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: full raw ring admits button release' $(BUILD)/input_queue_drop_critical.log && \
	 grep -F 'FAIL: button release remains observable after overload' $(BUILD)/input_queue_drop_critical.log
	@rc=0; $(BUILD)/input_queue_ack_dropped_edge > $(BUILD)/input_queue_ack_dropped_edge.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: admitted release remains semantic under renewed overload' $(BUILD)/input_queue_ack_dropped_edge.log && \
	 grep -F 'FAIL: retried release remains observable after renewed overload' $(BUILD)/input_queue_ack_dropped_edge.log
$(BUILD)/evq_priority_negctl: tests/unit/evq_test.c c/kernel/gui/evq.c c/kernel/gui/evq.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -DEVQ_NEGCTL_DROP_SEMANTIC -o $@ \
	 tests/unit/evq_test.c c/kernel/gui/evq.c -Ic/kernel/gui -Iinclude/abi
test-evq-priority-negctl: $(BUILD)/evq_priority_negctl
	@rc=0; $< > $(BUILD)/evq_priority_negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL full ring admits button-up' $(BUILD)/evq_priority_negctl.log && \
	 grep -F 'FAIL button-up remains observable after overload' $(BUILD)/evq_priority_negctl.log
test-input-queue: test-input-queue-negctl test-evq-priority-negctl test-evq $(BUILD)/input_queue_test
	@$(BUILD)/input_queue_test

# Keep the IRQ-to-WM bound on the same CI path as the DOM delivery proof.
test-input-delivery: test-input-queue
