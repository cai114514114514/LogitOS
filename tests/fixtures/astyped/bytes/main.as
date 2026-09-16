# aether: 3.0
# Arbitrary binary data is not text. Freezing takes a snapshot, including NUL
# and bytes that cannot occur alone in UTF-8; later Buffer writes stay local.
struct Packet:
    tag: i64
    payload: Bytes


def payload() -> Bytes:
    source = buffer(3)
    source[0] = 0
    source[1] = 128
    source[2] = 255
    frozen = Bytes(source)
    source[2] = 1
    assert frozen[2] == 255
    return frozen


def keep(value: Bytes) -> Callable[[], Bytes]:
    return lambda: value


def collect() -> i64:
    gc_collect()
    return 1


def consume(value: Bytes, count: i64) -> None:
    assert count == 1 and value[-1] == 255


def same[T: Equatable](left: T, right: T) -> bool:
    return left == right


def worker() -> None:
    empty = Bytes("")
    assert len(empty) == 0 and 0 not in empty
    assert empty == Bytes(buffer(0))
    value = payload()
    assert len(value) == 3 and value[-3] == 0
    assert 128 in value and -1 not in value and 256 not in value
    assert Bytes(value) == value and same(value, payload())
    assert value != Bytes("abc") and value != Bytes("ab")
    first, middle, last = value
    assert first == 0 and middle == 128 and last == 255
    selected = [byte for byte in value if byte > 0]
    assert len(selected) == 2 and selected[0] == 128 and selected[1] == 255
    total = 0
    for byte in payload():
        gc_collect()
        total += byte
    assert total == 383
    # Text construction copies the exact UTF-8 byte sequence, never an ASCII
    # truncation, and respects the bounds of a substring view.
    chinese = Bytes("中文")
    assert len(chinese) == 6 and chinese[0] == 228 and chinese[-1] == 135
    text = "x" + "abc" + "y"
    assert Bytes(text.slice(1, 4)) == Bytes("abc")
    boxed = Any(payload())
    assert boxed == Any(payload())
    assert is_type[Bytes](boxed) and not is_type[Buffer](boxed)
    retained = Packet(7, payload())
    items: List[Bytes] = [payload()]
    table: Dict[str, Bytes] = {"data": payload()}
    fixed: Array[Bytes, 1] = [payload()]
    optional: Optional[Bytes] = payload()
    closure = keep(payload())
    gc_collect()
    assert cast[Bytes](boxed)[-1] == 255
    assert retained.payload == value and items[0] == value
    assert table["data"] == value and fixed[0] == value
    assert optional != None and optional == value
    assert Any(Packet(7, payload())) == Any(Packet(7, payload()))
    assert Any(fixed) == Any(fixed)
    assert closure() == value and value in items
    consume(payload(), collect())
    assert str(value) == "Bytes(\"\\x00\\x80\\xff\")"
    assert str(boxed) == str(value)

    rejected = 0
    try:
        print(value[3])
    except IndexError:
        rejected += 1
    try:
        print(value[-4])
    except IndexError:
        rejected += 1
    try:
        print(empty[0])
    except IndexError:
        rejected += 1
    try:
        first, last = value
    except ValueError:
        rejected += 1
    try:
        cast[Buffer](boxed)
    except TypeError:
        rejected += 1
    assert rejected == 5


def main() -> None:
    worker()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native Bytes ok")
