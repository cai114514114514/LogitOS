# aether: 3.0
# Augmented assignment captures the receiver, index and old value before
# evaluating its right side. Resizing a container must not leave a stale slot.
total: i64 = 1
events: List[i64] = []
data = buffer(1)
items: List[i64] = [1]
mapping: Dict[str, i64] = {"value": 1}

class Cell:
    value: i64

struct Record:
    value: i64

struct Row:
    values: Array[i64, 2]

current = Cell(1)
records: List[Record] = [Record(1)]
title: str = str(41)
record_map: Dict[str, Record] = {"record": Record(1)}
rows: List[Row] = [Row([1, 2])]

def change_total() -> i64:
    global total
    total = 10
    return 2

def replace_cell() -> i64:
    global current
    current = Cell(90)
    gc_collect()
    return 2

def choose_buffer() -> Buffer:
    events.append(1)
    return data

def choose_index() -> i64:
    events.append(2)
    return 0

def change_byte() -> i64:
    events.append(3)
    data[0] = 10
    return 2

def grow_list() -> i64:
    items[0] = 10
    for number in range(64):
        items.append(number)
    gc_collect()
    return 2

def change_dict() -> i64:
    mapping.remove("value")
    for number in range(64):
        mapping[str(number)] = number
    gc_collect()
    return 2

def grow_records() -> i64:
    for number in range(64):
        records.append(Record(number))
    gc_collect()
    return 2

def replace_title() -> str:
    global title
    title = str(98)
    gc_collect()
    return "!"

def grow_record_map() -> i64:
    for number in range(64):
        record_map[str(number)] = Record(number)
    gc_collect()
    return 2

def remove_record() -> i64:
    record_map.remove("record")
    gc_collect()
    return 2

def choose_cell() -> Cell:
    events.append(4)
    return current

def literal_byte() -> i64:
    return 255

def grow_rows() -> i64:
    for number in range(64):
        rows.append(Row([number, number]))
    gc_collect()
    return 0

def main() -> None:
    global total, events
    total += change_total()
    print(total)
    assert total == 3

    saved = current
    current.value += replace_cell()
    assert saved.value == 3
    assert current.value == 90

    data[0] = 1
    choose_buffer()[choose_index()] += change_byte()
    assert data[0] == 3
    assert len(events) == 3
    assert events[0] == 1 and events[1] == 2 and events[2] == 3

    events = []
    choose_buffer()[choose_index()] = change_byte()
    assert data[0] == 2
    assert events[0] == 3 and events[1] == 1 and events[2] == 2

    items[0] += grow_list()
    assert items[0] == 3
    assert len(items) == 65
    mapping["value"] += change_dict()
    assert mapping["value"] == 3
    assert len(mapping) == 65
    records[0].value += grow_records()
    assert records[0].value == 3
    assert records[64].value == 63
    record_map["record"].value += grow_record_map()
    assert record_map["record"].value == 3
    try:
        record_map["record"].value += remove_record()
        assert False
    except KeyError:
        pass

    # The previous heap string must survive replacement of its last owner.
    global title
    title += replace_title()
    assert title == "41!"
    choose_cell().value += 2
    assert current.value == 92

    local = 20
    local -= 2
    local *= 3
    local /= 2
    local %= 5
    assert local == 2
    small: i8 = 127
    try:
        small += 1
        assert False
    except OverflowError:
        pass
    assert small == 127
    data[0] = 1
    try:
        data[0] += literal_byte()
        assert False
    except ValueError:
        pass
    assert data[0] == 1

    # Negative indices are captured as values. The final store interprets -1
    # against the current length, just like a normal subscript assignment.
    items[-1] += grow_list()
    assert items[-1] == 65

    # The target's outer index itself may relocate the parent's storage.
    rows[0].values[grow_rows()] += 2
    assert rows[0].values[0] == 3
    rows[0].values[grow_rows()] = 7
    assert rows[0].values[0] == 7
    matrix: Array[Array[i64, 2], 2] = [[1, 2], [3, 4]]
    matrix[1][0] *= 2
    assert matrix[1][0] == 6

    events = []
    try:
        items[999] += change_byte()
        assert False
    except IndexError:
        pass
    assert len(events) == 0
    try:
        mapping["missing"] += change_byte()
        assert False
    except KeyError:
        pass
    assert len(events) == 0
    print("native compound assignment ok")
