# a module: imported as `mathx` (lives at /usr/as/mathx.as)
PI = 314
def square(x):
    return x * x
def quad(x):
    return square(square(x))   # resolves module-mate `square`
