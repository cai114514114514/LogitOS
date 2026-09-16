# aether: 3.0
# This initializer is shared by both import paths in the diamond graph.
visits: i64 = 0
names: List[str] = ["global " + "root"]
message = "模块" + "状态"

def touch() -> i64:
    global visits
    visits += 1
    return visits

def replace_names() -> None:
    global names
    names = ["replacement " + "root"]

def loop_binding() -> None:
    global visits
    for visits in range(3):
        pass

def generic_touch[T: Number](value: T) -> T:
    global visits
    visits += 1
    return value
