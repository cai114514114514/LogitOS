# aether: 3.0

def original() -> None:
    raise IOError("读取失败")

def main() -> None:
    try:
        original()
    except ValueError:
        print("WRONG HANDLER")
    print("UNREACHABLE")
