# aether: 3.0
class Decoder:
    def decode(self) -> str:
        return "user method"


def binary(values: List[i64]) -> Bytes:
    data = buffer(len(values))
    for index in range(len(values)):
        data[index] = values[index]
    return Bytes(data)


def retained() -> str:
    return Bytes("中文" + chr(0) + "text").decode()


def worker() -> None:
    assert Bytes("").decode() == ""
    assert Bytes("ASCII").decode() == "ASCII"
    assert Bytes("中文😀").decode() == "中文😀"
    controls = binary([0, 127, 194, 128])
    assert Bytes(controls.decode()) == controls
    text = retained()
    optional: Optional[str] = Bytes("optional").decode()
    values: List[str] = [Bytes("first").decode(), Bytes("second").decode()]
    gc_collect()
    assert text == "中文" + chr(0) + "text"
    assert optional != None and optional == "optional"
    assert values[0] == "first" and values[1] == "second"
    assert Decoder().decode() == "user method"

    invalid: List[List[i64]] = [
        [128], [191], [192, 128], [193, 191], [194], [194, 65],
        [224, 128, 128], [224, 160], [237, 160, 128], [239, 191],
        [240, 128, 128, 128], [240, 144, 128], [244, 144, 128, 128],
        [245, 128, 128, 128], [255], [65, 0, 255]
    ]
    rejected = 0
    for value in invalid:
        try:
            binary(value).decode()
        except ConversionError as error:
            assert error.line > 0 and error.column > 0
            rejected += 1
    assert rejected == len(invalid)


def main() -> None:
    worker()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native UTF-8 decode ok")
