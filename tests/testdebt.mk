# tests/testdebt.mk -- the honesty package's wiring (2026-08-30).
#
# WHY THIS FILE EXISTS. Four browser-critical gates are DEFINED in the root
# Makefile (test-live-page, test-css-fidelity, test-webapi-page at the
# "on-device proof" block, test-dom-bindings beside the js_dom host tests).
# That file is shared by every agent in this workspace and is not edited by
# this package, so their ci lines land HERE instead: `ci-boot:` and
# `ci-host:` prerequisites accumulate across every definition in the graph,
# and a line in this fragment is indistinguishable to make (and to
# tools/audit_tests.py's reachability walk) from one beside the recipe.
#
# What these four are: the gates that prove the browser's CSS, its fetch()
# pipeline, a live page's event loop and the JS-DOM layout contract ON THE
# MACHINE. Each sat in tests/audit-unwired.baseline -- reachable by nobody,
# reading like coverage -- which is the debt this package exists to retire.
# They have all been booted in-session at least once on the day they were
# wired; a wired gate that has never run is the rot this file removes.

ci-boot: test-live-page test-css-fidelity test-webapi-page

# test-dom-bindings is a HOST gate (it links libcss_host.a and the real
# layout against the real QuickJS), so it belongs on ci-host, not ci-boot.
ci-host: test-dom-bindings
