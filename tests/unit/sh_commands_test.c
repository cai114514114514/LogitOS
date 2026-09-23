/* Exercise the real /bin/sh -c entry point against the same pipe/process model
 * used by sh_edit_test.c. A tokenizer-only test would miss the old failure:
 * main() passed the whole command string to exec_line(), which accepted the
 * first builtin and silently ignored every command after `;` or newline. */
#include "sh_hoststub.h"

#define main sh_main
#include "sh.c"
#undef main

static int checks, failures;

#define CHECK(cond, name) do { \
    checks++; \
    if (!(cond)) { printf("FAIL: %s\n", name); failures++; } \
} while (0)

static void finish_child(int nap_count)
{
    (void)nap_count;
    stub_child_exit_all();
}

static int run_command_string(char *script, const char *name)
{
    char *argv[] = { "sh", "-c", script, (char *)name, 0, 0 };
    int argc = name ? 4 : 3;
    nenv = 0;
    last_status = 0;
    g_arg0 = "sh";
    stub_exit_armed = 1;
    stub_exit_code = -1;
    if (setjmp(stub_exit_jmp) == 0)
        sh_main(argc, argv);
    stub_exit_armed = 0;
    return stub_exit_code;
}

int main(void)
{
    {
        int out = stub_pipe_alloc(), err = stub_pipe_alloc();
        stub_fd_bind(1, out, 1);
        stub_fd_bind(2, err, 1);
    }

    CHECK(run_command_string("export FIRST=one; export SECOND=$FIRST", 0) == 0,
          "semicolon: sh -c exit status");
    CHECK(env_get("FIRST") && !strcmp(env_get("FIRST"), "one"),
          "semicolon: first command value");
    CHECK(env_get("SECOND") && !strcmp(env_get("SECOND"), "one"),
          "semicolon: second command ran after expansion");

    CHECK(run_command_string("export FIRST=one\nexport SECOND=two", 0) == 0,
          "newline: sh -c exit status");
    CHECK(env_get("FIRST") && !strcmp(env_get("FIRST"), "one"),
          "newline: first command value");
    CHECK(env_get("SECOND") && !strcmp(env_get("SECOND"), "two"),
          "newline: second command ran");

    CHECK(run_command_string("export FIRST='a;b'; export SECOND=\"$FIRST\"", 0) == 0,
          "quoted semicolon: sh -c exit status");
    CHECK(env_get("FIRST") && !strcmp(env_get("FIRST"), "a;b"),
          "quoted semicolon stays inside the first value");
    CHECK(env_get("SECOND") && !strcmp(env_get("SECOND"), "a;b"),
          "quoted semicolon does not stop the next command");

    CHECK(run_command_string("export FIRST=a\\;b; export SECOND=$FIRST", 0) == 0,
          "escaped semicolon: sh -c exit status");
    CHECK(env_get("FIRST") && !strcmp(env_get("FIRST"), "a;b"),
          "escaped semicolon stays inside the first value");
    CHECK(env_get("SECOND") && !strcmp(env_get("SECOND"), "a;b"),
          "escaped semicolon does not stop the next command");

    CHECK(run_command_string("export FIRST=\"a\nb\"; export SECOND=$FIRST", 0) == 0,
          "quoted newline: sh -c exit status");
    CHECK(env_get("FIRST") && !strcmp(env_get("FIRST"), "a\nb"),
          "quoted newline stays inside the first value");
    CHECK(env_get("SECOND") && !strcmp(env_get("SECOND"), "a\nb"),
          "quoted newline does not stop the next command");

    CHECK(run_command_string("export NAME=$0; export COPY=$NAME", "named-shell") == 0,
          "named command string exits successfully");
    CHECK(env_get("COPY") && !strcmp(env_get("COPY"), "named-shell"),
          "$0 remains the -c name across sequential commands");

    stub_child_status = 7;
    stub_nap_hook = finish_child;
    CHECK(run_command_string("cat; export STATUS=$?", "named-shell") == 0,
          "foreground command followed by assignment exits with last status");
    stub_nap_hook = 0;
    CHECK(env_get("STATUS") && !strcmp(env_get("STATUS"), "7"),
          "$? in second command sees first command exit status");

    CHECK(run_command_string("export FIRST=one; exit 4; export SECOND=two", 0) == 4,
          "exit stops the remaining command string");
    CHECK(env_get("FIRST") && !strcmp(env_get("FIRST"), "one"),
          "command before exit ran");
    CHECK(!env_get("SECOND"), "command after exit did not run");

    CHECK(run_command_string("export FIRST='unfinished", 0) == 2,
          "unterminated quote is refused");
    CHECK(!env_get("FIRST"), "unterminated quote ran no command");

    char long_command[LINE + 20];
    memcpy(long_command, "export BIG=", 11);
    memset(long_command + 11, 'x', sizeof long_command - 12);
    long_command[sizeof long_command - 1] = 0;
    CHECK(run_command_string(long_command, 0) == 1,
          "overlong command is refused");
    CHECK(!env_get("BIG"), "overlong command ran no prefix");

    printf("sh command strings: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
