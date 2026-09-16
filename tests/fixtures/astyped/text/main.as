# aether: 3.0

def forbidden() -> str:
    raise ValueError("unchosen branch ran")

def main() -> None:
    text = "Aether 中文"
    assert "中文" in text
    assert not ("missing" in text)
    assert "" in text
    assert text.find("中文") == 7
    assert text.find("missing") == -1
    assert text.find("") == 0
    assert "abc" == "abc"
    assert "abc" != "abcd"
    assert "ab" < "abc"
    assert "ac" > "ab"
    assert "a\0b" < "a\0c"
    assert "\0b" in "a\0b"
    assert "a\0b".find("\0b") == 1
    assert len("a\0b") == 3
    chosen = text if True or False else forbidden()
    other = forbidden() if False else "second"
    narrow: i8 = 127 if True else -128
    assert narrow == 127
    assert (1 if False else 2 if True else 3) == 2
    print(chosen, other)
    print("text ok")
