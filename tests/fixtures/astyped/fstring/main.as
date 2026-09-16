# aether: 3.0

sequence = 0


def next_value() -> i64:
    global sequence
    sequence += 1
    gc_collect()
    return sequence


def label[T](value: T) -> str:
    return f"value={value}"


def failing() -> i64:
    raise ValueError("hole failed")


def worker() -> None:
    text = "中文 " + "text"
    assert f"" == ""
    assert F'{{{text}}}' == "{中文 text}"
    assert f"{{}} {{x}} }} {{" == "{} {x} } {"
    assert f"\n\t\r\0\\\"" == "\n\t\r\0\\\""
    assert f"literal }} only" == "literal } only"
    assert f"hello {  text  } {2 + 3 * 4}" == "hello 中文 text 14"
    assert f"{true} {None} {f64(2)}" == "true None 2"
    assert f"{[1, 2]} {{ { {'x': 3} } }}" == "[1, 2] { {'x': 3} }"
    assert f"{ '{{}}' }" == "{{}}"
    assert f"{f'nested {text}'}" == "nested 中文 text"
    assert f"{1 if text != '' else 2}" == "1"
    assert label(42) == "value=42"
    assert label(text) == "value=中文 text"
    assert label(Any(text)) == "value=中文 text"
    maybe: Optional[i64] = 7
    assert f"{maybe}" == "7"
    assert f"{maybe if maybe != None else None}" == "7"
    assert f"multiline {
        2 +
        3
    } done" == "multiline 5 done"

    # Earlier formatted results remain rooted while later holes allocate/GC.
    assert f"{text} {next_value()} {next_value()}" == "中文 text 1 2"
    try:
        result = f"before {failing()} after {next_value()}"
        assert false
    except ValueError as error:
        assert error.message == "hole failed"
    assert sequence == 2
    try:
        result = f"中文 {text[100]}"
        assert false
    except IndexError as error:
        assert error.column > 15
        assert error.file != ""


def main() -> None:
    worker()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native fstrings ok")
