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
	@: 'Each collapse happens in a SHADOW of the tree (tools/shadow_tree.py),' ; \
	: 'never in the tree: the old in-place sabotage + restore raced itself' ; \
	: 'under the parallel sweep and left a truncated Makefile behind.' ; \
	rc=0; SH=$(abspath $(BUILD))/license_shadow; \
	echo "-- (a) a third_party license file moved aside --"; \
	python3 tools/shadow_tree.py . $$SH --drop third_party/quickjs/LICENSE || exit 1; \
	out=`LICENSE_AUDIT_ROOT=$$SH python3 tools/license_audit.py --check third_party 2>&1`; ec=$$?; \
	case "$$ec:$$out" in \
	  0:*) echo "  BAD: control (a) exited 0 (should have failed)"; rc=1;; \
	  *"third_party/quickjs has no license file beside it"*) \
	    echo "  OK: `printf '%s' "$$out" | grep '^license-audit: FAIL'`";; \
	  *) echo "  BAD: control (a) failed but did not name quickjs's missing license file: $$out"; rc=1;; \
	esac; \
	echo "-- (b) a row deleted from THIRD_PARTY.md --"; \
	python3 tools/shadow_tree.py . $$SH --copy THIRD_PARTY.md || exit 1; \
	: 'sed reads the original and writes the shadow copy -- no -i: BSD sed' ; \
	: 'reads `-i SCRIPT` as a backup suffix, and the sabotage never happens.' ; \
	sed '/^| QuickJS |/d' THIRD_PARTY.md > $$SH/THIRD_PARTY.md; \
	out=`LICENSE_AUDIT_ROOT=$$SH python3 tools/license_audit.py --check third_party 2>&1`; ec=$$?; \
	case "$$ec:$$out" in \
	  0:*) echo "  BAD: control (b) exited 0 (should have failed)"; rc=1;; \
	  *"THIRD_PARTY.md does not name third_party/quickjs/"*) \
	    echo "  OK: `printf '%s' "$$out" | grep '^license-audit: FAIL'`";; \
	  *) echo "  BAD: control (b) failed but did not name the missing quickjs row: $$out"; rc=1;; \
	esac; \
	echo "-- (c) a fixture directory's PROVENANCE.md moved aside --"; \
	python3 tools/shadow_tree.py . $$SH --drop tests/fixtures/browser/PROVENANCE.md || exit 1; \
	out=`LICENSE_AUDIT_ROOT=$$SH python3 tools/license_audit.py --check fixtures 2>&1`; ec=$$?; \
	case "$$ec:$$out" in \
	  0:*) echo "  BAD: control (c) exited 0 (should have failed)"; rc=1;; \
	  *"tests/fixtures/browser/ has no PROVENANCE.md"*) \
	    echo "  OK: `printf '%s' "$$out" | grep '^license-audit: FAIL'`";; \
	  *) echo "  BAD: control (c) failed but did not name browser's missing PROVENANCE.md: $$out"; rc=1;; \
	esac; \
	echo "-- (d) a RELEASE_NOTICES line removed --"; \
	python3 tools/shadow_tree.py . $$SH --copy Makefile || exit 1; \
	sed 's/^RELEASE_NOTICES := LICENSE LICENSING\.md \\/RELEASE_NOTICES := LICENSE \\/' Makefile > $$SH/Makefile; \
	out=`LICENSE_AUDIT_ROOT=$$SH python3 tools/license_audit.py --check disk 2>&1`; ec=$$?; \
	case "$$ec:$$out" in \
	  0:*) echo "  BAD: control (d) exited 0 (should have failed)"; rc=1;; \
	  *"LICENSING.md is not listed in RELEASE_NOTICES"*) \
	    echo "  OK: `printf '%s' "$$out" | grep '^license-audit: FAIL'`";; \
	  *) echo "  BAD: control (d) failed but did not name LICENSING.md/RELEASE_NOTICES: $$out"; rc=1;; \
	esac; \
	rm -rf $$SH; \
	if [ $$rc -ne 0 ]; then echo "test-license-audit-negctl: FAIL"; exit 1; fi; \
	echo "test-license-audit-negctl: OK -- four collapses, each naming the right thing"
