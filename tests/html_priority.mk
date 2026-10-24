# Guest controls replace only the actual producers in the shipping link.
# Keep full-browser source ownership separate from their host harnesses.
HTML_PRIORITY_DIR = $(BUILD)/html-priority
HTML_PRIORITY_BASE_LINK = $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(BROWSER_JS_OBJ) \
                          $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) \
                          $(BUILD)/browserobj/malloc_big.o
HTML_PRIORITY_DEPS = tests/qmp/qmp_html_priority.py tests/qmp/qmp_ui.py \
                     $(wildcard tests/fixtures/html-priority/*) $(BROWSER_INTERFACE_DEPS)

$(HTML_PRIORITY_DIR)/clone.o: c/apps/gui/browser/js/runtime/clone.c c/apps/gui/browser/js/runtime/clone.h
	@mkdir -p $(HTML_PRIORITY_DIR)
	$(COMPILE) $(BROWSER_JS_CF) -DJS_CLONE_KEEP_BUFFER -c $< -o $@
$(HTML_PRIORITY_DIR)/forms.o: c/apps/gui/browser/dom/validation.c c/apps/gui/browser/dom/validation.h
	@mkdir -p $(HTML_PRIORITY_DIR)
	$(COMPILE) $(UCFLAGS) $(CSS_INC) -Ic/apps/gui/browser -DFORM_VALIDATION_IGNORE_ERRORS -c $< -o $@
$(HTML_PRIORITY_DIR)/page.o: c/apps/gui/browser/js/runtime/page.c c/apps/gui/browser/js/runtime/timers.h
	@mkdir -p $(HTML_PRIORITY_DIR)
	$(COMPILE) $(BROWSER_JS_CF) -DJS_TIMER_NO_CLAMP -c $< -o $@
$(HTML_PRIORITY_DIR)/worker.o: c/apps/gui/browser/js/workers/worker.c c/apps/gui/browser/js/runtime/timers.h
	@mkdir -p $(HTML_PRIORITY_DIR)
	$(COMPILE) $(BROWSER_JS_CF) -DJS_TIMER_NO_CLAMP -c $< -o $@
$(HTML_PRIORITY_DIR)/navigation.o: c/apps/gui/browser/app/main.c $(BROWSER_INTERFACE_DEPS)
	@mkdir -p $(HTML_PRIORITY_DIR)
# main.c consumes JS interfaces and belongs to BROWSER_JS_OBJ (jsobj), not
# the browserobj rendering group. Use its actual configuration domain too.
	$(COMPILE) $(BROWSER_JS_CF) -DBROWSER_FORM_GET_TRUNCATE -c $< -o $@
$(HTML_PRIORITY_DIR)/url.o: c/net/http/url.c c/net/http/url.h
	@mkdir -p $(HTML_PRIORITY_DIR)
	$(COMPILE) $(UCFLAGS) -DHTTP_URL_TRUNCATE -c $< -o $@

HTML_PRIORITY_CLONE_LINK = $(filter-out $(BUILD)/jsobj/c/apps/gui/browser/js/runtime/clone.o,$(HTML_PRIORITY_BASE_LINK)) $(HTML_PRIORITY_DIR)/clone.o
HTML_PRIORITY_FORMS_LINK = $(filter-out $(BUILD)/browserobj/c/apps/gui/browser/dom/validation.o,$(HTML_PRIORITY_BASE_LINK)) $(HTML_PRIORITY_DIR)/forms.o
HTML_PRIORITY_TIMERS_LINK = $(filter-out $(BUILD)/jsobj/c/apps/gui/browser/js/runtime/page.o $(BUILD)/jsobj/c/apps/gui/browser/js/workers/worker.o,$(HTML_PRIORITY_BASE_LINK)) $(HTML_PRIORITY_DIR)/page.o $(HTML_PRIORITY_DIR)/worker.o
HTML_PRIORITY_NAVIGATION_LINK = $(filter-out $(BUILD)/jsobj/c/apps/gui/browser/app/main.o $(BUILD)/browserobj/c/net/http/url.o,$(HTML_PRIORITY_BASE_LINK)) $(HTML_PRIORITY_DIR)/navigation.o $(HTML_PRIORITY_DIR)/url.o

$(HTML_PRIORITY_DIR)/clone.elf: $(HTML_PRIORITY_CLONE_LINK)
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(HTML_PRIORITY_CLONE_LINK) --end-group
$(HTML_PRIORITY_DIR)/forms.elf: $(HTML_PRIORITY_FORMS_LINK)
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(HTML_PRIORITY_FORMS_LINK) --end-group
$(HTML_PRIORITY_DIR)/timers.elf: $(HTML_PRIORITY_TIMERS_LINK)
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(HTML_PRIORITY_TIMERS_LINK) --end-group
$(HTML_PRIORITY_DIR)/navigation.elf: $(HTML_PRIORITY_NAVIGATION_LINK)
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(HTML_PRIORITY_NAVIGATION_LINK) --end-group
$(HTML_PRIORITY_DIR)/%.aex: $(HTML_PRIORITY_DIR)/%.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ Browser - 'B' 120 130 240 --stack-pages 2048

.PHONY: test-html-priority-guest test-html-priority-guest-clone test-html-priority-guest-forms test-html-priority-guest-timers test-html-priority-guest-navigation
test-html-priority-guest-clone: test-html-clone-negctl $(ISO) $(DISK) $(HTML_PRIORITY_DIR)/clone.aex $(HTML_PRIORITY_DEPS)
	$(MAKE) BUILD=$(BUILD) DISK=$(HTML_PRIORITY_DIR)/clone.img BROWSER_AEX=$(HTML_PRIORITY_DIR)/clone.aex $(HTML_PRIORITY_DIR)/clone.img
	python3 tests/qmp/qmp_html_priority.py $(ISO) $(HTML_PRIORITY_DIR)/clone.img --negative clone --out $(HTML_PRIORITY_DIR)/guest-clone-$$(date +%s)
test-html-priority-guest-forms: test-html-forms-negctl $(ISO) $(DISK) $(HTML_PRIORITY_DIR)/forms.aex $(HTML_PRIORITY_DEPS)
	$(MAKE) BUILD=$(BUILD) DISK=$(HTML_PRIORITY_DIR)/forms.img BROWSER_AEX=$(HTML_PRIORITY_DIR)/forms.aex $(HTML_PRIORITY_DIR)/forms.img
	python3 tests/qmp/qmp_html_priority.py $(ISO) $(HTML_PRIORITY_DIR)/forms.img --negative forms --out $(HTML_PRIORITY_DIR)/guest-forms-$$(date +%s)
test-html-priority-guest-timers: test-html-timers-negctl $(ISO) $(DISK) $(HTML_PRIORITY_DIR)/timers.aex $(HTML_PRIORITY_DEPS)
	$(MAKE) BUILD=$(BUILD) DISK=$(HTML_PRIORITY_DIR)/timers.img BROWSER_AEX=$(HTML_PRIORITY_DIR)/timers.aex $(HTML_PRIORITY_DIR)/timers.img
	python3 tests/qmp/qmp_html_priority.py $(ISO) $(HTML_PRIORITY_DIR)/timers.img --negative timers --out $(HTML_PRIORITY_DIR)/guest-timers-$$(date +%s)
# Serialize guest runs even under make -j: each is a 1 GiB machine, and host
# contention must not manufacture timeouts in the timer acceptance apparatus.
test-html-priority-guest-forms: test-html-priority-guest-clone
test-html-priority-guest-timers: test-html-priority-guest-forms
test-html-priority-guest-navigation: test-html-priority-guest-timers test-http-url-negctl $(HTML_PRIORITY_DIR)/navigation.aex
	$(MAKE) BUILD=$(BUILD) DISK=$(HTML_PRIORITY_DIR)/navigation.img BROWSER_AEX=$(HTML_PRIORITY_DIR)/navigation.aex $(HTML_PRIORITY_DIR)/navigation.img
	python3 tests/qmp/qmp_html_priority.py $(ISO) $(HTML_PRIORITY_DIR)/navigation.img --negative navigation --with-navigation --out $(HTML_PRIORITY_DIR)/guest-navigation-$$(date +%s)
test-html-priority-guest: test-html-priority-guest-navigation test-html-streams-negctl
	python3 tests/qmp/qmp_html_priority.py $(ISO) $(DISK) --with-navigation --out $(HTML_PRIORITY_DIR)/guest-current-$$(date +%s)
ci-boot: test-html-priority-guest
