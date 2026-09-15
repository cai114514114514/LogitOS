"""Require the exact ordinary-upgrade failures of the initial-scope control."""
import pathlib
import sys

lines = pathlib.Path(sys.argv[1]).read_text().splitlines()
expected = {
    "created store immediately joins upgrade transaction scope",
    "created store is found through the upgrade transaction",
    "createObjectStore and transaction lookup share one handle",
    "upgrade scope is sorted after two store creations",
    "database and upgrade transaction expose the same current names",
    "write through upgrade lookup survives into a later transaction",
    "deleting a store removes it from upgrade scope immediately",
    "lookup of a deleted store still throws NotFoundError",
    "recreated store shares its handle with transaction lookup",
    "additional store joins a nonempty upgrade scope",
    "delete and recreate leave a sorted current scope",
    "new store in a later upgrade is immediately addressable",
    "later upgrade lookup writes are readable after commit",
}
actual = {line.removeprefix("FAIL: ") for line in lines if line.startswith("FAIL: ")}
assert actual == expected, f"unexpected failure set: missing={expected-actual}, extra={actual-expected}"
assert "idb-upgrade-scope: 27 checks, 13 failures" in lines
assert not any("[js exception]" in line for line in lines)
assert "ok  : existing store outside a normal transaction still throws NotFoundError" in lines
assert "ok  : an unknown upgrade store still throws NotFoundError" in lines
print("idb-upgrade-scope control: exact 13 failures; normal scope restrictions preserved")
