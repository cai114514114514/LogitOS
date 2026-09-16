# aether: 3.0
from base import Animal


class Dog(Animal):
    def speak(self) -> str:
        return super.speak() + " (woof)"

    def get(self) -> str:
        return super.get() + "!"


class Puppy(Dog):
    age: i64

    def init(self, name: str, age: i64) -> None:
        super.init(name)
        self.age = age

    def speak(self) -> str:
        return super.speak() + " puppy"

    def reader(self) -> Callable[[], str]:
        return lambda: self.describe()

    def parent_reader(self) -> Callable[[], str]:
        return lambda: super.speak()


class Leaking:
    name: str

    def init(self) -> None:
        self.name = "base"
        consume(self)


class Incomplete(Leaking):
    later: i64

    def init(self) -> None:
        super.init()
        self.later = 7


class Complete(Leaking):
    later: i64

    def init(self) -> None:
        # Initializing new fields before a parent that exposes self is valid.
        self.later = 9
        super.init()


class Capturing:
    name: str

    def init(self) -> None:
        self.name = "capture"
        callback: Callable[[], str] = lambda: self.name
        callback()


class IncompleteCapture(Capturing):
    later: i64

    def init(self) -> None:
        super.init()
        self.later = 8


class CompleteCapture(Capturing):
    later: i64

    def init(self) -> None:
        self.later = 10
        super.init()


def consume(value: Leaking) -> None:
    pass


def as_animal() -> Animal:
    # Upcasting a constructor must allocate the concrete child layout.
    return Puppy("little " + "dog", 2)


def maybe_animal() -> Optional[Animal]:
    return Dog("optional " + "dog")


def callback() -> Callable[[], str]:
    view: Animal = Puppy("bound " + "receiver", 3)
    return view.describe


def worker() -> None:
    assert Complete().later == 9
    assert CompleteCapture().later == 10
    puppy = Puppy("Rex", 1)
    assert puppy.age == 1 and puppy.name == "Rex"
    assert puppy.speak() == "Rex makes a sound (woof) puppy"
    view: Animal = puppy
    assert view.describe() == "[Rex makes a sound (woof) puppy]"
    assert view.get() == "Rex!"
    view.name = "renamed"
    assert puppy.name == "renamed"
    assert as_animal().get() == "little dog!"
    values: List[Animal] = [Dog("first"), Puppy("second", 2)]
    assert values[1].get() == "second!"
    optional = maybe_animal()
    if optional != None:
        assert optional.get() == "optional dog!"
    keep = callback()
    reader = puppy.reader()
    parent_reader = puppy.parent_reader()
    gc_collect()
    assert keep() == "[bound receiver makes a sound (woof) puppy]"
    assert reader() == "[renamed makes a sound (woof) puppy]"
    assert parent_reader() == "renamed makes a sound (woof)"
    assert str(puppy) == "Puppy(name='renamed', age=1)"

    # A base initializer may appear complete from its own static view. It
    # still cannot expose an object whose derived fields are uninitialized.
    rejected = 0
    try:
        Incomplete()
    except RuntimeError as error:
        assert error.line > 0
        rejected += 1
    try:
        IncompleteCapture()
    except RuntimeError:
        rejected += 1
    assert rejected == 2


def main() -> None:
    worker()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native inheritance ok")
