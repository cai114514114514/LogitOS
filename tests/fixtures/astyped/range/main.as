# aether: 3.0

def descending() -> Range:
    return range(7, -2, -3)


def sum_range(values: Range) -> i64:
    total = 0
    for value in values:
        total += value
        gc_collect()
    return total


def strings() -> None:
    text = "A中文" + "\0Z"
    rebuilt = ""
    pieces: List[str] = []
    for byte in text:
        # A3 retains byte-oriented text iteration, matching indexing/ord.
        assert len(byte) == 1
        pieces.append(byte)
        rebuilt += byte
        text = "rebound"
        gc_collect()
    assert rebuilt == "A中文\0Z"
    assert "".join(pieces) == rebuilt
    last = ""
    for byte in "empty " + "owner":
        last = byte
    gc_collect()
    assert last == "r"


def ranges() -> None:
    values = descending()
    alias = values
    assert alias == values and values != descending()
    assert len(values) == 3
    assert values[0] == 7 and values[-1] == 1
    assert values[-3] == 7
    assert 4 in values and 2 not in values
    assert sum_range(values) == 12
    assert str(values) == "[7, 4, 1]"
    assert f"range: {range(3)}" == "range: [0, 1, 2]"
    assert len(range(0)) == 0 and len(range(3, 0)) == 0
    assert 0 not in range(0) and 1 not in range(0, 3, -1)
    boxed = Any(values)
    gc_collect()
    assert cast[Range](boxed)[1] == 4
    assert is_type[Range](boxed) and boxed == Any(alias)
    retained = [descending()]
    gc_collect()
    assert len(retained[0]) == 3
    optional: Optional[Range] = range(2)
    if optional != None:
        assert optional[-1] == 1
    huge = range(-9223372036854775808, 9223372036854775807, 3)
    assert len(huge) == 6148914691236517205
    assert huge[-1] == 9223372036854775804
    assert -9223372036854775808 in huge
    assert 9223372036854775804 in huge
    assert 9223372036854775805 not in huge
    full = range(-9223372036854775808, 9223372036854775807)
    assert full[-1] == 9223372036854775806
    assert full[-9223372036854775808] == -1
    try:
        len(full)
        assert false
    except OverflowError:
        pass
    descending_wide = range(9223372036854775807, -9223372036854775808, -9223372036854775808)
    assert len(descending_wide) == 2
    assert descending_wide[1] == -1
    assert -1 in descending_wide and 0 not in descending_wide
    try:
        values[3]
        assert false
    except IndexError:
        pass
    try:
        values[-4]
        assert false
    except IndexError:
        pass
    try:
        range(0, 0, 0)
        assert false
    except ValueError:
        pass
    # The direct numeric loop is still allocation-free, including at O0.
    before = gc_live_bytes()
    total = 0
    for value in range(10):
        if value == 3:
            continue
        if value == 7:
            break
        total += value
    assert total == 18 and gc_live_bytes() == before
    count = 0
    for value in range(9223372036854775806, 9223372036854775807, 2):
        count += 1
    assert count == 1


def main() -> None:
    strings()
    ranges()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native ranges and text iteration ok")
