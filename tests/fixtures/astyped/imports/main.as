# aether: 3.0
# A local strings module deliberately shadows the standard library. Explicit
# std imports must still identify the library in both execution and snapshots.
import strings as local_text
import std.strings as text
from std.strings import words as split_words
import nested.values as values
from nested.values import Point as Position, answer as number

def main() -> None:
    point: Position = Position(number)
    assert local_text.answer() == 7
    assert text.contains("native imports", "imports")
    assert split_words("one two")[1] == "two"
    assert point.x == 42
    assert values.answer == 42
    print("native imports ok")
