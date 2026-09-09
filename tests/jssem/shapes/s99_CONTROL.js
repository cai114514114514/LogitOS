// THE CONTROL for tests/jssem/shapes (CLAUDE.md rule 5: a control that cannot
// be watched failing is worse than no control, because it reads like one).
//
// This case prints a value that is DIFFERENT under node and under this engine
// BY CONSTRUCTION, so a run in which it comes back SAME is a run in which the
// harness compared nothing -- run-shapes.sh checks for exactly that and fails.
//
// The discriminator is the engine's own version string rather than a planted
// variable, so it needs no cooperation from the runner and cannot be
// accidentally made to agree by an edit to the harness.
print('engine =', (typeof process === 'object' && process && process.versions &&
                   process.versions.node) ? 'node' : 'not-node');

// Three neighbouring rows that MUST still agree. This is what separates "the
// control fired" from "everything differs", which is the failure mode that
// makes a control useless in the other direction -- if the harness were
// comparing two unrelated files, this line would differ too.
print('control-sanity-1 =', 1 + 1);
print('control-sanity-2 =', typeof Promise);
print('control-sanity-3 =', [3, 1, 2].sort().join(''));
