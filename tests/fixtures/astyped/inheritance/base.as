# aether: 3.0
# Imported base layout and signatures are shared with the derived module.
class Animal:
    name: str

    def init(self, name: str) -> None:
        self.name = name

    def speak(self) -> str:
        return self.name + " makes a sound"

    def describe(self) -> str:
        return "[" + self.speak() + "]"

    def get(self) -> str:
        return self.name
