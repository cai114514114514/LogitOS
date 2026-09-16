# aether: 3.0

class Named:
    name: str

    def label(self) -> str:
        return self.name

    def _suffix(self) -> str:
        return "!"

    def display(self) -> str:
        return self.name + self._suffix()

class Fallible:
    label: str

    def init(self, valid: bool) -> None:
        if not valid:
            raise ValueError("constructor rejected")
        self.label = "created " + "value"


def create(name: str) -> Named:
    return Named(name)
