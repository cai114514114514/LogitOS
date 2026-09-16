# aether: 3.0
import left
import right
import state
from state import message, visits

error: Error = ValueError("initial")
values: Array[i64, 2] = [3, 4]

def catch_global() -> None:
    global error
    try:
        raise IOError("stored " + "exception")
    except IOError as error:
        pass

def main() -> None:
    assert left.order == 1
    assert right.order == 2
    assert visits == 2
    assert message == "模块状态"
    gc_collect()
    assert state.names[0] == "global root"
    state.names.append("later")
    assert len(state.names) == 2
    state.replace_names()
    gc_collect()
    assert state.names[0] == "replacement root"
    state.loop_binding()
    assert visits == 2
    assert state.generic_touch(3) == 3
    assert state.generic_touch(2.5) == 2.5
    assert visits == 4
    values[0] = 7
    assert values[0] == 7
    catch_global()
    gc_collect()
    assert error.message == "stored exception"
    print("module globals ok")

def test_initializers() -> None:
    assert left.order == 1
    assert right.order == 2
