# aether: 3.0
import abi

def check_arguments() -> None:
    errors = 0
    try:
        abi.io_write(-1, Bytes("x"), 2)
    except ValueError:
        errors += 1
    try:
        abi.fd_read(-1, buffer(1), -1)
    except ValueError:
        errors += 1
    try:
        abi.file_delete("prefix" + chr(0) + "suffix")
    except ValueError:
        errors += 1
    try:
        abi.dir_name("/", 0, buffer(63))
    except ValueError:
        errors += 1
    try:
        abi.stat("/", abi.Stat(), 65535)
    except ValueError:
        errors += 1
    try:
        abi.text_measure("文本", 7, 16, 0)
    except ValueError:
        errors += 1
    try:
        abi.proc_spawn("missing", Bytes("AAAAAAAA"))
    except ValueError:
        errors += 1
    try:
        abi.proc_spawn("missing", buffer(9))
    except ValueError:
        errors += 1
    assert errors == 8

def main() -> None:
    check_arguments()
    print("native ABI argument guards ok")
