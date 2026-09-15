# IndexedDB upgrade store scope — 2026-09-13

The ordinary sequence `createObjectStore('drafts')` followed by
`openRequest.transaction.objectStore('drafts')` inside `onupgradeneeded` threw
NotFoundError. `beginUpgrade()` initialized the transaction's store-name list
from the old schema; schema edits updated the database's Map but left this
transaction list unchanged. On deletion, that same stale list could let lookup
reach a nonexistent record and throw TypeError instead of NotFoundError.

The captured Kimi baseline reports the store lookup error from a draft module
inside `beginUpgrade`. That stack is consistent with this defect. The fix is
established independently using invented records and ordinary upgrades; this
work did not fetch or execute any site module, inspect account state, access a
VM/disk, or prove that all Kimi errors share this cause.

## Change

`c/apps/browser/js_idb.c` now updates the active upgrade transaction's sorted
name list immediately after creating or deleting a store. Creation registers
the handle in the transaction's existing cache, so the creation return value
and subsequent transaction lookup refer to the same object. Deletion removes
the cached handle; recreating the name obtains a new one. Internal creation
does not invoke a page's public `objectStore()` wrapper.

The normal transaction scope check is unchanged. A store which exists in the
database but was not requested for a normal transaction still throws
NotFoundError. Unknown and deleted store names also remain errors.

This does not add IndexedDB durability, Worker/iframe installation, schema
rollback, store renaming, or a complete specification activity model. Existing
limitations in those independent areas are not claimed as repaired.

## Verification

All artifacts use `BUILD=build-ai-sites-idb`.

| Check | Result |
| --- | --- |
| New ordinary upgrade scope fixture | 27 checks, 0 failures |
| Same fixture under ASan/UBSan | 27 checks, 0 failures |
| Same final fixture against saved original `js_idb.c` | Exact 13 failures |
| Permanent `JS_IDB_STATIC_UPGRADE_SCOPE` control | Same exact 13 failures |
| Existing `test-idb` | 42 checks, 0 failures |
| Existing request-quiescence negative control | Detects its intentionally unscheduled get request |
| `test-mk-wired` | 299 fragments; 298 reachable, 1 declared |
| Independent x86_64-elf `js_idb.o` | Compiled successfully |

The new fixture covers initial and subsequent upgrades, sorted membership,
create/delete/recreate handle identity, writes through newly accessible store
handles, later reads after commit, normal transaction scope restrictions,
request settlement, and five transaction completion events. The negative
checker requires the exact 13 named failures and verifies that the normal
scope restrictions still pass. It is a prerequisite of the positive gate.

The initial saved-source run had 26 checks and 13 failures. A final ordinary
wrapper-observation check was then added; rerunning the actual saved source
produced 27 checks and the same exact 13 failures. Its log is
`build-ai-sites-idb/upgrade-original-final.log`; the initial log was preserved.
Final positive/control/sanitizer logs are in
`build-ai-sites-idb/idb-upgrade-scope/`; the combined build/regression log is
`build-ai-sites-idb/final-gates.log`. `final-source.sha256` records the product
and new test source hashes. No full browser image or real-site result is
claimed here; the root agent owns that integration and guest validation.

```sh
make -j3 BUILD=build-ai-sites-idb test-idb test-idb-upgrade-scope-san \
  test-mk-wired build-ai-sites-idb/jsobj/c/apps/browser/js_idb.o
```

New files: `tests/unit/idb_upgrade_scope_test.c`,
`tests/unit/idb_upgrade_scope_check.py`, and `tests/idb_upgrade_scope.mk`.
The Makefile adds that fragment immediately after `tests/idb.mk`. Other shared
Makefile edits and unrelated browser work were preserved.
