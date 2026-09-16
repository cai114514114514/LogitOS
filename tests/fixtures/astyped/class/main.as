# aether: 3.0
from models import Named, Fallible, create
from seq import map
import models

class Counter:
    label: str
    value: i64
    next: Optional[Counter]

    def init(self, label: str, start: i64) -> None:
        self.label = label
        if start < 0:
            self.value = 0
        else:
            self.value = start
        self.next = None
        # GC during construction must see the receiver and initialized text.
        gc_collect()

    def add(self, amount: i64) -> i64:
        self.value += amount
        return self.value

    def get(self) -> i64:
        return self.value

    def describe(self) -> str:
        return self.label

    def append(self, amount: i64) -> None:
        self.add(amount)

    def fail(self) -> None:
        raise ValueError(self.label)

class Plain:
    name: str
    counter: Counter

class Empty:
    pass


def make_callback() -> Callable[[i64], i64]:
    counter = Counter("callback " + "owner", 7)
    return counter.add


def make_owner() -> Counter:
    return Counter("retained " + "field", 0)


def exercise() -> None:
    first = Counter("first " + "counter", 3)
    alias = first
    assert alias.add(2) == 5
    assert first.value == 5 and first.get() == 5
    first.append(4)
    assert first.get() == 9
    assert first == alias
    second = Counter("second", -1)
    assert first != second
    first.next = second
    second.next = first
    gc_collect()
    next = first.next
    assert next != None and next.value == 0
    callback = make_callback()
    gc_collect()
    assert callback(2) == 9
    assert callback(3) == 12
    owner = make_owner()
    gc_collect()
    assert owner.describe() == "retained field"
    assert first.add == alias.add and first.add != second.add
    methods: List[Callable[[i64], i64]] = [first.add, second.add]
    gc_collect()
    assert methods[1](4) == 4
    assert map(second.add, [1, 2])[1] == 7
    name = create("imported " + "class")
    assert name.label() == "imported class"
    assert name.display() == "imported class!"
    assert models.Named("qualified").name == "qualified"
    named: Optional[Named] = Named("optional " + "object")
    gc_collect()
    assert named != None and named.name == "optional object"
    failed = false
    try:
        Fallible(false)
    except ValueError as error:
        failed = error.message == "constructor rejected"
    assert failed
    assert Fallible(true).label == "created value"
    boxes: List[Any] = [Any(first), Any(Plain("outer", second))]
    gc_collect()
    assert cast[Counter](boxes[0]) == first
    assert cast[Plain](boxes[1]).counter == second
    assert Any(first) == Any(alias) and Any(first) != Any(second)
    empty = Empty()
    gc_collect()
    assert empty == empty and empty != Empty()
    assert str(empty) == "Empty()"
    assert str(Plain("record", second)).find("Plain(name='record'") == 0
    assert str(first).find("(...)") >= 0
    caught = false
    try:
        first.fail()
    except ValueError as error:
        caught = error.message == "first counter"
    assert caught


def main() -> None:
    exercise()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native classes ok")
