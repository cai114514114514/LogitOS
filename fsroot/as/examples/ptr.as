# aether: 3.0
# indirection: a typed pointer over a raw byte buffer
# A3 correction: str is immutable. Own writable bytes explicitly, and keep
# that owner alive while using its raw address. Ptr does not keep it alive.
def main() -> None:
    storage = buffer(8)
    unsafe:
        pointer = i32ptr(addr(storage))
        pointer[0] = 1000
        pointer[1] = 337
        print("p[0] + p[1] =", pointer[0] + pointer[1])
