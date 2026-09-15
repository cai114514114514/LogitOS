# pointer-events auto/none, 2026-09-09

The transparent-box native hit gate exposed an unsupported property: both CSS.supports forms returned false for pointer-events:none, and a covering box remained the target. This is independent of the later Bing input-delay diagnosis: root's native replay attributed that delay to synchronous late-script waits, not a pointer overlay. No site name, URL or framework appears in this implementation.

[CSS UI 4 section 6.2](https://www.w3.org/TR/css-ui-4/#pointer-events-control) specifies initial auto, inheritance, and computed keyword preservation. None excludes the element's boxes from point targeting; an explicit auto descendant remains targetable. Keyboard focus eligibility and ancestor event propagation remain intact. SVG paint-specific keywords are outside this HTML implementation and continue to be refused.

## One producer and shared consumers

The property is appended to the real LibCSS property-name/parser/opcode/dispatch tables. The generated parser accepts auto, none and the engine's established CSS-wide keywords. Its inherited dispatch entry and compose function resolve inheritance, initial, unset, revert, specificity and importance through the existing cascade. This avoids a second extension scanner and avoids the extension @supports hook's name-only acceptance. css_extra receives no pointer-events producer.

The parser is generated from properties.gen with tools/gen_libcss.sh. The 2-bit computed field is declared in select_config.py and regenerated with select_generator.py, including all four generated outputs. Existing property opcodes and name indices remain unchanged; the new entry is appended. Internal packed bit positions can change, so all LibCSS objects must rebuild against the generated headers before the final guest link. The Makefile's existing header dependencies perform that rebuild.

css_engine copies the computed keyword into cstyle for native consumers and serializes it through the CSSP table. The existing parser-derived property enumeration exposes pointerEvents and pointer-events IDL accessors automatically. Both CSS.supports and stylesheet @supports consult the same real parser and reject unsupported values.

css_pointer_targetable reads only the candidate element's computed value; a text item uses its generating element. It does not search for a none ancestor, because that would erase an explicit auto descendant. Native hit and CSSOM elementFromPoint/elementsFromPoint share this decision. The latter also filters the ancestors it enumerates rather than reintroducing none ancestors after finding an auto child. Painting, keyboard focus and DOM event propagation do not call the filter.

## Evidence

The pre-edit executable produced 21 failures in 31 checks, including unsupported capability, missing computed readback, wrong native/CSSOM target and failed stylesheet @supports. The expanded final gate has 45 passing checks. Its permanent negative-control prerequisite leaves the real parser, inheritance and capability answers intact but bypasses the shared hit filter: it produces 10 observed failures, including native blank-area passthrough and inherited-text CSSOM targeting. This prevents a parser-only implementation from claiming success.

The existing transparent-box gate now takes its capability-positive branch, prints [true,true], and actually targets the underlying anchor through a pointer-events:none covering box. It passes 123 checks. New coverage includes multi-generation inheritance through display:contents, dynamic ancestor recascade, explicit auto recovery under opacity:0, important/inline precedence, invalid-value retention, keyboard focus, bubbling, and unchanged red-overlay painting. The initial property-table regression was its documented old assumption that the radius family were the final five entries; the gate now preserves those five and checks the sixth pointer-events append plus a functional parser anchor. Its deliberate adjacent-handler swap still fails.

Logs are under build/site-general/layout: pointer-events-before.log, pointer-events-final.log, pointer-events-regressions.log and pointer-events-regressions-final.log. tests/pointer_events.mk wires test-pointer-events with its negative control as a prerequisite. The native fixture tests/fixtures/engine-expansion/pointer-events.html reports POINTER-EVENTS PASS, input coordinates and explicit-auto child geometry; actual input and clicks print additional markers.

Final related checks pass: property table 18/18 with 153 live names; the generator reproduces all four committed outputs byte for byte; CSSOM 149/149; writer/reader ABI agreement; fixed projection 18/18; test-mk-wired 223 fragments, 222 reachable, one declared wrapper. Generated headers retain the generator's existing whitespace formatting instead of hand-edited output.

These are host cascade/paint/hit checks. Root owns final disk construction and guest native-input acceptance. No final guest result is claimed in this document. Full SVG pointer-events values, generic text selection policy, animations and any preexisting hit-test geometry approximations are not added by this property patch.


## Final guest correction

The preceding host-only boundary has now been closed for the native fixture by root's final disk. The final eleven-case run passes native typing through the none overlay, clicking the explicit-auto child and ancestor event bubbling. The screenshot visibly shows Python under the retained red border and the Auto child button. Browser/LibCSS sources did not drift during compilation and ISO/disk hashes did not change during the guest run. Compact proof is evidence/site-generalization-2026-09-09/pointer-events.png and native-results.json. This closes the fixture boundary, not full live-site compatibility.
