# pointer-events:none: independent unsupported capability

Read-only observation from the new transparent-box-hit host gate, 2026-09-09. A painted positioned overlay with pointer-events:none still receives the real browser_hittest_node_scroll hit instead of its underlying link; document.elementFromPoint agrees with that current behavior. The first full test preserves all three failed assertions in build/site-general/transparent-box-hit/current-before.log and legacy-unscoped-before.log. This is a distinct property implementation gap, not evidence against emitting transparent hit boxes.

Capability honesty was measured through the actual installed CSSOM runtime:

    CSS.supports('pointer-events','none')       // false
    CSS.supports('(pointer-events:none)')      // false

Both answers are false. The scoped gate now prints UNSUPPORTED, including the actual native target (`cover`) and empty href, without counting this consumer behavior as a pass. If the capability queries later report true, the same gate runs the strict native target/href/CSSOM assertions again. An exception or disagreement in the capability answers fails the gate. Ordinary transparent overlay interception remains an unconditional requirement.

No production property chain is being added in this task. A future implementation must add the computed value and its inheritance/default semantics, parser/serialization/property exposure, an honest supports producer, and the trusted/CSSOM hit consumers together. Re-parsing raw style attributes in the hit loop would miss stylesheet cascade and inherited values; making supports return true before those consumers land would turn this honest refusal into a false capability claim. HTML auto/none and SVG's extended values need their own stated scope. There is intentionally no source patch pretending this is one missing boolean check.
