# aether: 3.0
# These scopes exercise native cleanup, including edges that skip the end of
# the source block. The host gate also counts real allocations and releases.
events: List[i64] = []

def index() -> i64:
    events.append(1)
    return 0

def value() -> i64:
    events.append(2)
    gc_collect()
    return 3

def return_from_owner() -> i64:
    with owner = region(1):
        owner[0] = 17
        return owner[0]

def return_from_move() -> i64:
    with owner = region(1):
        with moved = owner.move():
            moved[0] = 23
            return moved[0]

def throw_from_move() -> None:
    with owner = region(8):
        with moved = owner.move():
            moved[7] = 88
            raise ValueError("release moved owner")

def conditional_move(take: bool) -> i64:
    with owner = region(2):
        owner[0] = 41
        if take:
            with moved = owner.move():
                return moved[0]
        else:
            return owner[0]
    return 0

def loop_exits() -> None:
    with outer = region(2):
        for attempt in range(3):
            with moved = outer.move():
                moved[1] = 29
                break
    for attempt in range(5):
        with owner = region(4):
            with moved = owner.move():
                moved[attempt % 4] = attempt
                if attempt < 3:
                    continue
                break
    with outer = region(1):
        while len(outer) == 1:
            with moved = outer.move():
                assert len(moved) == 1
                break

def main() -> None:
    before = gc_live_bytes()
    with empty = region(0):
        assert len(empty) == 0
    with owner = region(8):
        assert gc_live_bytes() == before
        for offset in range(8):
            assert owner[offset] == 0
        owner[-1] = 255
        owner[0] = 42
        owner[0] += 1
        assert owner[0] == 43 and owner[1] == 0 and owner[-1] == 255
        gc_collect()
        assert owner[-1] == 255

        # Ordinary assignment evaluates value then index; compound assignment
        # captures and reads its target first. Each index runs exactly once.
        owner[index()] = value()
        assert len(events) == 2 and events[0] == 2 and events[1] == 1
        owner[index()] += value()
        assert len(events) == 4 and events[2] == 1 and events[3] == 2
        assert owner[0] == 6
        owner[1] = 9
        assert owner[1] == 9

        errors = 0
        try:
            owner[8] = 0
        except IndexError:
            errors += 1
        try:
            owner[-9]
        except IndexError:
            errors += 1
        try:
            owner[1] = 256
        except ValueError:
            errors += 1
        try:
            owner[1] = -1
        except ValueError:
            errors += 1
        try:
            owner[-1] += 1
        except ValueError:
            errors += 1
        assert errors == 5 and owner[1] == 9 and owner[-1] == 255

        # A failed nested acquisition must leave the enclosing owner live.
        try:
            with bad = region(-1):
                pass
        except ValueError:
            assert owner[0] == 6
        with owner = owner.move():
            assert len(owner) == 8 and owner[0] == 6

    assert return_from_owner() == 17
    assert return_from_move() == 23
    assert conditional_move(True) == 41
    assert conditional_move(False) == 41
    loop_exits()
    caught = False
    try:
        throw_from_move()
    except ValueError as error:
        assert error.message == "release moved owner"
        caught = True
    assert caught
    print("native region ownership ok")
