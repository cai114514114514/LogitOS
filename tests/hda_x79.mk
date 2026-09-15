# X79 onboard HDA selection and failed-probe ownership gate.  This fragment is
# standalone so it can be included by the physical-driver aggregate without
# editing the contended top-level Makefile.
.PHONY: test-hda-x79-negctl test-hda-x79-host

test-hda-x79-negctl:
	@python3 tests/unit/hda_x79_run.py --build $(BUILD)/hda-x79-neg --negative-only

test-hda-x79-host: test-hda-x79-negctl
	@python3 tests/unit/hda_x79_run.py --build $(BUILD)/hda-x79
