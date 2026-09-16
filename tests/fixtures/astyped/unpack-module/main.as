# aether: 3.0

module_a, module_b = range(11, 13)
module_text, module_count = "module " + "text", collect()


def collect() -> i64:
    gc_collect()
    return 42


def main() -> None:
    gc_collect()
    assert module_a == 11 and module_b == 12
    assert module_text == "module text" and module_count == 42
    assert gc_live_bytes() == len(module_text) + 1
    print("native module unpack ok")
