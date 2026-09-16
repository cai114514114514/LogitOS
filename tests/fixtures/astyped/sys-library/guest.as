# aether: 3.0
import std.sys as system

def main() -> None:
    directory = "/state/sys-native-" + str(system.pid())
    path = directory + "/file.txt"
    moved = directory + "/renamed.txt"
    assert system.mkdir(directory) == 0
    assert system.write_file(path, Bytes("hello 中文")) == 12
    text = system.read_file(path)
    assert text is not None
    assert text == "hello 中文"
    # LogitFS whole-file reads reject an insufficient maximum; this is the
    # original API's bound, not a prefix-read request like fd.read(count).
    assert system.read_file_max(path, 5) is None
    exact = system.read_file_max(path, 12)
    assert exact is not None
    assert exact == "hello 中文"
    assert system.read_file(directory + "/missing") is None
    assert system.ls(directory + "/missing") is None
    names = system.ls(directory)
    assert names is not None
    assert "file.txt" in names
    assert system.rename(path, moved) == 0
    assert system.remove(moved) == 0

    old = system.cwd()
    assert old is not None
    assert system.chdir(directory) == 0
    current = system.cwd()
    assert current is not None
    assert current == directory
    assert system.chdir(old) == 0
    clock = system.time()
    assert clock.year >= 2024 and clock.hour >= 0 and clock.hour < 24
    assert system.pid() > 0 and system.cpu() >= 0
    system.sleep(0)

    # An interior view's suffix must not enter argv. The child checks every
    # argument (including empty, whitespace and UTF-8), then returns a real code.
    arguments = ["sys-child", "", "two words", ("prefix中文suffix").slice(6, 12)]
    child = system.spawn("/bin/sys-child", arguments)
    assert child > 0
    assert system.wait(child) == 9
    assert system.run("/bin/sys-child", arguments) == 9
    assert system.run("/bin/no-such-sys-child", arguments) == 127
    print("native sys guest ok")
