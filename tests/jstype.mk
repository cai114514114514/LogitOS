# ===================== the type differential ================================
#
# WHAT IT MEASURES. For every own property of every interface prototype this
# browser publishes, the property's KIND -- function, accessor, or the typeof of
# its data value -- taken IN THE GUEST by the real browser.aex from an ordinary
# page. Not presence: TYPE.
#
# WHY IT EXISTS. This tree has paid four times for one name on one interface,
# and each time the whole line behind it died silently:
#
#   Node.isEqualNode ABSENT            -> React declared hydration lost, switched
#                                         to client rendering, the client render
#                                         died; stripe 69 painted text runs -> 0
#   document.currentScript NULL        -> every webpack/turbopack chunk loader
#   Element.insertAdjacentElement ABS. -> bilibili's player never reached MSE
#   ...and a property with the WRONG TYPE, which is strictly worse.
#
# Absence is survivable. tests/fixtures/jsperf/baidu-async-search.js, committed:
#
#     var o = a.getContext === i ? !1 : a.getContext("2d");
#     if (o === !1) return !1;
#
# With the method absent that guard fires and the page takes its fallback. With
# a present member of the wrong type the guard does not fire and the page walks
# on holding the wrong thing -- or calls it and gets
# `TypeError: <name> is not a function (it is the number N)`.
#
# WHY THERE IS NO REFERENCE BROWSER HERE, having gone and looked first:
#   tests/fixtures/frameworks/CHROMEDIFF is an EXCEPTION differential and holds
#     no type information at all -- it was the obvious oracle and is not one.
#   build/wpt has no interfaces/ directory, so there are no .idl files locally.
#   node shares the JS builtins and has no DOM.
# Rather than invent a fifth oracle, the census reports only findings that are
# wrong in EVERY browser (see the header of tests/fixtures/jstype/typecensus.html
# for the three rules and the one argued ECMAScript exception).
#
# THE GATE IS A RATCHET, NOT A COUNT. tests/fixtures/jstype/BASELINE is the debt;
# a finding that is not in it fails the build; a baseline line that stops firing
# is progress and must be deleted in the commit that earns it.
#
# THE CONTROL IS A PREREQUISITE, NOT A SIBLING. `test-jstype: test-jstype-negctl`
# is one line and it is the difference between a control that runs and one of
# the 61 stranded ones; naming it on a ci-host: line satisfies the audit and
# still runs it never, which is worse because it looks fixed.
#
# ONE BOOT PER TARGET. Both targets boot QEMU, so this is a ci-boot gate.

.PHONY: test-jstype test-jstype-negctl test-jstype-write

# The census, against the committed baseline. Its own control runs first.
test-jstype: test-jstype-negctl $(ISO) $(DISK)
	python3 tests/qmp/jstype_check.py --iso $(ISO) --disk $(DISK)

# THE CONTROL, AND IT IS WATCHED FAILING RATHER THAN ASSERTED. `?negctl` makes
# the census page install three members on Element.prototype before it runs --
# one of each shape the three rules exist to catch -- and this target fails
# unless the census names all three. It is the same fixture and the same forty
# lines of census script as the positive run (one jar, one door), so a census
# that has quietly stopped working cannot report "0 findings" and be believed.
test-jstype-negctl: $(ISO) $(DISK)
	python3 tests/qmp/jstype_check.py --iso $(ISO) --disk $(DISK) --negctl

# Re-cut the baseline. Deliberately not a dependency of anything: a baseline
# that regenerates itself records whatever the build does today and can never
# go red.
test-jstype-write: $(ISO) $(DISK)
	python3 tests/qmp/jstype_check.py --iso $(ISO) --disk $(DISK) --write

ci-boot: test-jstype
