# aether: 3.0
# String methods, formatting, comprehensions and simultaneous assignment.
# The native example gate compares the entire output with the pre-migration run.

def main() -> None:
    print("join:", ",".join(["a", "b", "c"]))
    print("split:", " a  b ".split())
    print("strip:", "[" + "  hi  ".strip() + "]")
    print("case:", "MiXed".upper(), "MiXed".lower())
    print("replace:", "aXaXa".replace("X", "--"))
    print("find:", "hello".find("ll"), "x".find("z"))

    n = 7
    print(f"fstr: n={n} sq={n * n}")
    print("tern:", "even" if n % 2 == 0 else "odd")
    print("comp:", [x * x for x in range(4)])
    print("compif:", ",".join([str(x) for x in range(8) if x % 2 == 0]))

    # Swapping reads both old values before either variable is overwritten.
    a, b = 1, 2
    a, b = b, a
    x, y, z = [10, 20, 30]
    print("swap:", a, b, "unpack:", x + y + z)
    print("strings ok")
