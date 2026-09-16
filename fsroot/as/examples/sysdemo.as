# aether: 3.0
# The original system demo now runs as a native multi-module program. Text is
# explicitly encoded for byte I/O, and Optional failures are checked before use.
from std.sys import read_file, write_file, ls, remove, mkdir, run, time, pid, cwd


def main() -> None:
    payload = Bytes("written by AetherScript\n")
    written = write_file("/docs/sysdemo.txt", payload)
    assert written == len(payload)
    back = read_file("/docs/sysdemo.txt")
    assert back is not None
    print("roundtrip:", back.strip())

    names = ls("/docs")
    assert names is not None
    print("ls has it:", "sysdemo.txt" in names)
    assert remove("/docs/sysdemo.txt") == 0

    # The child inherits the output descriptor, so its complete text and the
    # real wait status are both observable by Studio or a command-line caller.
    code = run("/bin/echo", ["echo", "spawned-from-script"])
    print("spawn exit:", code)

    clock = time()
    print("clock sane:", clock.year >= 2024 and clock.hour >= 0 and clock.hour < 24)
    directory = cwd()
    assert directory is not None
    print(f"pid={pid() > 0} cwd={directory}")
    print("sysdemo ok")
