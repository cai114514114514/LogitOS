// The synchronous sibling, as a control on m33: a plain for-of break.
function* sgen() { try { yield 1; yield 2; } finally { print('SYNC-CLEANUP'); } }
for (const v of sgen()) break;
print('AFTER');
