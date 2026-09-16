# aether: 3.0
# recursion

def fib(n: i64) -> i64:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

def main() -> None:
    print("fib(20) =", fib(20))
