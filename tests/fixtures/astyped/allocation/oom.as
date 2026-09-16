# aether: 3.0
# The private runtime refuses this small allocation deterministically. No
# stress allocation is needed to test source-level MemoryError propagation.
def main() -> None:
    denied = False
    unsafe:
        try:
            memory = alloc(8)
            dealloc(memory)
        except MemoryError:
            denied = True
    assert denied
    print("native allocation failure ok")
