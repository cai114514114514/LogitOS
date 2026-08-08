# tests/frameworks.mk -- the framework corpus: which JS framework runtimes this
# browser cannot run, and WHY, ranked by cause across all of them.
#
# In its own fragment, like tests/webapi_platform.mk and tests/nic.mk, for the
# reason those give: several lines edit the top-level Makefile at once and a
# whole-file overwrite deletes targets written straight into it. Only the single
# `-include tests/frameworks.mk` line lives in the Makefile.
#
# WHAT THIS MEASURES THAT tests/fixtures/webapi DOES NOT
# That corpus is captured real sites, so a failure there is entangled with the
# site's own bugs, its A/B bucket and its CDN. This one is seven applications
# built here, from the frameworks' own default toolchains, each minimal and each
# exercising the same three things: component state, a click handler, and a
# lazily imported route so the bundler's chunk-loading runtime actually runs.
# Nothing in them is broken, which is what makes an exception here attributable.
#
# THE UNIT OF THE OUTPUT IS THE CAUSE, COUNTED IN FRAMEWORKS.
# "N of 7 die on X" is a work order. "angular throws 3" is a symptom with no
# address on it. See the header of tests/unit/framework_rank.py.
#
#   make probe-frameworks          the ranked cause table (report; never fails)
#   make test-frameworks           the same, asserted against BASELINE
#   make probe-frameworks-chrome   re-run the Chrome differential and fold it in
#
.PHONY: probe-frameworks test-frameworks probe-frameworks-chrome

FW_CORPUS  := tests/fixtures/frameworks
FW_RANK    := tests/unit/framework_rank.py
# `_`-prefixed directories are excluded: `_platform` is a diagnostic reduction
# of the seven bundles' throw sites, not an eighth application, and the Chrome
# differential would report it as one.
FW_DIRS    := $(filter-out $(FW_CORPUS)/_%,\
                $(sort $(patsubst %/,%,$(dir $(wildcard $(FW_CORPUS)/*/index.html)))))
# The Chrome differential's own output, kept as a file so the ranked table can
# fold it in without Chrome being present. It is COMMITTED for the same reason
# the fixtures are: it is the evidence the numbers were subtracted against, and
# a differential re-run on a different Chrome is a different measurement.
FW_CHROME  := $(FW_CORPUS)/CHROMEDIFF
FW_BASE    := $(FW_CORPUS)/BASELINE

# --- probe-frameworks: the table -------------------------------------------
# Depends on $(BUILD)/webapi_probe from tests/webapi_platform.mk. Deliberately
# the SAME probe binary the webapi corpus is measured with, not a second one:
# two instruments measuring the same thing is how you get two answers.
probe-frameworks: $(BUILD)/webapi_probe
	@python3 $(FW_RANK) $(BUILD)/webapi_probe $(FW_CORPUS) --chromediff $(FW_CHROME)

# --- test-frameworks: the assertion ----------------------------------------
# Host, no network, no QEMU, no Chrome -- so tools/audit_tests.py classifies it
# `host` and tools/ci.sh picks it up automatically. It pins two things:
#
#   * uncaught exceptions per framework, and the cause each is attributed to
#   * every platform feature the corpus dies on, from the `_platform` fixture
#
# It is a CHANGE DETECTOR, not a wish list. Everything in the baseline is the
# measured state, so the target is green today and goes red the moment a Web API
# lands -- which is exactly the acceptance check webapi_probe.c's header asks
# for: "when a name from channel 1 is implemented, the channel-2 error for that
# page must change, and if it does not, the implementation did not matter."
# When it fires, update BASELINE in the same commit and say which cause moved.
test-frameworks: $(BUILD)/webapi_probe
	@python3 $(FW_RANK) $(BUILD)/webapi_probe $(FW_CORPUS) \
	    --chromediff $(FW_CHROME) --baseline $(FW_BASE)

# --- probe-frameworks-chrome: re-take the differential ----------------------
# NOT a `test-` target, on purpose. It needs headless Chrome, node and openssl,
# none of which exist in the WSL image this repository builds in, so wiring it
# into `ci` would make the aggregate fail on a missing browser rather than on a
# broken one. Run it by hand when the corpus or the engine changes, and commit
# the refreshed $(FW_CHROME).
#
# The harness is tests/chrome/webapi_chromediff.mjs -- the JS-exception line's,
# used as-is rather than reimplemented. A throwaway --user-data-dir under the OS
# temp directory is created and deleted per run: no profile, no history.
probe-frameworks-chrome: $(BUILD)/webapi_probe
	@$(BUILD)/webapi_probe --errors --json $(FW_DIRS) > $(BUILD)/fw_probe.json 2>&1 || true
	@node tests/chrome/webapi_chromediff.mjs --probe $(BUILD)/fw_probe.json \
	    --wait 6000 $(FW_DIRS) | tee $(FW_CHROME)
	@echo "-- refreshed $(FW_CHROME); commit it with the numbers it produced"
