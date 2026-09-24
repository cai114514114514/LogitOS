#ifndef SHEBANG_H
#define SHEBANG_H

/* The `#!` line, parsed. Pure and freestanding so make test-shebang can drive
 * it on the host; proc_execve (exec.c) is its one caller.
 *
 * Until this existed execve refused every script with "bad aex header", so a
 * program that worked by `as script.as` could not be run as `./script.as`, and
 * nothing that expects `#!` -- a tool that writes a helper script and runs it,
 * a script on $PATH -- worked on this machine at all.
 *
 * The rules are Linux's (fs/binfmt_script.c), because that is the behaviour a
 * script written elsewhere was written against:
 *
 *   - the first two bytes are `#!`; blanks after them are skipped;
 *   - the interpreter is the next blank-delimited word;
 *   - EVERYTHING after it on the line, trimmed, is ONE optional argument --
 *     `#!/bin/as -x -y` passes the single string "-x -y", not two;
 *   - the line ends at '\n' or at the end of the file;
 *   - a line that does not end within SHEBANG_MAX bytes is REFUSED, never
 *     truncated: silently cutting an interpreter path would run a different
 *     program.
 *
 * One level only, and that is exec.c's to enforce: the interpreter must itself
 * be a binary. */

#define SHEBANG_MAX 128          /* bytes of the file the parser looks at */

#define SHEBANG_NOT     0        /* not a script: load it as a binary     */
#define SHEBANG_OK      1
#define SHEBANG_TOOLONG (-1)     /* no line end within SHEBANG_MAX        */
#define SHEBANG_EMPTY   (-2)     /* `#!` with no interpreter              */

/* `buf` holds the first `n` bytes of the file (n <= SHEBANG_MAX) and has room
 * for a terminator at buf[n]. It is modified in place; *interp and *arg point
 * into it. *arg is 0 when there is none. `whole` says whether those n bytes
 * are the entire file, which is what lets a line end at EOF. */
static inline int shebang_parse(char *buf, int n, int whole, char **interp, char **arg)
{
    *interp = 0; *arg = 0;
    if (n < 2 || buf[0] != '#' || buf[1] != '!') return SHEBANG_NOT;
    int end = -1;
    for (int i = 2; i < n; i++) if (buf[i] == '\n') { end = i; break; }
    if (end < 0) {
        if (!whole) return SHEBANG_TOOLONG;
        end = n;
    }
    buf[end] = 0;
    /* Trim the line's tail first -- blanks and a CR (a file saved with CRLF
     * endings) -- so neither the interpreter nor the argument can end in
     * one: the interpreter of "#!/bin/as\r\n" is "/bin/as". */
    for (int j = end - 1; j >= 2 && (buf[j] == ' ' || buf[j] == '\t' || buf[j] == '\r'); j--)
        buf[j] = 0;
    int i = 2;
    while (buf[i] == ' ' || buf[i] == '\t') i++;
    if (!buf[i]) return SHEBANG_EMPTY;
    *interp = &buf[i];
    while (buf[i] && buf[i] != ' ' && buf[i] != '\t') i++;
    if (!buf[i]) return SHEBANG_OK;
    buf[i++] = 0;
    while (buf[i] == ' ' || buf[i] == '\t') i++;
    if (buf[i]) *arg = &buf[i];
    return SHEBANG_OK;
}

#endif
