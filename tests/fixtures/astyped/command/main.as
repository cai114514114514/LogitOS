# aether: 3.0
# Commands stay inert until wait/out/with. Use real echo/cat programs so
# matching output requires bytes to cross native process and pipe boundaries.
def leave(command: Command) -> i64:
    with process = command.start():
        assert process.pid() > 0
        gc_collect()
        return 7

def main() -> None:
    directory = args()[1]
    child = args()[2]
    command = run("echo", "中文 words")
    assert command.pid() == -1 and command.status() == -1
    values = command.argv()
    assert len(values) == 2 and values[0] == "echo" and values[1] == "中文 words"
    gc_collect()
    assert command.out() == "中文 words\n"
    assert command.wait() == 0
    assert command.status() == 0 and command.pid() > 0

    stages = [run("echo", "through pipe"), run(["cat"])]
    pipeline = stages[0] |> stages[1]
    gc_collect()
    assert pipeline.out() == "through pipe\n"
    assert pipeline.wait() == 0

    output = directory + "/command-output.txt"
    run("echo", "redirected") -> output
    assert file_read(output).decode() == "redirected\n"
    input_command = run("cat") <- output
    assert input_command.out() == "redirected\n"
    copy = directory + "/command-copy.txt"
    run("cat") <- output -> copy
    assert file_read(copy) == file_read(output)

    # Both spellings start at scope entry and wait at every scope exit.
    task = run("cat") <- output -> copy
    assert leave(task) == 7
    assert task.status() == 0
    assert task.wait() == 0
    for index in range(3):
        task = run("cat") <- output -> copy
        with process = task:
            if index == 0:
                continue
            assert process.wait() == 0
    caught = False
    task = run("cat") <- output -> copy
    try:
        with process = task:
            raise ValueError("leave running child")
    except ValueError:
        caught = True
    assert caught and task.status() == 0 and task.wait() == 0

    # Input lists are copied; editing the caller list cannot change a command.
    arguments = ["echo", "snapshot"]
    command = run(arguments)
    arguments[1] = "changed"
    assert command.out() == "snapshot\n"
    assert run("/no-such-native-command").wait() == 127
    command = run(child, "arguments", "", "two words", "prefix中文suffix".slice(6, 12))
    assert command.out() == "arguments ok\n" and command.wait() == 7
    command = run(child, "emit")
    assert command.out() == "x" * 200000 and command.status() == 9
    pipeline = run(child, "exit", "7") |> run(child, "exit", "9")
    assert pipeline.wait() == 9
    print("native commands ok")
