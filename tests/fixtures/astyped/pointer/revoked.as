# aether: 3.0
# The test runtime drops capabilities at gc_collect(), after construction.
# Every denied access targets live owned storage, even in the broken control.
def main() -> None:
    owner = buffer(8)
    unsafe:
        address = addr(owner)
        pointer = i32ptr(address)
        gc_collect()
        denied = 0
        try:
            pointer[0]
        except PermissionError:
            denied += 1
        try:
            pointer[0] = 42
        except PermissionError:
            denied += 1
        try:
            pointer[0] += 1
        except PermissionError:
            denied += 1
        try:
            i32ptr(address)
        except PermissionError:
            denied += 1
        try:
            addr(pointer)
        except PermissionError:
            denied += 1
        try:
            mem2str(pointer, 1)
        except PermissionError:
            denied += 1
        assert denied == 6 and len(owner) == 8
    print("native pointer revocation ok")
