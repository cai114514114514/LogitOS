# aether: 3.0
def main() -> i64:
    arguments = args()
    if arguments[1] == "arguments":
        assert len(arguments) == 5
        assert arguments[2] == "" and arguments[3] == "two words" and arguments[4] == "中文"
        print("arguments ok")
        return 7
    if arguments[1] == "emit":
        with output = port(1):
            output.write("x" * 200000)
        return 9
    return parse_int(arguments[2])
