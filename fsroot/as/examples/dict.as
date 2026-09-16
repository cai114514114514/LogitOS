# aether: 3.0
# The literal infers Dict[str, i64]; later inserts keep those key/value types.

def main() -> None:
    d = {"one": 1, "two": 2}
    d["three"] = 3
    print("len =", len(d))
    print("two =", d["two"])
    print("has three:", d.has("three"))

    total = 0
    for key in d:
        total += d[key]
    print("sum =", total)
    print("contains one:", "one" in d)
