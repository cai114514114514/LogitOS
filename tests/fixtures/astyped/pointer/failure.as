# aether: 3.0
# This must fail before any load and retain the original subscript location.
def main() -> None:
    unsafe:
        i32ptr(0)[0]
