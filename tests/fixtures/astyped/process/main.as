# aether: 3.0

# Module initialization sees the same launch arguments as main. No compiler
# options belong in this list; a second call returns an independent container.
startup = args()


def garbage() -> None:
    for index in range(300):
        values = [index, index + 1]
        text = "garbage " + str(index)


def inspect() -> None:
    values = args()
    assert len(values) == 5
    assert len(values[0]) > 0
    assert values[1] == "中文"
    assert values[2] == "two words"
    assert values[3] == ""
    assert values[4] == "--json"
    assert startup[1] == values[1]
    values[1] = "changed"
    assert args()[1] == "中文" and startup[1] == "中文"
    gc_collect()
    assert values[2] == "two words"


def count_allocations() -> None:
    gc_collect()
    before = gc_stats()
    bytes_before = gc_live_bytes()
    values: List[i64] = []
    assert gc_stats() == before + 1
    assert gc_live_bytes() > bytes_before + 1
    values.append(42)
    # List identity and backing storage are separate native GC allocations.
    assert gc_stats() == before + 2


def main() -> None:
    inspect()
    count_allocations()
    gc_collect()
    baseline = gc_stats()
    garbage()
    before = gc_stats()
    reclaimed = gc()
    after = gc_stats()
    assert reclaimed > 0 and reclaimed == before - after
    assert after == baseline
    assert gc() == 0
    print("native arguments and collection counts ok")
