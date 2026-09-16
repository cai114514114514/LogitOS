# aether: 3.0
import std.sys as system

def main() -> None:
    assert system.ip_str(0) == "0.0.0.0"
    assert system.ip_str(0x7F000001) == "127.0.0.1"
    assert system.ip_str(-1) == "255.255.255.255"
    assert system.wait(-1) == -1
    assert system.wait(0) == -1
    errors = 0
    try:
        system.sleep(-1)
    except ValueError:
        errors += 1
    try:
        system.sleep(9223372036854775807)
    except OverflowError:
        errors += 1
    try:
        system.spawn("bad" + chr(0) + "path", ["program"])
    except ValueError:
        errors += 1
    try:
        system.spawn("missing", ["bad" + chr(0) + "argument"])
    except ValueError:
        errors += 1
    try:
        system.read_file_max("missing", -1)
    except ValueError:
        errors += 1
    assert errors == 5
    print("native sys argument checks ok")
