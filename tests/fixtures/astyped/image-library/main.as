# aether: 3.0
import std.image as image

def bytes_of(values: List[i64]) -> Buffer:
    data = buffer(len(values))
    for index in range(len(values)):
        data[index] = values[index]
    return data

def check_rectangle(actual: List[i64], expected: List[i64]) -> None:
    assert len(actual) == 4
    for index in range(4):
        assert actual[index] == expected[index]

def main() -> None:
    png = bytes_of([137, 80, 78, 71, 13, 10, 26, 10])
    assert image.magic(png, 8) == "PNG"
    assert image.magic(png, 7) == ""
    assert image.magic(Bytes("GIF89a"), 6) == "GIF"
    assert image.magic(bytes_of([255, 216, 255]), 3) == "JPEG"
    assert image.magic(Bytes("BM"), 2) == "BMP"
    assert image.magic(bytes_of([0, 0, 1, 0]), 4) == "ICO"
    assert image.magic(Bytes("RIFFxxxxWEBP"), 12) == "WebP"
    assert image.magic(Bytes("RIFFxxxxNOPE"), 12) == ""
    assert image.magic(Bytes("<svg"), 4) == "SVG/XML"
    assert image.magic(buffer(0), 0) == ""
    assert image.hexbytes(png, 8, 4) == "89 50 4E 47 "
    assert image.hexbytes(png, 2, 20) == "89 50 "
    assert image.hexbytes(png, 8, -1) == ""

    pixels = bytes_of([255, 0, 0, 255, 1, 2, 3, 4])
    picture = image.Image("sample", "raw", 2, 1, pixels)
    gc_collect()
    assert picture.pixels() == 2
    assert picture.at(0, 0) == 0xFF0000FF
    assert picture.at(1, 0) == 0x01020304
    errors = 0
    try:
        picture.at(2, 0)
    except IndexError:
        errors += 1
    try:
        image.Image("short", "raw", 2, 1, buffer(7))
    except ValueError:
        errors += 1
    try:
        image.magic(png, 9)
    except ValueError:
        errors += 1
    try:
        image.hexbytes(png, -1, 4)
    except ValueError:
        errors += 1
    try:
        image.fit(0, 1, 0, 0, 10, 10)
    except ValueError:
        errors += 1
    assert errors == 5
    check_rectangle(image.fit(4, 2, 10, 20, 6, 6), [10, 21, 6, 3])
    check_rectangle(image.fit(2, 4, 10, 20, 6, 6), [11, 20, 3, 6])
    check_rectangle(image.fit(100, 1, 0, 0, 1, 1), [0, 0, 1, 1])
    check_rectangle(image.fit(2, 2, 10, 20, 0, 5), [10, 20, 0, 0])
    check_rectangle(image.centre(10, 8, 10, 20, 4, 4), [7, 18, 10, 8])
    print("native image operations ok")
