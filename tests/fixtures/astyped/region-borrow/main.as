# aether: 3.0
# Plain slices carry data/length; their lexical loan descriptors must outlive
# every read and drop before their parent views or the owning Region.
def sum_bytes(data: Slice[u8]) -> i64:
    total = 0
    for byte in data:
        total += i64(byte)
    return total

def first[T: Integer](data: Slice[T]) -> T:
    return data[0]

def increment[T: Integer](data: MutSlice[T], amount: T) -> None:
    data[0] += amount

def delegate(data: MutSlice[u8]) -> None:
    increment(data, u8(2))
    assert data[0] == 11

def returned_value() -> i64:
    with owner = region(2):
        with parent = owner.borrow_mut(0, 2):
            parent[0] = 19
            with child = parent.borrow(0, 1):
                return sum_bytes(child)

def raise_from_view() -> None:
    with owner = region(2):
        with parent = owner.borrow_mut(0, 2):
            with child = parent.borrow(0, 2):
                child[2]

def main() -> None:
    with owner = region(8):
        owner[2] = 41
        owner[3] = 42
        with view = owner.borrow(2, 4):
            assert len(view) == 2 and view[0] == 41 and view[1] == 42
            assert owner[2] == 41
            assert sum_bytes(view) == 83
            assert first(view) == 41
            assert 41 in view and not (43 in view)
            assert str(view) == "[41, 42]"
            assert f"bytes {view}" == "bytes [41, 42]"
            snapshot = Bytes(view)
            assert snapshot[0] == 41 and snapshot[1] == 42
            callback: Callable[[Slice[u8]], i64] = sum_bytes
            assert callback(view) == 83
            with sibling = owner.borrow(0, 8):
                assert sibling[3] == 42
                with child = view.borrow(0, 1):
                    assert child[0] == 41 and view[1] == 42
            # Failed acquisition must neither leak a descriptor nor install
            # a phantom child that prevents a subsequent successful borrow.
            failed = False
            try:
                with invalid = view.borrow(0, 3):
                    pass
            except IndexError:
                failed = True
            assert failed and view[0] == 41

        with mutable = owner.borrow_mut(1, 5):
            mutable[0] = 7
            mutable[0] += 2
            assert mutable[0] == 9
            assert 9 in mutable
            delegate(mutable)
            updater: Callable[[MutSlice[u8], u8], None] = increment
            updater(mutable, u8(1))
            assert mutable[0] == 12
            mutable[0] = 9
            assert len(owner) == 8
            with child = mutable.borrow_mut(1, 3):
                child[0] = 51
                child[1] = 52
                assert len(mutable) == 4
                with reader = child.borrow(0, 2):
                    assert sum_bytes(reader) == 103
            assert mutable[1] == 51 and mutable[2] == 52
            with first_view = mutable.borrow(0, 1):
                with second_view = mutable.borrow(1, 3):
                    assert first_view[0] == 9 and sum_bytes(second_view) == 103
            values = [i64(byte) for byte in mutable]
            assert len(values) == 4 and values[0] == 9
            total = 0
            for byte in mutable:
                total += i64(byte)
            assert total == 112
        assert owner[1] == 9 and owner[2] == 51 and owner[3] == 52
        assert snapshot[0] == 41 and snapshot[1] == 42

        # Exception cleanup restores the parent's permission inside an outer
        # scope; return/break/continue also release each nested record.
        caught = False
        try:
            with parent = owner.borrow_mut(0, 4):
                with child = parent.borrow(0, 4):
                    raise ValueError("drop views")
        except ValueError:
            owner[0] = 13
            caught = True
        assert caught and owner[0] == 13
        for iteration in range(4):
            with view = owner.borrow_mut(0, 1):
                view[0] = u8(iteration)
                if iteration < 2:
                    continue
                break
        assert owner[0] == 2
        with empty = owner.borrow(8, 8):
            assert len(empty) == 0 and sum_bytes(empty) == 0
        with moved = owner.move():
            assert moved[3] == 52
    assert returned_value() == 19
    caught = False
    try:
        raise_from_view()
    except IndexError:
        caught = True
    assert caught
    print("native region borrows ok")
