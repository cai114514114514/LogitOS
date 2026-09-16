# aether: 3.0
import strings
import paths
import bits

struct Record:
    text: str
    values: List[str]

def identity[T](items: List[T]) -> List[T]:
    return items

def allocate_noise() -> i64:
    # Exercise automatic safepoints, not just explicit collect(). Fixed source
    # slots are overwritten in the loop instead of retaining every temporary.
    for i in range(300):
        noise = "temporary" * 100
    return 7

def collected(text: str, ignored: i64) -> str:
    gc_collect()
    return text

def make_error() -> Error:
    return ValueError("dynamic " + "message")

def fail() -> None:
    raise make_error()

def make_view() -> str:
    return ("prefix" + "suffix").slice(6, 12)

def check_bits() -> None:
    minimum = -9223372036854775808
    maximum = 9223372036854775807
    assert bits.bit(63) == minimum
    assert bits.mask(0) == 0
    assert bits.mask(63) == maximum
    assert bits.mask(64) == -1
    assert bits.has(7, 3)
    assert bits.set(4, 3) == 7
    assert bits.clear(7, 2) == 5
    assert bits.toggle(7, 3) == 4
    assert bits.put(4, 3, True) == 7
    assert bits.put(7, 3, False) == 4
    assert bits.low_byte(0x1234) == 0x34
    assert bits.high_byte(0x1234) == 0x12
    assert bits.align_down(15, 8) == 8
    assert bits.align_up(15, 8) == 16
    assert bits.align_up(maximum, 1) == maximum
    assert bits.count_ones(maximum) == 63
    assert bits.parity(7) == 1
    assert bits.rol(minimum, 1, 64) == 1
    assert bits.ror(1, 1, 64) == minimum
    assert bits.rol(1, -1, 64) == minimum
    assert bits.rol(-1, 27, 64) == -1
    assert bits.ror(-1, 0, 64) == -1
    assert bits.bytes_le(0x1234, 2)[0] == 0x34
    assert bits.bytes_le(-1, 8)[7] == 255
    assert len(bits.bytes_le(10, -1)) == 0
    assert wrapping_shl(i8(1), i8(7)) == i8(-128)
    failures = 0
    try:
        bits.bit(64)
    except ValueError:
        failures += 1
    try:
        bits.bit(-1)
    except ValueError:
        failures += 1
    try:
        bits.rol(1, 1, 0)
    except ValueError:
        failures += 1
    try:
        bits.align_up(maximum, 8)
    except OverflowError:
        failures += 1
    try:
        bits.count_ones(-1)
    except ValueError:
        failures += 1
    assert failures == 5

def check_library() -> None:
    assert "aBc中文".upper() == "ABC中文"
    assert "aBc中文".lower() == "abc中文"
    assert " \t\r\nabc ".strip() == "abc"
    assert "/".join(" a\t b\nc ".split()) == "a/b/c"
    assert "/".join("/a//b/".split("/")) == "/a//b/"
    assert "aaaaa".replace("aa", "b") == "bba"
    assert "abcdef".sub(-2, 3) == "abc"
    assert "abcdef".sub(100, 200) == ""
    assert strings.contains("a\0中文", "\0中")
    assert strings.starts_with("中文", "中")
    assert strings.ends_with("abc", "")
    assert not strings.ends_with("abc", "abcd")
    assert strings.slice("abcdef", -4, -1) == "cde"
    assert strings.slice("abcdef", -90, 90) == "abcdef"
    assert strings.slice("abc", 5, 1) == ""
    assert strings.find("中文a中文", "a") == 6
    assert strings.find("abc", "") == 0
    assert strings.rfind("abcabc", "abc") == 3
    assert strings.rfind("abc", "") == 3
    assert strings.rfind("a", "abc") == -1
    assert strings.repeat("ab", 3) == "ababab"
    assert strings.repeat("ab", -1) == ""
    assert strings.count("aaaaa", "aa") == 2
    assert strings.join(strings.split("/中文//a/", "/"), "/") == "/中文//a/"
    assert strings.join(strings.split("a\0b", "\0"), "-") == "a-b"
    assert strings.lstrip(" \t a  ") == "a  "
    assert strings.rstrip(" a\n\r") == " a"
    assert strings.strip("\r\n中文 \t") == "中文"
    assert strings.replace("aaaaa", "aa", "b") == "bba"
    assert strings.join(strings.lines("a\r\nb\n"), "/") == "a/b/"
    assert strings.join(strings.words("  a  b\tc  "), "/") == "a/b\tc"
    assert strings.pad_left("x", 4, "ab") == "ababx"
    assert strings.pad_right("x", 4, "ab") == "xabab"
    assert paths.is_abs("/a")
    assert paths.basename("/a/b///") == "b"
    assert paths.dirname("/a/b///") == "/a"
    assert paths.extname(".bashrc") == ""
    assert paths.extname("/a/.b.txt") == ".txt"
    assert paths.stem("/a/.b.txt") == ".b"
    assert paths.join("/a/b", "../c") == "/a/c"
    assert paths.join("a", "/b") == "/b"
    assert strings.join(paths.split("/a//b/"), "/") == "a/b"
    assert paths.normalize("../../a/../b") == "../../b"
    assert paths.normalize("/../../a//./b/..") == "/a"
    assert paths.normalize("") == "."
    assert paths.with_ext("/a/b.txt", "as") == "/a/b.as"
    failures = 0
    try:
        strings.split("a", "")
    except ValueError:
        failures += 1
    try:
        strings.count("a", "")
    except ValueError:
        failures += 1
    try:
        strings.replace("a", "", "b")
    except ValueError:
        failures += 1
    try:
        strings.pad_left("a", 0, "")
    except ValueError:
        failures += 1
    assert failures == 4

def check_roots() -> None:
    parts = strings.split("prefix" + "-中文-tail", "-")
    record = Record("record" + " text", parts)
    records: Array[Record, 1] = [record]
    nested: List[Array[Record, 1]] = [records]
    gc_collect()
    assert nested[0][0].values[1] == "中文"
    assert nested[0][0].text == "record text"
    assert collected("first " + "argument", allocate_noise()) == "first argument"
    alias = identity(parts)
    for i in range(200):
        alias.append("value" + " text")
        gc_collect()
    assert len(parts) == 203
    assert parts[-1] == "value text"
    parts[1] = "changed" + " value"
    assert alias[1] == "changed value"
    values: List[i8] = [1, 2]
    values.append(3)
    assert values[-1] == i8(3)
    # Element storage is native i8, not one tagged Value per integer.
    assert len(identity(values)) == 3
    view = make_view()
    gc_collect()
    assert view == "suffix"
    assert view[-1] == "x"
    failures = 0
    try:
        view[6]
    except IndexError:
        failures += 1
    try:
        parts[-1000]
    except IndexError:
        failures += 1
    try:
        fail()
    except ValueError as error:
        gc_collect()
        assert error.message == "dynamic message"
        failures += 1
    assert failures == 3

def main() -> None:
    check_bits()
    check_library()
    check_roots()
    gc_collect()
    # Every dynamic value was owned by a returned frame. No root may dangle
    # into that frame, and no managed allocation should remain after collection.
    assert gc_live_bytes() == 0
    print("managed stdlib ok")
