// queueMicrotask calling queueMicrotask, 2000 deep. The polyfill adds a
// promise per level; a scheduler that drains this way must not grow a stack.
var n = 0;
function step() { if (++n < 2000) queueMicrotask(step); else print('depth', n); }
queueMicrotask(step);
