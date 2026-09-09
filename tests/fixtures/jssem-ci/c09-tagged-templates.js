// Tagged templates. lit-html's entire caching strategy is "the strings array
// from one call site is the SAME OBJECT every time, so I can use it as a
// WeakMap key and parse the HTML once". If that identity does not hold, lit
// re-parses and re-creates the template on every single render, and every
// part-position it cached is invalidated -- it does not crash, it just does
// O(template) work per frame forever. That makes this the highest-value case
// in the classes-iteration set.
'use strict';

function tag(strings, ...vals) {
  return { strings, vals, raw: strings.raw };
}

function render(x) { return tag`a${x}b`; }

const r1 = render(1);
const r2 = render(2);

print('IDENTITY strings ' + (r1.strings === r2.strings));
print('IDENTITY raw ' + (r1.raw === r2.raw));
print('vals differ ' + (r1.vals[0] !== r2.vals[0]));

print('frozen ' + Object.isFrozen(r1.strings));
print('raw frozen ' + Object.isFrozen(r1.raw));
print('cooked ' + JSON.stringify(r1.strings) + ' len ' + r1.strings.length);
print('is array ' + Array.isArray(r1.strings) + ' ' + Array.isArray(r1.raw));

// a DIFFERENT call site must give a DIFFERENT object even with identical text
function render2(x) { return tag`a${x}b`; }
print('other site differs ' + (render(3).strings !== render2(3).strings));

// writing to a frozen strings array is a silent no-op in sloppy mode and a
// TypeError in strict; this file is strict.
try { r1.strings[0] = 'MUT'; print('MUTATED'); } catch (e) { print('mutate throws ' + e.constructor.name); }
try { r1.strings.push('x'); print('PUSHED'); } catch (e) { print('push throws ' + e.constructor.name); }
print('still ' + r1.strings[0]);

// raw vs cooked
const esc = tag`line\nnextA\t${0}end`;
print('cooked0 ' + JSON.stringify(esc.strings[0]));
print('raw0 ' + JSON.stringify(esc.raw[0]));

// an INVALID escape is a SyntaxError in an untagged template and produces
// `undefined` cooked with a live raw in a tagged one. Every CSS-in-JS library
// that writes \d or a Windows path in a template relies on this.
const bad = tag`\unicode and \xZZ ${1} \01`;
print('bad cooked ' + JSON.stringify(bad.strings[0]) + ' ' + JSON.stringify(bad.strings[1]));
print('bad raw ' + JSON.stringify(bad.raw[0]) + ' ' + JSON.stringify(bad.raw[1]));

// String.raw
print('String.raw ' + String.raw`a\nb${1}c\td`);

// nested templates, and a template as a tag argument
const inner = `IN${1 + 1}`;
print('nested ' + tag`x${`y${inner}z`}w`.vals[0]);

// member-expression tag, and a tag on a call result
const holder = { t: tag };
print('member tag ' + JSON.stringify(holder.t`m${1}n`.strings));
print('this ' + (function () { return this === holder; }).call(holder));

// the template object has no `raw` on raw itself, and is a plain array-like
print('raw of raw ' + (r1.raw.raw === undefined));
print('proto ' + (Object.getPrototypeOf(r1.strings) === Array.prototype));
print('desc ' + JSON.stringify(Object.getOwnPropertyDescriptor(r1.strings, 'raw')));
