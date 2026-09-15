# One reachable integration gate for the September browser expansion. Each
# component target owns its observed-failing controls as prerequisites; this
# aggregate cannot accidentally turn them into documentation-only CI names.
.PHONY: test-browser-expansion
test-browser-expansion: test-css-extra-cascade test-generated-content \
 test-event-path test-rejection-checkpoint test-native-mo test-live-range \
 test-inert-focus test-modal-top-layer test-web-digest test-web-crypto-ops \
 test-storage-persistence test-storage-backend test-interaction-runtime \
 test-cssom-abi test-mk-wired

# The production consumers are part of the same acceptance command.
test-browser-expansion: test-browser-loading test-css-live-wiring \
 test-content-supports-wiring test-wasm-lifecycle test-cache-invalidation

test-browser-expansion: test-svg-layout-cache test-svg-dom-paint test-browser-wiring

test-browser-expansion: test-css-inline-extensions test-css-pseudo-skip test-bootstrap-scan

test-browser-expansion: test-traversal-work

test-browser-expansion: test-focus-style-flush test-simple-selector

# This wave gates native paint/input consumers, including watched disconnections.
test-browser-expansion: test-popover-top-layer test-waapi-paint test-text-wiring test-element-scroll

test-browser-expansion: test-max-height test-frame-bootstrap-wiring test-message-port
test-browser-expansion: test-dom-wrapper-lifetime
test-browser-expansion: test-address-geometry
test-browser-expansion: test-form-caret test-percentage-height test-custom-elements
test-browser-expansion: test-late-callbacks
test-browser-expansion: test-caret-advance test-opacity-group
test-browser-expansion: test-device-media
test-browser-expansion: test-display-contents
test-browser-expansion: test-grid-font-units
test-browser-expansion: test-form-selection-utf16
test-browser-expansion: test-module-budget
test-browser-expansion: test-grid-percentage-item
test-browser-expansion: test-form-control-container
test-browser-expansion: test-dom-matrix
test-browser-expansion: test-absolute-auto-height
test-browser-expansion: test-script-resource-events
test-browser-expansion: test-empty-resource
test-browser-expansion: test-navigation-base
test-browser-expansion: test-positioned-insets
test-browser-expansion: test-transparent-box-hit
test-browser-expansion: test-pointer-events test-inserted-script-async
test-browser-expansion: test-parser-script-events test-webapi-headers test-flex-column-bridge
test-browser-expansion: test-sock-closed-drain test-bfetch-owner-loop
