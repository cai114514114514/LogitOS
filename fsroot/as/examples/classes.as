# aether: 3.0
# Fields keep their native layout through inheritance. super selects the
# parent implementation while ordinary self calls dispatch to overrides.
class Animal:
    name: str

    def init(self, name: str) -> None:
        self.name = name

    def speak(self) -> str:
        return self.name + " makes a sound"


class Dog(Animal):
    def speak(self) -> str:
        return super.speak() + " (woof)"


class Counter:
    n: i64

    def init(self) -> None:
        self.n = 0

    def bump(self) -> i64:
        self.n += 1
        return self.n


def main() -> None:
    animal = Animal("Generic")
    print("animal:", animal.speak())
    dog = Dog("Rex")
    print("dog:", dog.speak(), "name:", dog.name)
    counter = Counter()
    print("counter:", counter.bump(), counter.bump(), counter.bump())
