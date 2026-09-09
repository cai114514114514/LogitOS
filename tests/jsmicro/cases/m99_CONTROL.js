// THE CONTROL. This program is deliberately made to print a DIFFERENT order on
// the two engines, by keying on something that is genuinely different: node has
// a native queueMicrotask, this tree's browser has a Promise-based polyfill
// installed by js_platform.c, and only one of the two is a function whose
// source text contains "Promise".
// If this case does not appear as a DIFF, the harness is not an instrument.
print(String(queueMicrotask).indexOf('Promise') >= 0 ? 'polyfilled' : 'native');
