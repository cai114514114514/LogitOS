# classes: init / methods / fields / inheritance / super (M22.3)
class Animal:
    def init(self, name):
        self.name = name
    def speak(self):
        return self.name + " makes a sound"

class Dog(Animal):
    def speak(self):
        return super.speak() + " (woof)"

a = Animal("Generic")
print("animal:", a.speak())
d = Dog("Rex")
print("dog:", d.speak(), "name:", d.name)

# a small counter object to show mutable fields
class Counter:
    def init(self):
        self.n = 0
    def bump(self):
        self.n = self.n + 1
        return self.n

c = Counter()
print("counter:", c.bump(), c.bump(), c.bump())
