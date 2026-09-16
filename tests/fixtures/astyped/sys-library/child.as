# aether: 3.0
import std.sys as system

def main() -> i64:
    arguments = args()
    assert len(arguments) == 4
    assert arguments[0] == "sys-child"
    assert arguments[1] == ""
    assert arguments[2] == "two words"
    assert arguments[3] == "中文"
    # Exercise the public exit wrapper. A successful call never returns into
    # the language frame; code below it would change the captured child status.
    system.exit(9)
    return 99
