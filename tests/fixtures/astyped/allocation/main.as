# aether: 3.0
# Keep every raw access inside a live allocation. The error cases exercise
# release validation, never dereference a released or out-of-bounds pointer.
def text_snapshot() -> str:
    unsafe:
        memory = alloc(4)
        try:
            memory[0] = 65
            memory[1] = 51
            snapshot = mem2cstr(memory, 4)
        except Error as error:
            dealloc(memory)
            raise error
        dealloc(memory)
        return snapshot

def release_on_exception() -> None:
    unsafe:
        memory = alloc(8)
        try:
            memory[0] = 255
            raise ValueError("release this allocation")
        except Error as error:
            dealloc(memory)
            raise error
        dealloc(memory)

def main() -> None:
    unsafe:
        before = gc_live_bytes()
        memory: Ptr[u8] = alloc(64)
        alias = memory
        assert gc_live_bytes() == before
        try:
            for index in range(64):
                assert memory[index] == 0
            words = i32ptr(addr(memory))
            words[0] = -42
            words[1] = 1337
            memory[63] = 255
            gc_collect()
            assert words[0] + words[1] == 1295
            assert memory[63] == 255

            rejected = 0
            try:
                dealloc(i8ptr(addr(memory) + u64(1)))
            except ValueError:
                rejected += 1
            owner = buffer(8)
            try:
                dealloc(i8ptr(addr(owner)))
            except ValueError:
                rejected += 1
            try:
                dealloc(i8ptr(0))
            except ValueError:
                rejected += 1
            assert rejected == 3
            owner[0] = 19
            assert owner[0] == 19 and words[0] == -42
        except Error as error:
            dealloc(memory)
            raise error
        # Retyping a base address does not change who allocated it.
        dealloc(i64ptr(addr(memory)))

        # No intervening allocation: the old address cannot have been reused.
        duplicate = False
        try:
            dealloc(alias)
        except ValueError:
            duplicate = True
        assert duplicate

        for size in [0, -1]:
            invalid_size = False
            try:
                alloc(size)
            except ValueError:
                invalid_size = True
            assert invalid_size

    assert text_snapshot() == "A3"
    caught = False
    try:
        release_on_exception()
    except ValueError:
        caught = True
    assert caught
    print("native manual allocation ok")
