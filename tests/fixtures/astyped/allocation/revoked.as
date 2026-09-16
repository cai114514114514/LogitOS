# aether: 3.0
# The test runtime drops RAW at the first explicit collection and restores it
# at the second. A refused release must leave the existing allocation alive.
def main() -> None:
    unsafe:
        memory = alloc(8)
        memory[0] = 73
        gc_collect()
        denied = 0
        try:
            temporary = alloc(4)
            dealloc(temporary)
        except PermissionError:
            denied += 1
        try:
            dealloc(memory)
        except PermissionError:
            denied += 1
        assert denied == 2
        gc_collect()
        assert memory[0] == 73
        dealloc(memory)
    print("native allocation revocation ok")
