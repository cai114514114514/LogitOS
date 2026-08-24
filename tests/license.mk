# tools/license_audit.py, wired. See that file's own docstring for what the
# four checks are; this fragment is only the CI wiring and the negative
# control, in the shape every other fragment in this tree uses (test-cookie-
# jar-negctl above all): the control is a PREREQUISITE of the positive, which
# is what makes "a control is run by its positive counterpart" -- the
# assumption tools/audit_tests.py's NOT_CI makes about every `test-*-negctl`
# -- actually true here instead of merely asserted.
#
# Four collapses, one per check, each restored to the tree unconditionally
# (the restore runs even when the assertion that follows it fails) and each
# checked two ways: the checker must exit nonzero, AND its message must name
# the specific thing that broke -- "the checker failed" is not the property
# under test, "it failed on the thing I broke, in those words" is.

.PHONY: test-license-audit test-license-audit-negctl

ci-host: test-license-audit
test-license-audit: test-license-audit-negctl

test-license-audit:
	@python3 tools/license_audit.py

test-license-audit-negctl:
	@mkdir -p $(BUILD)
	@rc=0; \
	echo "-- (a) a third_party license file moved aside --"; \
	mv third_party/quickjs/LICENSE $(BUILD)/license_negctl_a; \
	out=`python3 tools/license_audit.py --check third_party 2>&1`; ec=$$?; \
	mv $(BUILD)/license_negctl_a third_party/quickjs/LICENSE; \
	case "$$ec:$$out" in \
	  0:*) echo "  BAD: control (a) exited 0 (should have failed)"; rc=1;; \
	  *"third_party/quickjs has no license file beside it"*) \
	    echo "  OK: `printf '%s' "$$out" | grep '^license-audit: FAIL'`";; \
	  *) echo "  BAD: control (a) failed but did not name quickjs's missing license file: $$out"; rc=1;; \
	esac; \
	echo "-- (b) a row deleted from THIRD_PARTY.md --"; \
	cp THIRD_PARTY.md $(BUILD)/license_negctl_b; \
	sed -i '/^| QuickJS |/d' THIRD_PARTY.md; \
	out=`python3 tools/license_audit.py --check third_party 2>&1`; ec=$$?; \
	cp $(BUILD)/license_negctl_b THIRD_PARTY.md; \
	case "$$ec:$$out" in \
	  0:*) echo "  BAD: control (b) exited 0 (should have failed)"; rc=1;; \
	  *"THIRD_PARTY.md does not name third_party/quickjs/"*) \
	    echo "  OK: `printf '%s' "$$out" | grep '^license-audit: FAIL'`";; \
	  *) echo "  BAD: control (b) failed but did not name the missing quickjs row: $$out"; rc=1;; \
	esac; \
	echo "-- (c) a fixture directory's PROVENANCE.md moved aside --"; \
	mv tests/fixtures/browser/PROVENANCE.md $(BUILD)/license_negctl_c; \
	out=`python3 tools/license_audit.py --check fixtures 2>&1`; ec=$$?; \
	mv $(BUILD)/license_negctl_c tests/fixtures/browser/PROVENANCE.md; \
	case "$$ec:$$out" in \
	  0:*) echo "  BAD: control (c) exited 0 (should have failed)"; rc=1;; \
	  *"tests/fixtures/browser/ has no PROVENANCE.md"*) \
	    echo "  OK: `printf '%s' "$$out" | grep '^license-audit: FAIL'`";; \
	  *) echo "  BAD: control (c) failed but did not name browser's missing PROVENANCE.md: $$out"; rc=1;; \
	esac; \
	echo "-- (d) a RELEASE_NOTICES line removed --"; \
	cp Makefile $(BUILD)/license_negctl_d; \
	sed -i 's/^RELEASE_NOTICES := LICENSE LICENSING\.md \\/RELEASE_NOTICES := LICENSE \\/' Makefile; \
	out=`python3 tools/license_audit.py --check disk 2>&1`; ec=$$?; \
	cp $(BUILD)/license_negctl_d Makefile; \
	case "$$ec:$$out" in \
	  0:*) echo "  BAD: control (d) exited 0 (should have failed)"; rc=1;; \
	  *"LICENSING.md is not listed in RELEASE_NOTICES"*) \
	    echo "  OK: `printf '%s' "$$out" | grep '^license-audit: FAIL'`";; \
	  *) echo "  BAD: control (d) failed but did not name LICENSING.md/RELEASE_NOTICES: $$out"; rc=1;; \
	esac; \
	if [ $$rc -ne 0 ]; then echo "test-license-audit-negctl: FAIL"; exit 1; fi; \
	echo "test-license-audit-negctl: OK -- four collapses, each naming the right thing"
