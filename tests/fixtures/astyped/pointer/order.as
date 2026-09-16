# aether: 3.0
# Pointer/index expressions must run once. The right side may replace the
# captured pointer, but a compound assignment still updates its original place.
storage = buffer(16)
trace: List[str] = []


def place() -> Ptr[i32]:
    trace.append("place")
    unsafe:
        return i32ptr(addr(storage))


def index() -> i64:
    trace.append("index")
    return 0


def value() -> i32:
    trace.append("value")
    gc_collect()
    return 7


def main() -> None:
    global trace
    unsafe:
        place()[index()] = value()
        assert str(trace) == "['value', 'place', 'index']"
        trace = []
        place()[index()] += value()
        assert str(trace) == "['place', 'index', 'value']"
        pointer = place()
        assert pointer[0] == 14
        # Re-evaluation of the pointer after the RHS would update the second
        # word instead. A closure supplies an observable reassignment.
        def redirect() -> i32:
            unsafe:
                pointer = i32ptr(addr(storage) + u64(4))
            return 3
        pointer[0] += redirect()
        assert pointer[0] == 0
        assert place()[0] == 17
    print("native pointer evaluation order ok")
