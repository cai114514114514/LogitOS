# aether: 3.0
# Optional carries a presence tag and an inline payload. A None test narrows
# only the local value: mutable fields and module storage still need a copy.
struct Header:
    label: Optional[str]
    code: i64

saved: Optional[str] = "module " + "value"


def maybe(flag: bool) -> Optional[str]:
    if flag:
        return "live " + "text"
    return None


def require(value: Optional[str]) -> str:
    if value == None:
        raise ValueError("missing text")
    return value


def choose[T](value: Optional[T], fallback: T) -> T:
    if value != None:
        return value
    return fallback


def make_header() -> Any:
    return Any(Header(maybe(true), 42))


def collect() -> str:
    gc_collect()
    return "second"


def values() -> None:
    empty = None
    assert empty == None and None == None
    assert Any(None) == None and Any(None) != Any(1)
    assert is_type[NoneType](Any(None))
    assert str(None) == "None"
    assert "True" == "Tr" + "ue" and "False" == "Fa" + "lse"
    missing: Optional[str] = None
    present: Optional[str] = maybe(true)
    assert missing == None and None != present
    assert str(missing) == "None" and str(present) == "live text"
    assert require(present) == "live text"
    assert choose(present, "fallback") == "live text"
    assert choose(missing, "fallback") == "fallback"
    assert choose(None, "fallback") == "fallback"
    assert choose(7, 0) == 7
    assert (present if present != None else "fallback") == "live text"
    inferred = 7 if true else None
    assert inferred != None
    assert inferred == 7
    assert present != None and len(present) == 9
    assert missing == None or len(missing) == 0
    assert missing is None
    assert present is not None and len(present) == 9
    assert not (present == None)
    assert len(present) == 9
    present = None
    assert present == None
    present = "replacement"
    if present != None:
        assert len(present) == 11
    if missing != None:
        assert len(missing) == 0
    else:
        missing = "filled"
    assert missing != None
    assert missing == "filled"

    numbers: List[Optional[i64]] = [None, 3, None, 9]
    assert str(numbers) == "[None, 3, None, 9]"
    sought: Optional[i64] = 3
    assert sought in numbers
    absent: Optional[i64] = None
    assert absent in numbers
    other: Optional[i64] = 11
    assert other not in numbers
    assert Any(absent) == Any(numbers[0])
    assert Any(sought) != Any(numbers[0])
    assert Any(sought) == Any(numbers[1])
    boxed = make_header()
    gc_collect()
    header = cast[Header](boxed)
    assert require(header.label) == "live text"
    assert require(saved) == "module value"

    texts: Optional[List[str]] = ["first " + "text", collect()]
    assert texts != None
    assert texts[0] == "first text" and texts[1] == "second"
    dictionary: Optional[Dict[str, str]] = {"first": "first " + "text", "second": collect()}
    assert dictionary != None
    found = dictionary.get("first")
    assert require(found) == "first text"
    assert dictionary.get("missing") == None
    assert dictionary.get("missing", "fallback") == "fallback"
    nullable: Dict[str, Optional[str]] = {"empty": None, "full": "present"}
    nested = nullable.get("empty")
    assert nested != None
    inner: Optional[str] = choose(nested, maybe(false))
    assert inner == None

    i: Optional[i64] = 3
    total = 0
    while i != None:
        total += i
        i = None
    assert total == 3
    caught = false
    try:
        require(None)
    except ValueError:
        caught = true
    assert caught
    return None


def main() -> None:
    values()
    # The iterable is selected once. Dropping its nullable source in the body
    # must neither invalidate the iterator nor let GC reclaim its live list.
    sequence: Optional[List[str]] = ["first " + "item", "second " + "item"]
    assert sequence is not None
    joined = ""
    for item in sequence:
        sequence = None
        gc_collect()
        joined += item
    assert joined == "first itemsecond item"
    assert sequence is None
    gc_collect()
    print("native Optional ok")
