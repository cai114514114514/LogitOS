# closure + lambda demo (M22.1)
def counter():
    c = 0
    def inc():
        c = c + 1
        return c
    return inc
f = counter()
print("counts:", f(), f(), f())
double = lambda x: x * 2
print("double 21:", double(21))
def adder(n):
    return lambda x: x + n
print("add10 to 5:", adder(10)(5))
